// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 Google LLC
 */

#define pr_fmt(fmt) "blk-crypto: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/keyslot-manager.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/siphash.h>
#include <linux/slab.h>

#include "blk-crypto-internal.h"

const struct blk_crypto_mode blk_crypto_modes[] = {
	[BLK_ENCRYPTION_MODE_AES_256_XTS] = {
		.keysize = 64,
		.ivsize = 16,
	},
	[BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV] = {
		.keysize = 16,
		.ivsize = 16,
	},
	[BLK_ENCRYPTION_MODE_ADIANTUM] = {
		.keysize = 32,
		.ivsize = 32,
	},
};

/*
 * This number needs to be at least (the number of threads doing IO
 * concurrently) * (maximum recursive depth of a bio), so that we don't
 * deadlock on crypt_ctx allocations. The default is chosen to be the same
 * as the default number of post read contexts in both EXT4 and F2FS.
 */
static int num_prealloc_crypt_ctxs = 128;

module_param(num_prealloc_crypt_ctxs, int, 0444);
MODULE_PARM_DESC(num_prealloc_crypt_ctxs,
		"Number of bio crypto contexts to preallocate");

static struct kmem_cache *bio_crypt_ctx_cache;
static mempool_t *bio_crypt_ctx_pool;

static int __init bio_crypt_ctx_init(void)
{
	size_t i;

	bio_crypt_ctx_cache = KMEM_CACHE(bio_crypt_ctx, 0);
	if (!bio_crypt_ctx_cache)
		goto out_no_mem;

	bio_crypt_ctx_pool = mempool_create_slab_pool(num_prealloc_crypt_ctxs,
						      bio_crypt_ctx_cache);
	if (!bio_crypt_ctx_pool)
		goto out_no_mem;

	/* This is assumed in various places. */
	BUILD_BUG_ON(BLK_ENCRYPTION_MODE_INVALID != 0);

	/* Sanity check that no algorithm exceeds the defined limits. */
	for (i = 0; i < BLK_ENCRYPTION_MODE_MAX; i++) {
		BUG_ON(blk_crypto_modes[i].keysize > BLK_CRYPTO_MAX_KEY_SIZE);
		BUG_ON(blk_crypto_modes[i].ivsize > BLK_CRYPTO_MAX_IV_SIZE);
	}

	return 0;
out_no_mem:
	panic("Failed to allocate mem for bio crypt ctxs\n");
}
subsys_initcall(bio_crypt_ctx_init);

void bio_crypt_set_ctx(struct bio *bio, const struct blk_crypto_key *key,
		       const u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE], gfp_t gfp_mask)
{
	struct bio_crypt_ctx *bc = mempool_alloc(bio_crypt_ctx_pool, gfp_mask);

	bc->bc_key = key;
	memcpy(bc->bc_dun, dun, sizeof(bc->bc_dun));

	bio->bi_crypt_context = bc;
}

void __bio_crypt_free_ctx(struct bio *bio)
{
	mempool_free(bio->bi_crypt_context, bio_crypt_ctx_pool);
	bio->bi_crypt_context = NULL;
}

void __bio_crypt_clone(struct bio *dst, struct bio *src, gfp_t gfp_mask)
{
	dst->bi_crypt_context = mempool_alloc(bio_crypt_ctx_pool, gfp_mask);
	*dst->bi_crypt_context = *src->bi_crypt_context;
}
EXPORT_SYMBOL_GPL(__bio_crypt_clone);

void bio_crypt_dun_increment(u64 dun[BLK_CRYPTO_DUN_ARRAY_SIZE],
			     unsigned int inc)
{
	int i = 0;

	while (inc && i < BLK_CRYPTO_DUN_ARRAY_SIZE) {
		dun[i] += inc;
		inc = (dun[i] < inc);
		i++;
	}
}

void __bio_crypt_advance(struct bio *bio, unsigned int bytes)
{
	struct bio_crypt_ctx *bc = bio->bi_crypt_context;

	bio_crypt_dun_increment(bc->bc_dun,
				bytes >> bc->bc_key->data_unit_size_bits);
}

/*
 * Returns true if @bc_dun plus @bytes converted to data units is equal to
 * @next_dun, treating the DUNs as multi-limb integers.
 */
bool bio_crypt_dun_is_contiguous(const struct bio_crypt_ctx *bc,
				 unsigned int bytes,
				 const u64 next_dun[BLK_CRYPTO_DUN_ARRAY_SIZE])
{
	int i = 0;
	unsigned int carry = bytes >> bc->bc_key->data_unit_size_bits;

	while (i < BLK_CRYPTO_DUN_ARRAY_SIZE) {
		if (bc->bc_dun[i] + carry != next_dun[i])
			return false;
		/*
		 * If the addition in this limb overflowed, then we need to
		 * carry 1 into the next limb. Else the carry is 0.
		 */
		if ((bc->bc_dun[i] + carry) < carry)
			carry = 1;
		else
			carry = 0;
		i++;
	}

	/* If the DUN wrapped through 0, don't treat it as contiguous. */
	return carry == 0;
}

/*
 * Checks that two bio crypt contexts are compatible - i.e. that
 * they are mergeable except for data_unit_num continuity.
 */
static bool bio_crypt_ctx_compatible(struct bio_crypt_ctx *bc1,
				     struct bio_crypt_ctx *bc2)
{
	if (!bc1)
		return !bc2;

	return bc2 && bc1->bc_key == bc2->bc_key;
}

bool bio_crypt_rq_ctx_compatible(struct request *rq, struct bio *bio)
{
	return bio_crypt_ctx_compatible(rq->crypt_ctx, bio->bi_crypt_context);
}

/*
 * Checks that two bio crypt contexts are compatible, and also
 * that their data_unit_nums are continuous (and can hence be merged)
 * in the order b_1 followed by b_2.
 */
bool bio_crypt_ctx_mergeable(struct bio_crypt_ctx *bc1, unsigned int bc1_bytes,
			     struct bio_crypt_ctx *bc2)
{
	if (!bio_crypt_ctx_compatible(bc1, bc2))
		return false;

	return !bc1 || bio_crypt_dun_is_contiguous(bc1, bc1_bytes, bc2->bc_dun);
}

/*
 * Check that all I/O segments are data unit aligned, and set bio->bi_status
 * on error.
 */
static bool bio_crypt_check_alignment(struct bio *bio)
{
	const unsigned int data_unit_size =
		bio->bi_crypt_context->bc_key->crypto_cfg.data_unit_size;
	struct bvec_iter iter;
	struct bio_vec bv;

	bio_for_each_segment(bv, bio, iter) {
		if (!IS_ALIGNED(bv.bv_len | bv.bv_offset, data_unit_size)) {
			bio->bi_status = BLK_STS_IOERR;
			return false;
		}
	}

	return true;
}

blk_status_t __blk_crypto_init_request(struct request *rq)
{
	return blk_ksm_get_slot_for_key(rq->q->ksm, rq->crypt_ctx->bc_key,
					&rq->crypt_keyslot);
}

/**
 * __blk_crypto_free_request - Uninitialize the crypto fields of a request.
 *
 * @rq: The request whose crypto fields to uninitialize.
 *
 * Completely uninitializes the crypto fields of a request. If a keyslot has
 * been programmed into some inline encryption hardware, that keyslot is
 * released. The rq->crypt_ctx is also freed.
 */
void __blk_crypto_free_request(struct request *rq)
{
	blk_ksm_put_slot(rq->crypt_keyslot);
	mempool_free(rq->crypt_ctx, bio_crypt_ctx_pool);
	blk_crypto_rq_set_defaults(rq);
}

/**
 * __blk_crypto_bio_prep - Prepare bio for inline encryption
 *
 * @bio_ptr: pointer to original bio pointer
 *
 * Succeeds if the bio doesn't have inline encryption enabled or if the bio
 * crypt context provided for the bio is supported by the underlying device's
 * inline encryption hardware. Ends the bio with error otherwise.
 *
 * Caller must ensure bio has bio_crypt_ctx.
 *
 * Return: true on success; false on error (and bio->bi_status will be set
 *	   appropriately, and bio_endio() will have been called so bio
 *	   submission should abort).
 */
bool __blk_crypto_bio_prep(struct bio **bio_ptr)
{
	struct bio *bio = *bio_ptr;

	/* Error if bio has no data. */
	if (WARN_ON_ONCE(!bio_has_data(bio)))
		goto fail;

	if (!bio_crypt_check_alignment(bio))
		goto fail;

	/*
	 * Success if device supports the encryption context, and blk-integrity
	 * isn't supported by device/is turned off.
	 */
	if (!blk_ksm_crypto_cfg_supported(bio->bi_disk->queue->ksm,
				&bio->bi_crypt_context->bc_key->crypto_cfg)) {
		bio->bi_status = BLK_STS_NOTSUPP;
		goto fail;
	}

	return true;
fail:
	bio_endio(*bio_ptr);
	return false;
}

/**
 * __blk_crypto_rq_bio_prep - Prepare a request when its first bio is inserted
 *
 * @rq: The request to prepare
 * @bio: The first bio being inserted into the request
 *
 * Frees the bio crypt context in the request's old rq->crypt_ctx, if any, and
 * moves the bio crypt context of the bio into the request's rq->crypt_ctx.
 */
void __blk_crypto_rq_bio_prep(struct request *rq, struct bio *bio)
{
	mempool_free(rq->crypt_ctx, bio_crypt_ctx_pool);
	rq->crypt_ctx = bio->bi_crypt_context;
	bio->bi_crypt_context = NULL;
}

void __blk_crypto_rq_prep_clone(struct request *dst, struct request *src)
{
	dst->crypt_ctx = src->crypt_ctx;
}

/**
 * __blk_crypto_insert_cloned_request - Prepare a cloned request to be inserted
 *					into a request queue.
 * @rq: the request being queued
 *
 * Return: BLK_STS_OK on success, nonzero on error.
 */
blk_status_t __blk_crypto_insert_cloned_request(struct request *rq)
{
	return blk_crypto_init_request(rq);
}

/**
 * blk_crypto_init_key() - Prepare a key for use with blk-crypto
 * @blk_key: Pointer to the blk_crypto_key to initialize.
 * @raw_key: Pointer to the raw key. Must be the correct length for the chosen
 *	     @crypto_mode; see blk_crypto_modes[].
 * @crypto_mode: identifier for the encryption algorithm to use
 * @dun_bytes: number of bytes that will be used to specify the DUN when this
 *	       key is used
 * @data_unit_size: the data unit size to use for en/decryption
 *
 * Return: 0 on success, -errno on failure.  The caller is responsible for
 *	   zeroizing both blk_key and raw_key when done with them.
 */
int blk_crypto_init_key(struct blk_crypto_key *blk_key, const u8 *raw_key,
			enum blk_crypto_mode_num crypto_mode,
			unsigned int dun_bytes,
			unsigned int data_unit_size)
{
	const struct blk_crypto_mode *mode;
	static siphash_key_t hash_key;

	memset(blk_key, 0, sizeof(*blk_key));

	if (crypto_mode >= ARRAY_SIZE(blk_crypto_modes))
		return -EINVAL;

	mode = &blk_crypto_modes[crypto_mode];
	if (mode->keysize == 0)
		return -EINVAL;

	if (!is_power_of_2(data_unit_size))
		return -EINVAL;

	blk_key->crypto_cfg.crypto_mode = crypto_mode;
	blk_key->crypto_cfg.dun_bytes = dun_bytes;
	blk_key->crypto_cfg.data_unit_size = data_unit_size;
	blk_key->data_unit_size_bits = ilog2(data_unit_size);
	blk_key->size = mode->keysize;
	memcpy(blk_key->raw, raw_key, mode->keysize);

	/*
	 * The keyslot manager uses the SipHash of the key to implement O(1) key
	 * lookups while avoiding leaking information about the keys.  It's
	 * precomputed here so that it only needs to be computed once per key.
	 */
	get_random_once(&hash_key, sizeof(hash_key));
	blk_key->hash = siphash(raw_key, mode->keysize, &hash_key);

	return 0;
}

bool blk_crypto_config_supported(struct request_queue *q,
				 const struct blk_crypto_config *cfg)
{
	return blk_ksm_crypto_cfg_supported(q->ksm, cfg);
}

/**
 * blk_crypto_start_using_key() - Start using a blk_crypto_key on a device
 * @key: A key to use on the device
 * @q: the request queue for the device
 *
 * Upper layers must call this function to ensure that the hardware supports
 * the key's crypto settings.
 *
 * Return: 0 on success; -ENOPKG if the hardware doesn't support the key
 */
int blk_crypto_start_using_key(const struct blk_crypto_key *key,
			       struct request_queue *q)
{
	if (blk_ksm_crypto_cfg_supported(q->ksm, &key->crypto_cfg))
		return 0;
	return -ENOPKG;
}
EXPORT_SYMBOL_GPL(blk_crypto_start_using_key);

/**
 * blk_crypto_evict_key() - Evict a key from any inline encryption hardware
 *			    it may have been programmed into
 * @q: The request queue who's associated inline encryption hardware this key
 *     might have been programmed into
 * @key: The key to evict
 *
 * Upper layers (filesystems) should call this function to ensure that a key
 * is evicted from hardware that it might have been programmed into. This
 * will call blk_ksm_evict_key on the queue's keyslot manager, if one
 * exists, and supports the crypto algorithm with the specified data unit size.
 *
 * Return: 0 on success or if key is not present in the q's ksm, -err on error.
 */
int blk_crypto_evict_key(struct request_queue *q,
			 const struct blk_crypto_key *key)
{
	if (q->ksm && blk_ksm_crypto_cfg_supported(q->ksm, &key->crypto_cfg))
		return blk_ksm_evict_key(q->ksm, key);

	return 0;
}
