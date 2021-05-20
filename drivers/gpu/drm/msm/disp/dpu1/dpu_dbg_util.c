// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__

#include "dpu_dbg.h"
#include "dpu_hw_catalog.h"

/**
 * _sde_power_check - check if power needs to enabled
 * @dump_mode: to check if power need to be enabled
 * Return: true if success; false otherwise
 */
static inline bool _dpu_power_check(enum dpu_dbg_dump_context dump_mode)
{
	return (dump_mode == DPU_DBG_DUMP_CLK_ENABLED_CTX) ? false : true;
}

/**
 * _dpu_dump_reg - helper function for dumping rotator register set content
 * @dump_name: register set name
 * @reg_dump_flag: dumping flag controlling in-log/memory dump location
 * @base_addr: starting address of io region for calculating offsets to print
 * @addr: starting address offset for dumping
 * @len_bytes: range of the register set
 * @dump_mem: output buffer for memory dump location option
 * @from_isr: whether being called from isr context
 */
static void _dpu_dump_reg(struct dpu_dbg_base *dbg_base,
		const char *dump_name, u32 reg_dump_flag,
		void __iomem *base_addr, void __iomem *addr,
		size_t len_bytes, u32 **dump_mem)
{
	u32 in_log, in_mem, len_align, len_padded, in_dump;
	u32 *dump_addr = NULL;
	void __iomem *end_addr;
	int i;
	int rc;

	if (!len_bytes)
		return;

	in_log = (reg_dump_flag & DPU_DBG_DUMP_IN_LOG);
	in_mem = (reg_dump_flag & DPU_DBG_DUMP_IN_MEM);
	in_dump = (reg_dump_flag & DPU_DBG_DUMP_IN_COREDUMP);

	pr_debug("%s: reg_dump_flag=%d in_log=%d in_mem=%d\n",
		dump_name, reg_dump_flag, in_log, in_mem);

	if (!in_log && !in_mem && !in_dump)
		return;

	if (in_log)
		dev_info(dbg_base->dev, "%s: start_offset 0x%lx len 0x%zx\n",
				dump_name, (unsigned long)(addr - base_addr),
					len_bytes);

	len_align = (len_bytes + REG_DUMP_ALIGN - 1) / REG_DUMP_ALIGN;
	len_padded = len_align * REG_DUMP_ALIGN;
	end_addr = addr + len_bytes;

	if (in_mem || in_dump) {
		if (dump_mem && !(*dump_mem))
			*dump_mem = devm_kzalloc(dbg_base->dev, len_padded,
					GFP_KERNEL);

		if (dump_mem && *dump_mem) {
			dump_addr = *dump_mem;
			dev_info(dbg_base->dev,
				"%s: start_addr:0x%pK len:0x%x reg_offset=0x%lx\n",
				dump_name, dump_addr, len_padded,
				(unsigned long)(addr - base_addr));
			if (in_dump)
				drm_printf(dbg_base->dpu_dbg_printer,
						"%s: start_addr:0x%pK len:0x%x reg_offset=0x%lx\n",
						dump_name, dump_addr,
						len_padded,
						(unsigned long)(addr -
						base_addr));
		} else {
			in_mem = 0;
			pr_err("dump_mem: kzalloc fails!\n");
		}
	}

	if (_dpu_power_check(dbg_base->dump_mode)) {
		rc = pm_runtime_get_sync(dbg_base->dev);
		if (rc < 0) {
			pr_err("failed to enable power %d\n", rc);
			return;
		}
	}

	for (i = 0; i < len_align; i++) {
		u32 x0, x4, x8, xc;

		if (in_log || in_mem) {
			x0 = (addr < end_addr) ? readl_relaxed(addr + 0x0) : 0;
			x4 = (addr + 0x4 < end_addr) ? readl_relaxed(addr +
					0x4) : 0;
			x8 = (addr + 0x8 < end_addr) ? readl_relaxed(addr +
					0x8) : 0;
			xc = (addr + 0xc < end_addr) ? readl_relaxed(addr +
					0xc) : 0;
		}

		if (in_log)
			dev_info(dbg_base->dev,
					"0x%lx : %08x %08x %08x %08x\n",
					(unsigned long)(addr - base_addr),
					x0, x4, x8, xc);

		if (dump_addr && in_mem) {
			dump_addr[i * 4] = x0;
			dump_addr[i * 4 + 1] = x4;
			dump_addr[i * 4 + 2] = x8;
			dump_addr[i * 4 + 3] = xc;
		}

		if (in_dump) {
			drm_printf(dbg_base->dpu_dbg_printer,
					"0x%lx : %08x %08x %08x %08x\n",
					(unsigned long)(addr - base_addr),
					dump_addr[i * 4],
					dump_addr[i * 4 + 1],
					dump_addr[i * 4 + 2],
					dump_addr[i * 4 + 3]);

		}

		addr += REG_DUMP_ALIGN;
	}

	if (_dpu_power_check(dbg_base->dump_mode))
		pm_runtime_put_sync(dbg_base->dev);
}

/**
 * _dpu_dbg_get_dump_range - helper to retrieve dump length for a range node
 * @range_node: range node to dump
 * @max_offset: max offset of the register base
 * @Return: length
 */
static u32 _dpu_dbg_get_dump_range(struct dpu_dbg_reg_offset *range_node,
		size_t max_offset)
{
	u32 length = 0;

	if (range_node->start == 0 && range_node->end == 0) {
		length = max_offset;
	} else if (range_node->start < max_offset) {
		if (range_node->end > max_offset)
			length = max_offset - range_node->start;
		else if (range_node->start < range_node->end)
			length = range_node->end - range_node->start;
	}

	return length;
}

static int _dpu_dump_reg_range_cmp(void *priv, const struct list_head *a,
		const struct list_head *b)
{
	struct dpu_dbg_reg_range *ar, *br;

	if (!a || !b)
		return 0;

	ar = container_of(a, struct dpu_dbg_reg_range, head);
	br = container_of(b, struct dpu_dbg_reg_range, head);

	return ar->offset.start - br->offset.start;
}

/**
 * _dpu_dump_reg_by_ranges - dump ranges or full range of the register blk base
 * @dbg: register blk base structure
 * @reg_dump_flag: dump target, memory, kernel log, or both
 */
static void _dpu_dump_reg_by_ranges(struct dpu_dbg_base *dbg_base,
		struct dpu_dbg_reg_base *dbg,
		u32 reg_dump_flag)
{
	void __iomem *addr;
	size_t len;
	struct dpu_dbg_reg_range *range_node;

	if (!dbg || !(dbg->base || dbg->cb)) {
		pr_err("dbg base is null!\n");
		return;
	}

//	dump_stack();
	dev_info(dbg_base->dev, "%s:=========%s DUMP=========\n", __func__,
			dbg->name);

	if (reg_dump_flag & DPU_DBG_DUMP_IN_COREDUMP)
		drm_printf(dbg_base->dpu_dbg_printer,
				"%s:=========%s DUMP=========\n",
				__func__, dbg->name);

	if (dbg->cb) {
		dbg->cb(dbg->cb_ptr);
	/* If there is a list to dump the registers by ranges, use the ranges */
	} else if (!list_empty(&dbg->sub_range_list)) {
		/* sort the list by start address first */
		list_sort(NULL, &dbg->sub_range_list, _dpu_dump_reg_range_cmp);
		list_for_each_entry(range_node, &dbg->sub_range_list, head) {
			len = _dpu_dbg_get_dump_range(&range_node->offset,
				dbg->max_offset);
			addr = dbg->base + range_node->offset.start;

			pr_debug("%s: range_base=0x%pK start=0x%x end=0x%x\n",
				range_node->range_name,
				addr, range_node->offset.start,
				range_node->offset.end);

			_dpu_dump_reg(dbg_base, range_node->range_name,
					reg_dump_flag,
					dbg->base, addr, len,
					&range_node->reg_dump);
		}
	} else {
		/* If there is no list to dump ranges, dump all registers */
		dev_info(dbg_base->dev,
				"Ranges not found, will dump full registers\n");
		dev_info(dbg_base->dev, "base:0x%pK len:0x%zx\n", dbg->base,
				dbg->max_offset);
		addr = dbg->base;
		len = dbg->max_offset;
		_dpu_dump_reg(dbg_base, dbg->name, reg_dump_flag,
				dbg->base, addr, len,
				&dbg->reg_dump);
	}
}

/**
 * _dpu_dump_reg_by_blk - dump a named register base region
 * @blk_name: register blk name
 */
static void _dpu_dump_reg_by_blk(struct dpu_dbg_base *dbg_base,
		const char *blk_name)
{
	struct dpu_dbg_reg_base *blk_base;

	if (!dbg_base)
		return;

	list_for_each_entry(blk_base, &dbg_base->reg_base_list, reg_base_head) {
		if (strlen(blk_base->name) &&
			!strcmp(blk_base->name, blk_name)) {
			_dpu_dump_reg_by_ranges(dbg_base, blk_base,
				dbg_base->enable_reg_dump);
			break;
		}
	}
}

/**
 * _dpu_dump_reg_all - dump all register regions
 */
static void _dpu_dump_reg_all(struct dpu_dbg_base *dbg_base)
{
	struct dpu_dbg_reg_base *blk_base;

	if (!dbg_base)
		return;

	list_for_each_entry(blk_base, &dbg_base->reg_base_list, reg_base_head) {

		if (!strlen(blk_base->name))
			continue;

		_dpu_dump_reg_by_blk(dbg_base, blk_base->name);
	}
}

struct dpu_dbg_reg_base *_dpu_dump_get_blk_addr(struct dpu_dbg_base *dbg_base,
		const char *blk_name)
{
	struct dpu_dbg_reg_base *blk_base;

	list_for_each_entry(blk_base, &dbg_base->reg_base_list, reg_base_head)
		if (strlen(blk_base->name) && !strcmp(blk_base->name, blk_name))
			return blk_base;

	return NULL;
}

void _dpu_dump_array(struct dpu_dbg_base *dbg_base,
		struct dpu_dbg_reg_base *blk_arr[],
		u32 len, bool do_panic, const char *name, bool dump_all)
{
	int i;

	mutex_lock(&dbg_base->mutex);

	if (dump_all || !blk_arr || !len) {
		_dpu_dump_reg_all(dbg_base);
	} else {
		for (i = 0; i < len; i++) {
			if (blk_arr[i] != NULL)
				_dpu_dump_reg_by_ranges(dbg_base,
						blk_arr[i],
						dbg_base->enable_reg_dump);
		}
	}

	if (do_panic)
		panic(name);

	mutex_unlock(&dbg_base->mutex);
}
