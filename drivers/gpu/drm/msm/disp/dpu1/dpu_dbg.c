// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2009-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__

#include "dpu_dbg.h"
#include "dpu_hw_catalog.h"

/* global dpu debug base structure */
static struct dpu_dbg_base dpu_dbg;


#ifdef CONFIG_DEV_COREDUMP
static ssize_t dpu_devcoredump_read(char *buffer, loff_t offset,
		size_t count, void *data, size_t datalen)
{
	struct drm_print_iterator iter;
	struct drm_printer p;

	iter.data = buffer;
	iter.offset = 0;
	iter.start = offset;
	iter.remain = count;

	p = drm_coredump_printer(&iter);

	drm_printf(&p, "---\n");

	drm_printf(&p, "module: " KBUILD_MODNAME "\n");
	drm_printf(&p, "dpu devcoredump\n");
	drm_printf(&p, "timestamp %lld\n", ktime_to_ns(dpu_dbg.timestamp));

	dpu_dbg.dpu_dbg_printer = &p;
	dpu_dbg.enable_reg_dump = DPU_DBG_DUMP_IN_COREDUMP;

	drm_printf(&p, "===================dpu regs================\n");

	_dpu_dump_array(&dpu_dbg, dpu_dbg.req_dump_blks,
		ARRAY_SIZE(dpu_dbg.req_dump_blks),
		dpu_dbg.work_panic, "evtlog_workitem",
		dpu_dbg.dump_all);

	drm_printf(&p, "===================dpu drm state================\n");

	if (dpu_dbg.atomic_state)
		drm_atomic_print_new_state(dpu_dbg.atomic_state,
				&p);

	return count - iter.remain;
}

static void dpu_devcoredump_free(void *data)
{
	if (dpu_dbg.atomic_state) {
		drm_atomic_state_put(dpu_dbg.atomic_state);
		dpu_dbg.atomic_state = NULL;
	}
	dpu_dbg.coredump_pending = false;
}

static void dpu_devcoredump_capture_state(void)
{
	struct drm_device *ddev;
	struct drm_modeset_acquire_ctx ctx;

	dpu_dbg.timestamp = ktime_get();

	ddev = dpu_dbg.drm_dev;

	drm_modeset_acquire_init(&ctx, 0);

	while (drm_modeset_lock_all_ctx(ddev, &ctx) != 0)
		drm_modeset_backoff(&ctx);

	dpu_dbg.atomic_state = drm_atomic_helper_duplicate_state(ddev,
			&ctx);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}
#else
static void dpu_devcoredump_capture_state(void)
{
}
#endif /* CONFIG_DEV_COREDUMP */

/**
 * _dpu_dump_work - deferred dump work function
 * @work: work structure
 */
static void _dpu_dump_work(struct kthread_work *work)
{
	/* reset the enable_reg_dump to default before every dump */
	dpu_dbg.enable_reg_dump = DEFAULT_REGDUMP;

	//_dpu_dump_array(&dpu_dbg, dpu_dbg.req_dump_blks,
	//	ARRAY_SIZE(dpu_dbg.req_dump_blks),
	//	dpu_dbg.work_panic, "evtlog_workitem",
	//	dpu_dbg.dump_all);

	dpu_devcoredump_capture_state();

#ifdef CONFIG_DEV_COREDUMP
	if (dpu_dbg.enable_reg_dump & DPU_DBG_DUMP_IN_MEM) {
		dev_coredumpm(dpu_dbg.dev, THIS_MODULE, &dpu_dbg, 0, GFP_KERNEL,
				dpu_devcoredump_read, dpu_devcoredump_free);
		dpu_dbg.coredump_pending = true;
	}
#endif
}

static int dpu_debugfs_set(void *data, u64 value)
{
	if (value == 42) {
		pr_err("VK: ***** DUMPING ****** \n");
		_dpu_dump_array(&dpu_dbg, dpu_dbg.req_dump_blks,
			ARRAY_SIZE(dpu_dbg.req_dump_blks),
			false, "42isans", true);
	} else
		pr_err("VK: wrong answer to life\n");

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(dpu_debugfs_ops, NULL, dpu_debugfs_set, "%llu\n");

void dpu_dbg_dump(enum dpu_dbg_dump_context dump_mode, const char *name, ...)
{
	int i, index = 0;
	bool do_panic = false;
	bool dump_all = false;
	va_list args;
	char *blk_name = NULL;
	struct dpu_dbg_reg_base *blk_base = NULL;
	struct dpu_dbg_reg_base **blk_arr;
	u32 blk_len;

	/*
	 * if there is a coredump pending return immediately till dump
	 * if read by userspace or timeout happens
	 */
	if (((dpu_dbg.enable_reg_dump == DPU_DBG_DUMP_IN_MEM) ||
		 (dpu_dbg.enable_reg_dump == DPU_DBG_DUMP_IN_COREDUMP)) &&
		dpu_dbg.coredump_pending) {
		pr_debug("coredump is pending read\n");
		return;
	}

	blk_arr = &dpu_dbg.req_dump_blks[0];
	blk_len = ARRAY_SIZE(dpu_dbg.req_dump_blks);

	memset(dpu_dbg.req_dump_blks, 0,
			sizeof(dpu_dbg.req_dump_blks));
	dpu_dbg.dump_all = false;
	dpu_dbg.dump_mode = dump_mode;

	va_start(args, name);
	i = 0;
	while ((blk_name = va_arg(args, char*))) {

		if (IS_ERR_OR_NULL(blk_name))
			break;

		blk_base = _dpu_dump_get_blk_addr(&dpu_dbg, blk_name);
		if (blk_base) {
			if (index < blk_len) {
				blk_arr[index] = blk_base;
				index++;
			} else {
				pr_err("insufficient space to dump %s\n",
						blk_name);
			}
		}

		if (!strcmp(blk_name, "all"))
			dump_all = true;

		if (!strcmp(blk_name, "panic"))
			do_panic = true;

	}
	va_end(args);

	dpu_dbg.work_panic = do_panic;
	dpu_dbg.dump_all = dump_all;

	kthread_queue_work(dpu_dbg.dump_worker,
			&dpu_dbg.dump_work);

}

int dpu_dbg_init(struct device *dev)
{
	if (!dev) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	mutex_init(&dpu_dbg.mutex);
	INIT_LIST_HEAD(&dpu_dbg.reg_base_list);
	dpu_dbg.dev = dev;

	dpu_dbg.work_panic = false;
	dpu_dbg.enable_reg_dump = DEFAULT_REGDUMP;

	dpu_dbg.dump_worker = kthread_create_worker(0, "%s", "dpu_dbg");
	if (IS_ERR(dpu_dbg.dump_worker))
		dev_err(dev, "failed to create dpu dbg task\n");

	kthread_init_work(&dpu_dbg.dump_work, _dpu_dump_work);

	pr_info("dump:%d\n", dpu_dbg.enable_reg_dump);

	pr_err("VK: Creating debugfs\n");
	debugfs_create_file("DPU-DUMP", 0600, NULL, NULL, &dpu_debugfs_ops);
	return 0;
}

void dpu_dbg_register_drm_dev(struct drm_device *ddev)
{
	dpu_dbg.drm_dev = ddev;
}

static void dpu_dbg_reg_base_destroy(void)
{
	struct dpu_dbg_reg_range *range_node, *range_tmp;
	struct dpu_dbg_reg_base *blk_base, *blk_tmp;
	struct dpu_dbg_base *dbg_base = &dpu_dbg;

	/* if the dbg init failed or was never called */
	if (!dbg_base || !dpu_dbg.dev)
		return;

	list_for_each_entry_safe(blk_base, blk_tmp, &dbg_base->reg_base_list,
							reg_base_head) {
		list_for_each_entry_safe(range_node, range_tmp,
				&blk_base->sub_range_list, head) {
			list_del(&range_node->head);
			kfree(range_node);
		}
		list_del(&blk_base->reg_base_head);
		kfree(blk_base);
	}
}

/**
 * dpu_dbg_destroy - destroy dpu debug facilities
 */
void dpu_dbg_destroy(void)
{
	if (dpu_dbg.dump_worker)
		kthread_destroy_worker(dpu_dbg.dump_worker);
	dpu_dbg_reg_base_destroy();
	mutex_destroy(&dpu_dbg.mutex);
}

int dpu_dbg_reg_register_base(const char *name, void __iomem *base,
		size_t max_offset)
{
	struct dpu_dbg_base *dbg_base = &dpu_dbg;
	struct dpu_dbg_reg_base *reg_base;

	if (!name || !strlen(name)) {
		pr_err("no debug name provided\n");
		return -EINVAL;
	}

	reg_base = kzalloc(sizeof(*reg_base), GFP_KERNEL);
	if (!reg_base)
		return -ENOMEM;

	strlcpy(reg_base->name, name, sizeof(reg_base->name));
	reg_base->base = base;
	reg_base->max_offset = max_offset;
	reg_base->off = 0;
	reg_base->cnt = DEFAULT_BASE_REG_CNT;
	reg_base->reg_dump = NULL;

	/* Initialize list to make sure check for null list will be valid */
	INIT_LIST_HEAD(&reg_base->sub_range_list);

	pr_info("%s base: %pK max_offset 0x%zX\n", reg_base->name,
			reg_base->base, reg_base->max_offset);

	list_add(&reg_base->reg_base_head, &dbg_base->reg_base_list);

	return 0;
}

void dpu_dbg_reg_register_dump_range(const char *base_name,
		const char *range_name, u32 offset_start, u32 offset_end,
		uint32_t xin_id)
{
	struct dpu_dbg_reg_base *reg_base;
	struct dpu_dbg_reg_range *range;

	reg_base = _dpu_dump_get_blk_addr(&dpu_dbg, base_name);
	if (!reg_base) {
		pr_err("error: for range %s unable to locate base %s\n",
				range_name, base_name);
		return;
	}

	if (!range_name || strlen(range_name) == 0) {
		pr_err("%pS: bad range name, base_name %s, offset_start 0x%X, end 0x%X\n",
				__builtin_return_address(0), base_name,
				offset_start, offset_end);
		return;
	}

	if (offset_end - offset_start < REG_DUMP_ALIGN ||
			offset_start > offset_end) {
		pr_err("%pS: bad range, base_name %s, range_name %s, offset_start 0x%X, end 0x%X\n",
				__builtin_return_address(0), base_name,
				range_name, offset_start, offset_end);
		return;
	}

	range = kzalloc(sizeof(*range), GFP_KERNEL);
	if (!range)
		return;

	strlcpy(range->range_name, range_name, sizeof(range->range_name));
	range->offset.start = offset_start;
	range->offset.end = offset_end;
	range->xin_id = xin_id;
	list_add_tail(&range->head, &reg_base->sub_range_list);

	pr_info("base_name %s, range_name %s, start 0x%X, end 0x%X\n",
			base_name, range->range_name,
			range->offset.start, range->offset.end);
}
