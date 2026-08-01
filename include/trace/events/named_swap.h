/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM named_swap

#if !defined(_TRACE_NAMED_SWAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_NAMED_SWAP_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

#ifndef _TRACE_NAMED_SWAP_DEFS
#define _TRACE_NAMED_SWAP_DEFS

#define NAMED_SWAP_INDEX_NONE	(~0ULL)

enum named_swap_fault_path {
	NAMED_SWAP_FAULT_MISSING_READ,
	NAMED_SWAP_FAULT_MISSING_WRITE,
	NAMED_SWAP_FAULT_REFAULT,
	NAMED_SWAP_FAULT_WP_ZERO,
	NAMED_SWAP_FAULT_WP_REUSE,
	NAMED_SWAP_FAULT_WP_COW,
	NAMED_SWAP_FAULT_ACCESS,
	NAMED_SWAP_FAULT_SWAP_FALLBACK,
	NAMED_SWAP_FAULT_SIGBUS,
};

#define NAMED_SWAP_FAULT_PATHS					\
	{ NAMED_SWAP_FAULT_MISSING_READ,	"missing_read" },	\
	{ NAMED_SWAP_FAULT_MISSING_WRITE,	"missing_write" },	\
	{ NAMED_SWAP_FAULT_REFAULT,		"refault" },		\
	{ NAMED_SWAP_FAULT_WP_ZERO,		"wp_zero" },		\
	{ NAMED_SWAP_FAULT_WP_REUSE,		"wp_reuse" },		\
	{ NAMED_SWAP_FAULT_WP_COW,		"wp_cow" },		\
	{ NAMED_SWAP_FAULT_ACCESS,		"access" },		\
	{ NAMED_SWAP_FAULT_SWAP_FALLBACK,	"swap_fallback" },	\
	{ NAMED_SWAP_FAULT_SIGBUS,		"sigbus" }

enum named_swap_resize_op {
	NAMED_SWAP_RESIZE_ENLARGE,
	NAMED_SWAP_RESIZE_SHRINK,
	NAMED_SWAP_RESIZE_DEALLOC,
	NAMED_SWAP_RESIZE_UNCOMMIT,
	NAMED_SWAP_RESIZE_ALLOC,
};

#define NAMED_SWAP_RESIZE_OPS					\
	{ NAMED_SWAP_RESIZE_ENLARGE,	"enlarge" },		\
	{ NAMED_SWAP_RESIZE_SHRINK,	"shrink" },		\
	{ NAMED_SWAP_RESIZE_DEALLOC,	"dealloc" },		\
	{ NAMED_SWAP_RESIZE_UNCOMMIT,	"uncommit" },		\
	{ NAMED_SWAP_RESIZE_ALLOC,	"alloc" }

enum named_swap_wb_reason {
	NAMED_SWAP_WB_RECLAIM,
	NAMED_SWAP_WB_FLUSHER,
};

#define NAMED_SWAP_WB_REASONS					\
	{ NAMED_SWAP_WB_RECLAIM,	"reclaim" },		\
	{ NAMED_SWAP_WB_FLUSHER,	"flusher" }

/*
 * Why a named_swap readahead window was chosen. Used to explain I/O size
 * collapse despite linear user faults (cache holes / small windows / etc).
 */
enum named_swap_ra_reason {
	NAMED_SWAP_RA_MMAP_AROUND,	/* default mmap miss: centered window */
	NAMED_SWAP_RA_MMAP_SEQ,		/* VM_SEQ_READ -> sync_ra(ra_pages) */
	NAMED_SWAP_RA_SYNC_INIT,	/* sync: start-of-file / sequential init */
	NAMED_SWAP_RA_SYNC_HISTORY,	/* sync: ramp from cached contig history */
	NAMED_SWAP_RA_SYNC_SMALL,	/* sync: treated as small/random */
	NAMED_SWAP_RA_ASYNC_HIT,	/* async: expected PG_readahead index */
	NAMED_SWAP_RA_ASYNC_INTERLEAVED,/* async: interleaved / next-miss query */
};

#define NAMED_SWAP_RA_REASONS						\
	{ NAMED_SWAP_RA_MMAP_AROUND,		"mmap_around" },	\
	{ NAMED_SWAP_RA_MMAP_SEQ,		"mmap_seq" },		\
	{ NAMED_SWAP_RA_SYNC_INIT,		"sync_init" },		\
	{ NAMED_SWAP_RA_SYNC_HISTORY,		"sync_history" },	\
	{ NAMED_SWAP_RA_SYNC_SMALL,		"sync_small" },		\
	{ NAMED_SWAP_RA_ASYNC_HIT,		"async_hit" },		\
	{ NAMED_SWAP_RA_ASYNC_INTERLEAVED,	"async_interleaved" }

#define NAMED_SWAP_SEQ_STOPS						\
	{ 0 /* NAMED_SWAP_SEQ_OK */,		"ok" },			\
	{ 1 /* WINDOW_FULL */,			"window_full" },	\
	{ 2 /* NO_ENTRY */,			"no_entry" },		\
	{ 3 /* VALUE_ENTRY */,			"value_entry" },	\
	{ 4 /* INDEX_GAP */,			"index_gap" },		\
	{ 5 /* DIRTY */,			"dirty" },		\
	{ 6 /* WRITEBACK */,			"writeback" },		\
	{ 7 /* WRONG_GEN */,			"wrong_gen" },		\
	{ 8 /* WRONG_ZONE */,			"wrong_zone" },		\
	{ 9 /* WRONG_TYPE */,			"wrong_type" },		\
	{ 10 /* WRONG_LRUVEC */,		"wrong_lruvec" },	\
	{ 11 /* NOT_LRU */,			"not_lru" },		\
	{ 12 /* UNEVICTABLE */,			"unevictable" },	\
	{ 13 /* ACTIVE */,			"active" },		\
	{ 14 /* SORT */,			"sort" },		\
	{ 15 /* ISOLATE_FAIL */,		"isolate_fail" },	\
	{ 16 /* GO_BACK_LIMIT */,		"go_back_limit" },	\
	{ 17 /* MAPPING_GONE */,		"mapping_gone" }

#define NAMED_SWAP_RECLAIM_RESULTS					\
	{ 0 /* FREED */,			"freed" },		\
	{ 1 /* FREED_RACE */,			"freed_race" },		\
	{ 2 /* KEEP_LOCK */,			"keep_lock" },		\
	{ 3 /* KEEP_MAPPED */,			"keep_mapped" },	\
	{ 4 /* KEEP_REFERENCED */,		"keep_referenced" },	\
	{ 5 /* KEEP_DIRTY */,			"keep_dirty" },		\
	{ 6 /* KEEP_PAGEOUT */,			"keep_pageout" },	\
	{ 7 /* KEEP_WRITEBACK */,		"keep_writeback" },	\
	{ 8 /* KEEP_MAPPING */,			"keep_mapping" },	\
	{ 9 /* KEEP_OTHER */,			"keep_other" },		\
	{ 10 /* ACTIVATE_UNEVICTABLE */,	"activate_unevictable" }, \
	{ 11 /* ACTIVATE_WRITEBACK */,		"activate_writeback" },	\
	{ 12 /* ACTIVATE_REFERENCED */,		"activate_referenced" }, \
	{ 13 /* ACTIVATE_UNMAP_FAIL */,		"activate_unmap_fail" }, \
	{ 14 /* ACTIVATE_PINNED */,		"activate_pinned" },	\
	{ 15 /* ACTIVATE_DIRTY */,		"activate_dirty" },	\
	{ 16 /* ACTIVATE_PAGEOUT */,		"activate_pageout" },	\
	{ 17 /* ACTIVATE_BUFFERS */,		"activate_buffers" },	\
	{ 18 /* ACTIVATE_SWAP */,		"activate_swap" },	\
	{ 19 /* ACTIVATE_OTHER */,		"activate_other" },	\
	{ 20 /* WAIT_WRITEBACK */,		"wait_writeback" },	\
	{ 21 /* DEMOTE */,			"demote" }

/*
 * Packed args for named_swap_balance_dirty (TRACE_EVENT BPF limit is 12).
 * Rates are pages/s; *_kbps printed in the event via PAGE_SHIFT convert.
 */
struct named_swap_dirty_trace {
	unsigned long thresh;
	unsigned long bg_thresh;
	unsigned long dirty;
	unsigned long wb_thresh;
	unsigned long wb_dirty;
	unsigned long dirty_ratelimit;
	unsigned long task_ratelimit;
	unsigned long dirtied;
	long pause_ms;
	unsigned int flags;
	unsigned long write_bw;
	unsigned long avg_write_bw;
	unsigned long dirty_rate;
	unsigned long pos_ratio;
	unsigned long setpoint;
	unsigned long freerun_ceil;
	unsigned long balanced_dirty_ratelimit;
	long period_ms;
	long min_pause_ms;
	long max_pause_ms;
};

#endif /* _TRACE_NAMED_SWAP_DEFS */

TRACE_EVENT(named_swap_file_create,
	TP_PROTO(struct file *file, u64 index, unsigned long len, int ret),

	TP_ARGS(file, index, len, ret),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(struct file *, file)
		__field(u64, index)
		__field(unsigned long, len)
		__field(unsigned long, pages)
		__field(int, ret)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->file = file;
		__entry->index = index;
		__entry->len = len;
		__entry->pages = len >> PAGE_SHIFT;
		__entry->ret = ret;
	),

	TP_printk("pid=%d comm=%s file=%p index=%llu len=%lu pages=%lu ret=%d",
		__entry->pid, __entry->comm, __entry->file,
		__entry->index, __entry->len, __entry->pages, __entry->ret)
);

TRACE_EVENT(named_swap_file_release,
	TP_PROTO(struct file *file, u64 index, unsigned long pages),

	TP_ARGS(file, index, pages),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(u64, index)
		__field(unsigned long, pages)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->index = index;
		__entry->pages = pages;
	),

	TP_printk("file=%p index=%llu pages=%lu",
		__entry->file, __entry->index, __entry->pages)
);

TRACE_EVENT(named_swap_link,
	TP_PROTO(struct file *file, struct vm_area_struct *vma,
		 struct anon_vma *anon_vma, u64 index, bool refreshed),

	TP_ARGS(file, vma, anon_vma, index, refreshed),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(struct vm_area_struct *, vma)
		__field(struct anon_vma *, anon_vma)
		__field(u64, index)
		__field(bool, refreshed)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->vma = vma;
		__entry->anon_vma = anon_vma;
		__entry->index = index;
		__entry->refreshed = refreshed;
	),

	TP_printk("file=%p index=%llu vma=%p anon_vma=%p refreshed=%d",
		__entry->file, __entry->index, __entry->vma,
		__entry->anon_vma, __entry->refreshed)
);

TRACE_EVENT(named_swap_resize,
	TP_PROTO(struct vm_area_struct *vma, enum named_swap_resize_op op,
		 unsigned long addr, unsigned long delta, loff_t old_size,
		 loff_t new_size, u64 index, int ret),

	TP_ARGS(vma, op, addr, delta, old_size, new_size, index, ret),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(pid_t, tgid)
		__array(char, comm, TASK_COMM_LEN)
		__field(struct vm_area_struct *, vma)
		__field(unsigned long, vma_start)
		__field(unsigned long, vma_end)
		__field(unsigned long, vm_flags)
		__field(unsigned long, pgoff)
		__field(unsigned long, addr)
		__field(unsigned long, delta)
		__field(loff_t, old_size)
		__field(loff_t, new_size)
		__field(u64, index)
		__field(enum named_swap_resize_op, op)
		__field(int, ret)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		__entry->tgid = current->tgid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->vma = vma;
		__entry->vma_start = vma ? vma->vm_start : 0;
		__entry->vma_end = vma ? vma->vm_end : 0;
		__entry->vm_flags = vma ? vma->vm_flags : 0;
		__entry->pgoff = vma ? vma->vm_pgoff : 0;
		__entry->addr = addr;
		__entry->delta = delta;
		__entry->old_size = old_size;
		__entry->new_size = new_size;
		__entry->index = index;
		__entry->op = op;
		__entry->ret = ret;
	),

	TP_printk("pid=%d tgid=%d comm=%s op=%s vma=%p %lx-%lx flags=%lx pgoff=%lx addr=%lx delta=%lu old=%lld new=%lld index=%llu ret=%d",
		__entry->pid, __entry->tgid, __entry->comm,
		__print_symbolic(__entry->op, NAMED_SWAP_RESIZE_OPS),
		__entry->vma, __entry->vma_start, __entry->vma_end,
		__entry->vm_flags, __entry->pgoff, __entry->addr,
		__entry->delta, __entry->old_size, __entry->new_size,
		__entry->index, __entry->ret)
);

TRACE_EVENT(named_swap_user_segv,
	TP_PROTO(unsigned long address, unsigned long ip, unsigned long error_code,
		 int si_code, struct vm_area_struct *vma, u64 index),

	TP_ARGS(address, ip, error_code, si_code, vma, index),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(pid_t, tgid)
		__array(char, comm, TASK_COMM_LEN)
		__field(unsigned long, address)
		__field(unsigned long, ip)
		__field(unsigned long, error_code)
		__field(int, si_code)
		__field(unsigned long, vma_start)
		__field(unsigned long, vma_end)
		__field(unsigned long, vm_flags)
		__field(unsigned long, pgoff)
		__field(u64, index)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		__entry->tgid = current->tgid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->address = address;
		__entry->ip = ip;
		__entry->error_code = error_code;
		__entry->si_code = si_code;
		__entry->vma_start = vma ? vma->vm_start : 0;
		__entry->vma_end = vma ? vma->vm_end : 0;
		__entry->vm_flags = vma ? vma->vm_flags : 0;
		__entry->pgoff = vma ? vma->vm_pgoff : 0;
		__entry->index = index;
	),

	TP_printk("pid=%d tgid=%d comm=%s addr=%lx ip=%lx err=0x%lx si=%d vma=%lx-%lx flags=%lx pgoff=%lx index=%llu",
		__entry->pid, __entry->tgid, __entry->comm,
		__entry->address, __entry->ip, __entry->error_code,
		__entry->si_code, __entry->vma_start, __entry->vma_end,
		__entry->vm_flags, __entry->pgoff, __entry->index)
);

TRACE_EVENT(named_swap_fault,
	TP_PROTO(struct vm_area_struct *vma, unsigned long address,
		 unsigned int fault_flags, u64 index,
		 enum named_swap_fault_path path, unsigned int alias_count,
		 int mapcount, vm_fault_t ret),

	TP_ARGS(vma, address, fault_flags, index, path, alias_count, mapcount,
		ret),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(struct mm_struct *, mm)
		__field(struct vm_area_struct *, vma)
		__field(unsigned long, address)
		__field(pgoff_t, pgoff)
		__field(unsigned int, fault_flags)
		__field(u64, index)
		__field(enum named_swap_fault_path, path)
		__field(unsigned int, alias_count)
		__field(int, mapcount)
		__field(vm_fault_t, ret)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->mm = vma->vm_mm;
		__entry->vma = vma;
		__entry->address = address;
		__entry->pgoff = linear_page_index(vma, address);
		__entry->fault_flags = fault_flags;
		__entry->index = index;
		__entry->path = path;
		__entry->alias_count = alias_count;
		__entry->mapcount = mapcount;
		__entry->ret = ret;
	),

	TP_printk("pid=%d comm=%s mm=%p vma=%p address=%lx pgoff=%lu index=%llu path=%s flags=%s alias_count=%u mapcount=%d ret=%s",
		__entry->pid, __entry->comm, __entry->mm, __entry->vma,
		__entry->address, __entry->pgoff, __entry->index,
		__print_symbolic(__entry->path, NAMED_SWAP_FAULT_PATHS),
		__print_flags(__entry->fault_flags, "|", FAULT_FLAG_TRACE),
		__entry->alias_count, __entry->mapcount,
		__print_flags(__entry->ret, "|", VM_FAULT_RESULT_TRACE))
);

TRACE_EVENT(named_swap_alias_count,
	TP_PROTO(struct vm_area_struct *vma, unsigned long address,
		 u64 index, unsigned int count),

	TP_ARGS(vma, address, index, count),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(struct mm_struct *, mm)
		__field(struct vm_area_struct *, vma)
		__field(unsigned long, address)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(unsigned int, count)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->mm = vma->vm_mm;
		__entry->vma = vma;
		__entry->address = address;
		__entry->pgoff = linear_page_index(vma, address);
		__entry->index = index;
		__entry->count = count;
	),

	TP_printk("pid=%d comm=%s mm=%p vma=%p address=%lx pgoff=%lu index=%llu count=%u",
		__entry->pid, __entry->comm, __entry->mm, __entry->vma,
		__entry->address, __entry->pgoff, __entry->index,
		__entry->count)
);

TRACE_EVENT(named_swap_filemap_lookup,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 bool hit, bool uptodate),

	TP_ARGS(mapping, pgoff, index, hit, uptodate),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(bool, hit)
		__field(bool, uptodate)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->hit = hit;
		__entry->uptodate = uptodate;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu hit=%d uptodate=%d",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->hit,
		__entry->uptodate)
);

TRACE_EVENT(named_swap_folio_alloc,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 unsigned long pfn, unsigned int order, gfp_t gfp),

	TP_ARGS(mapping, pgoff, index, pfn, order, gfp),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(unsigned long, pfn)
		__field(unsigned int, order)
		__field(gfp_t, gfp)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->pfn = pfn;
		__entry->order = order;
		__entry->gfp = gfp;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu pfn=%lx order=%u gfp=%#x",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->pfn, __entry->order,
		__entry->gfp)
);

TRACE_EVENT(named_swap_cache_add,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 unsigned long pfn, unsigned int order, bool shadow),

	TP_ARGS(mapping, pgoff, index, pfn, order, shadow),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(unsigned long, pfn)
		__field(unsigned int, order)
		__field(bool, shadow)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->pfn = pfn;
		__entry->order = order;
		__entry->shadow = shadow;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu pfn=%lx order=%u shadow=%d",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->pfn, __entry->order,
		__entry->shadow)
);

TRACE_EVENT(named_swap_writeback,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff,
		 unsigned long pfn, unsigned int order, int mapcount,
		 enum named_swap_wb_reason reason, int result),

	TP_ARGS(mapping, pgoff, pfn, order, mapcount, reason, result),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(unsigned long, pfn)
		__field(unsigned int, order)
		__field(int, mapcount)
		__field(enum named_swap_wb_reason, reason)
		__field(int, result)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->pfn = pfn;
		__entry->order = order;
		__entry->mapcount = mapcount;
		__entry->reason = reason;
		__entry->result = result;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu pfn=%lx order=%u mapcount=%d reason=%s result=%d",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->pfn, __entry->order,
		__entry->mapcount,
		__print_symbolic(__entry->reason, NAMED_SWAP_WB_REASONS),
		__entry->result)
);

TRACE_EVENT(named_swap_cache_delete,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 unsigned long pfn, unsigned int order),

	TP_ARGS(mapping, pgoff, index, pfn, order),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(unsigned long, pfn)
		__field(unsigned int, order)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->pfn = pfn;
		__entry->order = order;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu pfn=%lx order=%u",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->pfn, __entry->order)
);

TRACE_EVENT(named_swap_unmap,
	TP_PROTO(struct vm_area_struct *vma, unsigned long address, u64 index),

	TP_ARGS(vma, address, index),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(struct mm_struct *, mm)
		__field(struct vm_area_struct *, vma)
		__field(unsigned long, address)
		__field(pgoff_t, pgoff)
		__field(u64, index)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->mm = vma->vm_mm;
		__entry->vma = vma;
		__entry->address = address;
		__entry->pgoff = linear_page_index(vma, address);
		__entry->index = index;
	),

	TP_printk("pid=%d comm=%s mm=%p vma=%p address=%lx pgoff=%lu index=%llu",
		__entry->pid, __entry->comm, __entry->mm, __entry->vma,
		__entry->address, __entry->pgoff, __entry->index)
);

TRACE_EVENT(named_swap_readahead,
	TP_PROTO(struct address_space *mapping, pgoff_t start, u64 index,
		 unsigned long nr_pages, unsigned long async_size),

	TP_ARGS(mapping, start, index, nr_pages, async_size),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, start)
		__field(u64, index)
		__field(unsigned long, nr_pages)
		__field(unsigned long, async_size)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->start = start;
		__entry->index = index;
		__entry->nr_pages = nr_pages;
		__entry->async_size = async_size;
	),

	TP_printk("dev=%d:%d ino=%lx start=%lu index=%llu nr_pages=%lu async_size=%lu",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->start, __entry->index, __entry->nr_pages,
		__entry->async_size)
);

/*
 * Planned readahead window (before hole splits). Compare size/ra_pages to
 * named_swap_ra_submit nr_pages to see collapse.
 */
TRACE_EVENT(named_swap_ra_window,
	TP_PROTO(struct address_space *mapping, u64 index,
		 enum named_swap_ra_reason reason, pgoff_t start,
		 unsigned long size, unsigned long async_size,
		 unsigned long ra_pages, unsigned long req_count,
		 unsigned long aux, unsigned int mmap_miss),

	TP_ARGS(mapping, index, reason, start, size, async_size, ra_pages,
		req_count, aux, mmap_miss),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, index)
		__field(enum named_swap_ra_reason, reason)
		__field(pgoff_t, start)
		__field(unsigned long, size)
		__field(unsigned long, async_size)
		__field(unsigned long, ra_pages)
		__field(unsigned long, req_count)
		__field(unsigned long, aux)
		__field(unsigned int, mmap_miss)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->index = index;
		__entry->reason = reason;
		__entry->start = start;
		__entry->size = size;
		__entry->async_size = async_size;
		__entry->ra_pages = ra_pages;
		__entry->req_count = req_count;
		__entry->aux = aux;
		__entry->mmap_miss = mmap_miss;
	),

	TP_printk("pid=%d comm=%s dev=%d:%d ino=%lx index=%llu reason=%s start=%lu size=%lu async_size=%lu ra_pages=%lu req_count=%lu aux=%lu mmap_miss=%u",
		__entry->pid, __entry->comm,
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->index,
		__print_symbolic(__entry->reason, NAMED_SWAP_RA_REASONS),
		__entry->start, __entry->size, __entry->async_size,
		__entry->ra_pages, __entry->req_count, __entry->aux,
		__entry->mmap_miss)
);

/*
 * Actual I/O batch handed to ->readahead()/->read_folio(). nr_pages here is
 * what hits the block layer after cache-hole splits.
 */
TRACE_EVENT(named_swap_ra_submit,
	TP_PROTO(struct address_space *mapping, pgoff_t start, u64 index,
		 unsigned long nr_pages),

	TP_ARGS(mapping, start, index, nr_pages),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, start)
		__field(u64, index)
		__field(unsigned long, nr_pages)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->start = start;
		__entry->index = index;
		__entry->nr_pages = nr_pages;
	),

	TP_printk("pid=%d comm=%s dev=%d:%d ino=%lx start=%lu index=%llu nr_pages=%lu",
		__entry->pid, __entry->comm,
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->start, __entry->index, __entry->nr_pages)
);

/*
 * page_cache_ra_unbounded hit an already-present folio and flushed a partial
 * contiguous batch. pending_nr is the batch size about to be submitted;
 * wanted_nr is the original window. hole_pgoff is the present page index.
 */
TRACE_EVENT(named_swap_ra_hole,
	TP_PROTO(struct address_space *mapping, pgoff_t window_start,
		 pgoff_t hole_pgoff, u64 index, unsigned long pending_nr,
		 unsigned long wanted_nr),

	TP_ARGS(mapping, window_start, hole_pgoff, index, pending_nr,
		wanted_nr),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, window_start)
		__field(pgoff_t, hole_pgoff)
		__field(u64, index)
		__field(unsigned long, pending_nr)
		__field(unsigned long, wanted_nr)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->window_start = window_start;
		__entry->hole_pgoff = hole_pgoff;
		__entry->index = index;
		__entry->pending_nr = pending_nr;
		__entry->wanted_nr = wanted_nr;
	),

	TP_printk("pid=%d comm=%s dev=%d:%d ino=%lx window_start=%lu hole_pgoff=%lu index=%llu pending_nr=%lu wanted_nr=%lu",
		__entry->pid, __entry->comm,
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->window_start, __entry->hole_pgoff, __entry->index,
		__entry->pending_nr, __entry->wanted_nr)
);

TRACE_EVENT(named_swap_folio_uptodate,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 unsigned long pfn, unsigned int order, bool success),

	TP_ARGS(mapping, pgoff, index, pfn, order, success),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(unsigned long, pfn)
		__field(unsigned int, order)
		__field(bool, success)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->pfn = pfn;
		__entry->order = order;
		__entry->success = success;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu pfn=%lx order=%u success=%d",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->pfn, __entry->order,
		__entry->success)
);

TRACE_EVENT(named_swap_balance_dirty,
	TP_PROTO(struct address_space *mapping, u64 index,
		 const struct named_swap_dirty_trace *t),

	TP_ARGS(mapping, index, t),

	TP_STRUCT__entry(
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
		__field(dev_t, dev)
		__field(unsigned long, ino)
		__field(u64, index)
		__field(unsigned long, thresh)
		__field(unsigned long, bg_thresh)
		__field(unsigned long, dirty)
		__field(unsigned long, wb_thresh)
		__field(unsigned long, wb_dirty)
		__field(unsigned long, dirty_ratelimit)
		__field(unsigned long, task_ratelimit)
		__field(unsigned long, dirtied)
		__field(long, pause_ms)
		__field(unsigned int, flags)
		__field(unsigned long, write_bw)
		__field(unsigned long, avg_write_bw)
		__field(unsigned long, dirty_rate)
		__field(unsigned long, pos_ratio)
		__field(unsigned long, setpoint)
		__field(unsigned long, freerun_ceil)
		__field(unsigned long, balanced_dirty_ratelimit)
		__field(long, period_ms)
		__field(long, min_pause_ms)
		__field(long, max_pause_ms)
		__field(unsigned long, write_bw_kbps)
		__field(unsigned long, avg_write_bw_kbps)
		__field(unsigned long, dirty_rate_kbps)
		__field(unsigned long, dirty_ratelimit_kbps)
		__field(unsigned long, task_ratelimit_kbps)
		__field(unsigned long, balanced_dirty_ratelimit_kbps)
	),

	TP_fast_assign(
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->index = index;
		__entry->thresh = t->thresh;
		__entry->bg_thresh = t->bg_thresh;
		__entry->dirty = t->dirty;
		__entry->wb_thresh = t->wb_thresh;
		__entry->wb_dirty = t->wb_dirty;
		__entry->dirty_ratelimit = t->dirty_ratelimit;
		__entry->task_ratelimit = t->task_ratelimit;
		__entry->dirtied = t->dirtied;
		__entry->pause_ms = t->pause_ms;
		__entry->flags = t->flags;
		__entry->write_bw = t->write_bw;
		__entry->avg_write_bw = t->avg_write_bw;
		__entry->dirty_rate = t->dirty_rate;
		__entry->pos_ratio = t->pos_ratio;
		__entry->setpoint = t->setpoint;
		__entry->freerun_ceil = t->freerun_ceil;
		__entry->balanced_dirty_ratelimit = t->balanced_dirty_ratelimit;
		__entry->period_ms = t->period_ms;
		__entry->min_pause_ms = t->min_pause_ms;
		__entry->max_pause_ms = t->max_pause_ms;
		__entry->write_bw_kbps = t->write_bw << (PAGE_SHIFT - 10);
		__entry->avg_write_bw_kbps = t->avg_write_bw << (PAGE_SHIFT - 10);
		__entry->dirty_rate_kbps = t->dirty_rate << (PAGE_SHIFT - 10);
		__entry->dirty_ratelimit_kbps =
			t->dirty_ratelimit << (PAGE_SHIFT - 10);
		__entry->task_ratelimit_kbps =
			t->task_ratelimit << (PAGE_SHIFT - 10);
		__entry->balanced_dirty_ratelimit_kbps =
			t->balanced_dirty_ratelimit << (PAGE_SHIFT - 10);
	),

	TP_printk("pid=%d comm=%s dev=%d:%d ino=%lx index=%llu thresh=%lu bg_thresh=%lu dirty=%lu wb_thresh=%lu wb_dirty=%lu dirty_ratelimit=%lu task_ratelimit=%lu dirtied=%lu pause_ms=%ld freerun=%d dirty_exceeded=%d write_bw=%lu avg_write_bw=%lu dirty_rate=%lu pos_ratio=%lu setpoint=%lu freerun_ceil=%lu balanced_dirty_ratelimit=%lu period_ms=%ld min_pause_ms=%ld max_pause_ms=%ld write_bw_kbps=%lu avg_write_bw_kbps=%lu dirty_rate_kbps=%lu dirty_ratelimit_kbps=%lu task_ratelimit_kbps=%lu balanced_dirty_ratelimit_kbps=%lu",
		__entry->pid, __entry->comm,
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->index, __entry->thresh, __entry->bg_thresh,
		__entry->dirty, __entry->wb_thresh, __entry->wb_dirty,
		__entry->dirty_ratelimit, __entry->task_ratelimit,
		__entry->dirtied, __entry->pause_ms,
		!!(__entry->flags & 1), !!(__entry->flags & 2),
		__entry->write_bw, __entry->avg_write_bw, __entry->dirty_rate,
		__entry->pos_ratio, __entry->setpoint, __entry->freerun_ceil,
		__entry->balanced_dirty_ratelimit, __entry->period_ms,
		__entry->min_pause_ms, __entry->max_pause_ms,
		__entry->write_bw_kbps, __entry->avg_write_bw_kbps,
		__entry->dirty_rate_kbps, __entry->dirty_ratelimit_kbps,
		__entry->task_ratelimit_kbps,
		__entry->balanced_dirty_ratelimit_kbps)
);

TRACE_EVENT(named_swap_sort_promote,
	TP_PROTO(struct folio *folio, u64 index, bool dirty, bool writeback,
		 int old_gen, int new_gen),

	TP_ARGS(folio, index, dirty, writeback, old_gen, new_gen),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(ino_t, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(bool, dirty)
		__field(bool, writeback)
		__field(int, old_gen)
		__field(int, new_gen)
	),

	TP_fast_assign(
		struct address_space *mapping = folio_mapping(folio);

		__entry->dev = mapping && mapping->host ?
			(mapping->host->i_sb ? mapping->host->i_sb->s_dev :
			 mapping->host->i_rdev) : 0;
		__entry->ino = mapping && mapping->host ?
			mapping->host->i_ino : 0;
		__entry->pgoff = folio->index;
		__entry->index = index;
		__entry->dirty = dirty;
		__entry->writeback = writeback;
		__entry->old_gen = old_gen;
		__entry->new_gen = new_gen;
	),

	TP_printk("dev=%d:%d ino=%lx pgoff=%lu index=%llu dirty=%d writeback=%d old_gen=%d new_gen=%d",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index, __entry->dirty,
		__entry->writeback, __entry->old_gen, __entry->new_gen)
);

TRACE_EVENT(named_swap_seq_start,
	TP_PROTO(struct address_space *mapping, u64 index, pgoff_t seed,
		 pgoff_t go_back_to, unsigned int ahead_size),

	TP_ARGS(mapping, index, seed, go_back_to, ahead_size),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(ino_t, ino)
		__field(u64, index)
		__field(pgoff_t, seed)
		__field(pgoff_t, go_back_to)
		__field(unsigned int, ahead_size)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->index = index;
		__entry->seed = seed;
		__entry->go_back_to = go_back_to;
		__entry->ahead_size = ahead_size;
	),

	TP_printk("dev=%d:%d ino=%lx index=%llu seed=%lu go_back_to=%lu ahead_size=%u",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->index, __entry->seed, __entry->go_back_to,
		__entry->ahead_size)
);

TRACE_EVENT(named_swap_seq_step,
	TP_PROTO(struct address_space *mapping, u64 index, pgoff_t cur,
		 pgoff_t next, bool sequential, enum named_swap_seq_stop reason,
		 bool dirty, bool writeback, int gen, unsigned int isolated),

	TP_ARGS(mapping, index, cur, next, sequential, reason, dirty,
		writeback, gen, isolated),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(ino_t, ino)
		__field(u64, index)
		__field(pgoff_t, cur)
		__field(pgoff_t, next)
		__field(bool, sequential)
		__field(enum named_swap_seq_stop, reason)
		__field(bool, dirty)
		__field(bool, writeback)
		__field(int, gen)
		__field(unsigned int, isolated)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->index = index;
		__entry->cur = cur;
		__entry->next = next;
		__entry->sequential = sequential;
		__entry->reason = reason;
		__entry->dirty = dirty;
		__entry->writeback = writeback;
		__entry->gen = gen;
		__entry->isolated = isolated;
	),

	TP_printk("dev=%d:%d ino=%lx index=%llu cur=%lu next=%lu sequential=%d reason=%s dirty=%d writeback=%d gen=%d isolated=%u",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->index, __entry->cur, __entry->next,
		__entry->sequential,
		__print_symbolic(__entry->reason, NAMED_SWAP_SEQ_STOPS),
		__entry->dirty, __entry->writeback, __entry->gen,
		__entry->isolated)
);

TRACE_EVENT(named_swap_seq_stop,
	TP_PROTO(struct address_space *mapping, u64 index,
		 enum named_swap_seq_stop reason, unsigned int hits,
		 pgoff_t window_start, pgoff_t window_end,
		 unsigned int ahead_size),

	TP_ARGS(mapping, index, reason, hits, window_start, window_end,
		ahead_size),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(ino_t, ino)
		__field(u64, index)
		__field(enum named_swap_seq_stop, reason)
		__field(unsigned int, hits)
		__field(pgoff_t, window_start)
		__field(pgoff_t, window_end)
		__field(unsigned int, ahead_size)
	),

	TP_fast_assign(
		__entry->dev = mapping->host->i_sb ?
			mapping->host->i_sb->s_dev : mapping->host->i_rdev;
		__entry->ino = mapping->host->i_ino;
		__entry->index = index;
		__entry->reason = reason;
		__entry->hits = hits;
		__entry->window_start = window_start;
		__entry->window_end = window_end;
		__entry->ahead_size = ahead_size;
	),

	TP_printk("dev=%d:%d ino=%lx index=%llu reason=%s hits=%u window=[%lu,%lu) ahead_size=%u",
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->index,
		__print_symbolic(__entry->reason, NAMED_SWAP_SEQ_STOPS),
		__entry->hits, __entry->window_start, __entry->window_end,
		__entry->ahead_size)
);

/*
 * Per-folio shrink_folio_list outcome for named_swap. Correlate with
 * named_swap_seq_step (isolate) and named_swap_cache_delete (actually gone).
 * result=keep_* / activate_* means the folio stayed in page cache.
 */
TRACE_EVENT(named_swap_reclaim_folio,
	TP_PROTO(struct address_space *mapping, pgoff_t pgoff, u64 index,
		 enum named_swap_reclaim_result result, bool dirty,
		 bool writeback, bool mapped, int mapcount, int refcount,
		 int references),

	TP_ARGS(mapping, pgoff, index, result, dirty, writeback, mapped,
		mapcount, refcount, references),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(ino_t, ino)
		__field(pgoff_t, pgoff)
		__field(u64, index)
		__field(enum named_swap_reclaim_result, result)
		__field(bool, dirty)
		__field(bool, writeback)
		__field(bool, mapped)
		__field(int, mapcount)
		__field(int, refcount)
		__field(int, references)
		__field(pid_t, pid)
		__array(char, comm, TASK_COMM_LEN)
	),

	TP_fast_assign(
		__entry->dev = mapping && mapping->host ?
			(mapping->host->i_sb ? mapping->host->i_sb->s_dev :
			 mapping->host->i_rdev) : 0;
		__entry->ino = mapping && mapping->host ?
			mapping->host->i_ino : 0;
		__entry->pgoff = pgoff;
		__entry->index = index;
		__entry->result = result;
		__entry->dirty = dirty;
		__entry->writeback = writeback;
		__entry->mapped = mapped;
		__entry->mapcount = mapcount;
		__entry->refcount = refcount;
		__entry->references = references;
		__entry->pid = current->pid;
		memcpy(__entry->comm, current->comm, TASK_COMM_LEN);
	),

	TP_printk("pid=%d comm=%s dev=%d:%d ino=%lx pgoff=%lu index=%llu result=%s dirty=%d writeback=%d mapped=%d mapcount=%d refcount=%d refs=%d",
		__entry->pid, __entry->comm,
		MAJOR(__entry->dev), MINOR(__entry->dev), __entry->ino,
		__entry->pgoff, __entry->index,
		__print_symbolic(__entry->result, NAMED_SWAP_RECLAIM_RESULTS),
		__entry->dirty, __entry->writeback, __entry->mapped,
		__entry->mapcount, __entry->refcount, __entry->references)
);

#endif /* _TRACE_NAMED_SWAP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
