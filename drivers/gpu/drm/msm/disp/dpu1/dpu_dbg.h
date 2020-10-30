/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 */

#ifndef DPU_DBG_H_
#define DPU_DBG_H_

#include <drm/drm_atomic_helper.h>
#include <drm/drm_device.h>
#include "../../../drm_crtc_internal.h"
#include <drm/drm_print.h>
#include <drm/drm_atomic.h>
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/slab.h>
#include <linux/list_sort.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/kthread.h>
#include <linux/devcoredump.h>
#include <stdarg.h>

#define DPU_DBG_DUMP_DATA_LIMITER (NULL)

enum dpu_dbg_dump_flag {
	DPU_DBG_DUMP_IN_LOG = BIT(0),
	DPU_DBG_DUMP_IN_MEM = BIT(1),
	DPU_DBG_DUMP_IN_COREDUMP = BIT(2),
};

enum dpu_dbg_dump_context {
	DPU_DBG_DUMP_PROC_CTX,
	DPU_DBG_DUMP_CLK_ENABLED_CTX,
};

#define DPU_DBG_BASE_MAX		10

#define DEFAULT_PANIC		0
#define DEFAULT_REGDUMP		DPU_DBG_DUMP_IN_MEM
#define DEFAULT_BASE_REG_CNT	DEFAULT_MDSS_HW_BLOCK_SIZE
#define ROW_BYTES		16
#define RANGE_NAME_LEN		40
#define REG_BASE_NAME_LEN	80

/* print debug ranges in groups of 4 u32s */
#define REG_DUMP_ALIGN		16

/**
 * struct dpu_dbg_reg_offset - tracking for start and end of region
 * @start: start offset
 * @start: end offset
 */
struct dpu_dbg_reg_offset {
	u32 start;
	u32 end;
};

/**
 * struct dpu_dbg_reg_range - register dumping named sub-range
 * @head: head of this node
 * @reg_dump: address for the mem dump
 * @range_name: name of this range
 * @offset: offsets for range to dump
 * @xin_id: client xin id
 */
struct dpu_dbg_reg_range {
	struct list_head head;
	u32 *reg_dump;
	char range_name[RANGE_NAME_LEN];
	struct dpu_dbg_reg_offset offset;
	uint32_t xin_id;
};

/**
 * struct dpu_dbg_reg_base - register region base.
 *	may sub-ranges: sub-ranges are used for dumping
 *	or may not have sub-ranges: dumping is base -> max_offset
 * @reg_base_head: head of this node
 * @sub_range_list: head to the list with dump ranges
 * @name: register base name
 * @base: base pointer
 * @off: cached offset of region for manual register dumping
 * @cnt: cached range of region for manual register dumping
 * @max_offset: length of region
 * @buf: buffer used for manual register dumping
 * @buf_len:  buffer length used for manual register dumping
 * @reg_dump: address for the mem dump if no ranges used
 * @cb: callback for external dump function, null if not defined
 * @cb_ptr: private pointer to callback function
 */
struct dpu_dbg_reg_base {
	struct list_head reg_base_head;
	struct list_head sub_range_list;
	char name[REG_BASE_NAME_LEN];
	void __iomem *base;
	size_t off;
	size_t cnt;
	size_t max_offset;
	char *buf;
	size_t buf_len;
	u32 *reg_dump;
	void (*cb)(void *ptr);
	void *cb_ptr;
};

/**
 * struct dpu_dbg_base - global sde debug base structure
 * @evtlog: event log instance
 * @reg_base_list: list of register dumping regions
 * @dev: device pointer
 * @drm_dev: drm device pointer
 * @mutex: mutex to serialize access to serialze dumps, debugfs access
 * @req_dump_blks: list of blocks requested for dumping
 * @work_panic: panic after dump if internal user passed "panic" special region
 * @enable_reg_dump: whether to dump registers into memory, kernel log, or both
 * @dump_all: dump all entries in register dump
 * @coredump_pending: coredump is pending read from userspace
 * @atomic_state: atomic state duplicated at the time of the error
 * @dump_worker: kworker thread which runs the dump work
 * @dump_work: kwork which dumps the registers and drm state
 * @timestamp: timestamp at which the coredump was captured
 * @dpu_dbg_printer: drm printer handle used to take drm snapshot
 * @dump_mode: decides whether the data is dumped in memory or logs
 */
struct dpu_dbg_base {
	struct list_head reg_base_list;
	struct device *dev;
	struct drm_device *drm_dev;
	struct mutex mutex;

	struct dpu_dbg_reg_base *req_dump_blks[DPU_DBG_BASE_MAX];

	bool work_panic;
	u32 enable_reg_dump;

	bool dump_all;
	bool coredump_pending;

	struct drm_atomic_state *atomic_state;

	struct kthread_worker *dump_worker;
	struct kthread_work dump_work;
	ktime_t timestamp;

	struct drm_printer *dpu_dbg_printer;

	enum dpu_dbg_dump_context dump_mode;
};

struct dpu_dbg_power_ctrl {
	void *handle;
	void *client;
	int (*enable_fn)(void *handle, void *client, bool enable);
};


/**
 * DPU_DBG_DUMP - trigger dumping of all dpu_dbg facilities
 * @va_args:	list of named register dump ranges and regions to dump, as
 *		registered previously through dpu_dbg_reg_register_base and
 *		dpu_dbg_reg_register_dump_range.
 *		Including the special name "panic" will trigger a panic after
 *		the dumping work has completed.
 */
#define DPU_DBG_DUMP(...) dpu_dbg_dump(DPU_DBG_DUMP_PROC_CTX, __func__, \
		##__VA_ARGS__, DPU_DBG_DUMP_DATA_LIMITER)

/**
 * DPU_DBG_DUMP_CLK_EN - trigger dumping of all dpu_dbg facilities, without clk
 * @va_args:	list of named register dump ranges and regions to dump, as
 *		registered previously through dpu_dbg_reg_register_base and
 *		dpu_dbg_reg_register_dump_range.
 *		Including the special name "panic" will trigger a panic after
 *		the dumping work has completed.
 */
#define DPU_DBG_DUMP_CLK_EN(...) dpu_dbg_dump(DPU_DBG_DUMP_CLK_ENABLED_CTX, \
		__func__, ##__VA_ARGS__, DPU_DBG_DUMP_DATA_LIMITER)

/**
 * dpu_dbg_init - initialize global sde debug facilities: evtlog, regdump
 * @dev:		device handle
 * Returns:		0 or -ERROR
 */
int dpu_dbg_init(struct device *dev);

/**
 * dpu_dbg_register_drm_dev - register a drm device with the dpu dbg module
 * @ddev:		drm device hangle
 * Returns:		void
 */
void dpu_dbg_register_drm_dev(struct drm_device *ddev);

/**
 * dpu_dbg_destroy - destroy the global sde debug facilities
 * Returns:	none
 */
void dpu_dbg_destroy(void);

/**
 * dpu_dbg_dump - trigger dumping of all dpu_dbg facilities
 * @queue_work:	whether to queue the dumping work to the work_struct
 * @name:	string indicating origin of dump
 * @va_args:	list of named register dump ranges and regions to dump, as
 *		registered previously through dpu_dbg_reg_register_base and
 *		dpu_dbg_reg_register_dump_range.
 *		Including the special name "panic" will trigger a panic after
 *		the dumping work has completed.
 * Returns:	none
 */
void dpu_dbg_dump(enum dpu_dbg_dump_context mode, const char *name, ...);

/**
 * dpu_dbg_reg_register_base - register a hw register address section for later
 *	dumping. call this before calling dpu_dbg_reg_register_dump_range
 *	to be able to specify sub-ranges within the base hw range.
 * @name:	name of base region
 * @base:	base pointer of region
 * @max_offset:	length of region
 * Returns:	0 or -ERROR
 */
int dpu_dbg_reg_register_base(const char *name, void __iomem *base,
		size_t max_offset);

/**
 * dpu_dbg_reg_register_dump_range - register a hw register sub-region for
 *	later register dumping associated with base specified by
 *	dpu_dbg_reg_register_base
 * @base_name:		name of base region
 * @range_name:		name of sub-range within base region
 * @offset_start:	sub-range's start offset from base's base pointer
 * @offset_end:		sub-range's end offset from base's base pointer
 * @xin_id:		xin id
 * Returns:		none
 */
void dpu_dbg_reg_register_dump_range(const char *base_name,
		const char *range_name, u32 offset_start, u32 offset_end,
		uint32_t xin_id);

/**
 * dpu_dbg_set_sde_top_offset - set the target specific offset from mdss base
 *	address of the top registers. Used for accessing debug bus controls.
 * @blk_off: offset from mdss base of the top block
 */
void dpu_dbg_set_sde_top_offset(u32 blk_off);

/**
 * _dpu_dump_array - dump array of register bases
 * @blk_arr: array of register base pointers
 * @len: length of blk_arr
 * @do_panic: whether to trigger a panic after dumping
 * @name: string indicating origin of dump
 * @dump_all: dump all regs
 */
void _dpu_dump_array(struct dpu_dbg_base *dbg_base,
		struct dpu_dbg_reg_base *blk_arr[],
		u32 len, bool do_panic, const char *name, bool dump_all);

/**
 * _dpu_dump_get_blk_addr - retrieve register block address by name
 * @blk_name: register blk name
 * @Return: register blk base, or NULL
 */
struct dpu_dbg_reg_base *_dpu_dump_get_blk_addr(struct dpu_dbg_base *dbg_base,
		const char *blk_name);

#endif /* DPU_DBG_H_ */
