// SPDX-License-Identifier: GPL-2.0
/*
 * Named swap backing files.
 *
 * Creates a root directory and one backing file per anon_vma under
 * <pool_dir>/<index>.  Storage mode (fs / swap / hybrid), device, and
 * directories are owned by named_swap_storage.c.
 *
 * Enable is deferred until the caller is on a real root (not initramfs
 * ramfs/tmpfs) and each configured pool directory is on a writable,
 * non-ephemeral filesystem.  Swap/hybrid also wait until named_swap.root
 * is mounted on named_swap.device.  Earlier anonymous mmaps stay ordinary
 * anon.
 */
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/dcache.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/filelock.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/fsnotify.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/hash.h>
#include <linux/ktime.h>
#include <linux/ptrace.h>
#include <linux/sched/debug.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/init_task.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/overflow.h>
#include <linux/rmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/mman.h>
#define CREATE_TRACE_POINTS
#include <trace/events/named_swap.h>
#undef CREATE_TRACE_POINTS
#include <linux/mm_inline.h>
#include <linux/memcontrol.h>
#include <linux/huge_mm.h>
#include <linux/highmem.h>
#include <linux/pagewalk.h>
#include <linux/buffer_head.h>
#include <linux/backing-dev.h>
#include "internal.h"

/*
 * Facade struct file (named_swap_fops) over a real backing file on disk.
 * Page cache and I/O use the lower file; the wrapper carries named_swap hooks.
 */
struct named_swap_file {
	struct file *lower;
	spinlock_t bind_lock;
	u64 index;
	unsigned long nr_pages;
	struct mutex size_lock;
	u64 size_gen;
	bool holes_dirty;
	loff_t vfs_size;
	enum named_swap_storage_pool pool;
};

static DEFINE_XARRAY(named_swap_files);
static DEFINE_MUTEX(named_swap_xa_lock);

enum named_swap_resize_op {
	NAMED_SWAP_RESIZE_ENLARGE,
	NAMED_SWAP_RESIZE_SHRINK,
	NAMED_SWAP_RESIZE_DEALLOC,
	NAMED_SWAP_RESIZE_UNCOMMIT,
	NAMED_SWAP_RESIZE_ALLOC,
};

int named_swap_debug = NAMED_SWAP_DBG_HIST | NAMED_SWAP_DBG_SEGV |
		       NAMED_SWAP_DBG_ASSERT;
EXPORT_SYMBOL_GPL(named_swap_debug);

#define NAMED_SWAP_HIST_N	512

struct named_swap_hist_ent {
	u64			ktime_ns;
	u32			op;
	int			ret;
	pid_t			pid;
	pid_t			tgid;
	char			comm[TASK_COMM_LEN];
	unsigned long		vma_start;
	unsigned long		vma_end;
	unsigned long		vm_flags;
	unsigned long		pgoff;
	unsigned long		addr;
	unsigned long		delta;
	loff_t			old_size;
	loff_t			new_size;
	u64			index;
};

static struct named_swap_hist_ent named_swap_hist[NAMED_SWAP_HIST_N];
static atomic64_t named_swap_hist_seq;

static const char *named_swap_resize_name(u32 op)
{
	switch (op) {
	case NAMED_SWAP_RESIZE_ENLARGE:
		return "enlarge";
	case NAMED_SWAP_RESIZE_SHRINK:
		return "shrink";
	case NAMED_SWAP_RESIZE_DEALLOC:
		return "dealloc";
	case NAMED_SWAP_RESIZE_UNCOMMIT:
		return "uncommit";
	case NAMED_SWAP_RESIZE_ALLOC:
		return "alloc";
	default:
		return "?";
	}
}

static loff_t named_swap_debug_isize(struct file *file)
{
	struct file *lower;

	if (!file)
		return -EINVAL;
	lower = named_swap_lower(file);
	if (!lower)
		return -EINVAL;
	return i_size_read(file_inode(lower));
}

static u64 named_swap_vma_index(struct vm_area_struct *vma)
{
	u64 index = NAMED_SWAP_INDEX_NONE;

	if (vma && vma->vm_file)
		named_swap_file_index(vma->vm_file, &index);
	return index;
}

static void named_swap_hist_record(struct vm_area_struct *vma,
				   enum named_swap_resize_op op,
				   unsigned long addr, unsigned long delta,
				   loff_t old_size, loff_t new_size,
				   u64 index, int ret)
{
	struct named_swap_hist_ent *e;
	u64 seq;

	if (!vma)
		return;

	if (named_swap_debug & NAMED_SWAP_DBG_PRINT)
		pr_info("named_swap %s pid=%d comm=%s vma=%lx-%lx flags=%lx pgoff=%lx addr=%lx delta=%lu old=%lld new=%lld index=%llu ret=%d\n",
			named_swap_resize_name(op), current->pid, current->comm,
			vma->vm_start, vma->vm_end, vma->vm_flags, vma->vm_pgoff,
			addr, delta, old_size, new_size, index, ret);

	if (named_swap_debug & NAMED_SWAP_DBG_STACK)
		dump_stack();

	if (!(named_swap_debug & NAMED_SWAP_DBG_HIST))
		return;

	seq = atomic64_inc_return(&named_swap_hist_seq) - 1;
	e = &named_swap_hist[seq & (NAMED_SWAP_HIST_N - 1)];
	e->ktime_ns = ktime_get_ns();
	e->op = op;
	e->ret = ret;
	e->pid = current->pid;
	e->tgid = current->tgid;
	memcpy(e->comm, current->comm, TASK_COMM_LEN);
	e->vma_start = vma->vm_start;
	e->vma_end = vma->vm_end;
	e->vm_flags = vma->vm_flags;
	e->pgoff = vma->vm_pgoff;
	e->addr = addr;
	e->delta = delta;
	e->old_size = old_size;
	e->new_size = new_size;
	e->index = index;
}

void named_swap_check_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma;
	loff_t off, sz;

	if (!(named_swap_debug & NAMED_SWAP_DBG_ASSERT) || !vmf || !vmf->vma)
		return;
	vma = vmf->vma;
	if (!vma_is_named_swap(vma) || !vma->vm_file)
		return;
	off = (loff_t)linear_page_index(vma, vmf->address) << PAGE_SHIFT;
	sz = named_swap_debug_isize(vma->vm_file);
	if (sz >= 0 && off >= sz) {
		pr_err("named_swap ASSERT fault past i_size pid=%d comm=%s addr=%lx off=%lld i_size=%lld vma=%lx-%lx pgoff=%lx index=%llu\n",
		       current->pid, current->comm, vmf->address, off, sz,
		       vma->vm_start, vma->vm_end, vma->vm_pgoff,
		       named_swap_vma_index(vma));
		dump_stack();
	}
}
EXPORT_SYMBOL_GPL(named_swap_check_fault);

static void named_swap_debug_dump_vma(const char *tag, unsigned long addr,
				      struct vm_area_struct *vma)
{
	u64 index = NAMED_SWAP_INDEX_NONE;
	loff_t isz = -1;

	if (!vma) {
		pr_err("named_swap %s addr=%lx vma=NULL\n", tag, addr);
		return;
	}
	if (vma->vm_file) {
		named_swap_file_index(vma->vm_file, &index);
		isz = named_swap_debug_isize(vma->vm_file);
	}
	pr_err("named_swap %s addr=%lx vma=%px %lx-%lx flags=%lx pgoff=%lx index=%llu i_size=%lld named=%d file=%px\n",
	       tag, addr, vma, vma->vm_start, vma->vm_end, vma->vm_flags,
	       vma->vm_pgoff, index, isz,
	       vma_is_named_swap(vma), vma->vm_file);
}

static void named_swap_debug_dump_hist(pid_t tgid)
{
	u64 seq = atomic64_read(&named_swap_hist_seq);
	unsigned int i, n, printed = 0;

	n = min_t(u64, seq, NAMED_SWAP_HIST_N);
	pr_err("named_swap hist last %u ops (seq=%llu) matching tgid=%d\n",
	       n, seq, tgid);
	for (i = 0; i < n; i++) {
		const struct named_swap_hist_ent *e;
		u64 idx = seq - n + i;

		e = &named_swap_hist[idx & (NAMED_SWAP_HIST_N - 1)];
		if (tgid && e->tgid != tgid)
			continue;
		pr_err("  [%u] %s pid=%d comm=%s vma=%lx-%lx flags=%lx pgoff=%lx addr=%lx delta=%lu old=%lld new=%lld index=%llu ret=%d\n",
		       printed, named_swap_resize_name(e->op), e->pid, e->comm,
		       e->vma_start, e->vma_end, e->vm_flags, e->pgoff,
		       e->addr, e->delta, e->old_size, e->new_size,
		       e->index, e->ret);
		if (++printed >= 64)
			break;
	}
	if (!printed)
		pr_err("  (no hist entries for tgid=%d)\n", tgid);
}

#define NS_WRITE_SLOTS		8192

struct ns_write_slot {
	u64		index;
	pgoff_t		pgoff;
	pid_t		tgid;
	u8		state;
};

static struct ns_write_slot ns_writes[NS_WRITE_SLOTS];

static struct ns_write_slot *ns_write_slot(u64 index, pgoff_t pgoff)
{
	return &ns_writes[hash_64(index ^ ((u64)pgoff << 1), 13) &
			  (NS_WRITE_SLOTS - 1)];
}

void named_swap_note_written(struct vm_area_struct *vma, unsigned long addr)
{
	struct ns_write_slot *s;
	u64 index;
	pgoff_t pgoff;

	if (!(named_swap_debug & NAMED_SWAP_DBG_ASSERT) ||
	    !vma_is_named_swap(vma) || named_swap_file_index(vma->vm_file, &index))
		return;
	pgoff = linear_page_index(vma, addr);
	s = ns_write_slot(index, pgoff);
	s->index = index;
	s->pgoff = pgoff;
	s->tgid = current->tgid;
	s->state = 1;
}
EXPORT_SYMBOL_GPL(named_swap_note_written);

void named_swap_check_zero_read(struct vm_area_struct *vma, unsigned long addr)
{
	struct ns_write_slot *s;
	u64 index;
	pgoff_t pgoff;

	if (!(named_swap_debug & NAMED_SWAP_DBG_ASSERT) ||
	    !vma_is_named_swap(vma) || named_swap_file_index(vma->vm_file, &index))
		return;
	pgoff = linear_page_index(vma, addr);
	s = ns_write_slot(index, pgoff);
	if (s->index != index || s->pgoff != pgoff || s->state != 1)
		return;
	pr_err("named_swap ASSERT zero-read after write pid=%d comm=%s addr=%lx index=%llu pgoff=%lx tgid_wrote=%d\n",
	       current->pid, current->comm, addr, index, pgoff, s->tgid);
	dump_stack();
}
EXPORT_SYMBOL_GPL(named_swap_check_zero_read);

int named_swap_hist_show(struct seq_file *m, void *v)
{
	u64 seq = atomic64_read(&named_swap_hist_seq);
	unsigned int i, n;

	n = min_t(u64, seq, NAMED_SWAP_HIST_N);
	seq_printf(m, "seq=%llu debug=0x%x showing %u\n",
		   seq, named_swap_debug, n);
	for (i = 0; i < n; i++) {
		const struct named_swap_hist_ent *e;
		u64 idx = seq - n + i;

		e = &named_swap_hist[idx & (NAMED_SWAP_HIST_N - 1)];
		seq_printf(m, "%s pid=%d tgid=%d comm=%s vma=%lx-%lx flags=%lx pgoff=%lx addr=%lx delta=%lu old=%lld new=%lld index=%llu ret=%d\n",
			   named_swap_resize_name(e->op), e->pid, e->tgid,
			   e->comm, e->vma_start, e->vma_end, e->vm_flags,
			   e->pgoff, e->addr, e->delta, e->old_size,
			   e->new_size, e->index, e->ret);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(named_swap_hist_show);

void named_swap_debug_user_segv(struct pt_regs *regs, unsigned long address,
				unsigned long error_code, int si_code,
				struct vm_area_struct *vma)
{
	static atomic_t dumps;
	unsigned long ip;
	u64 index = NAMED_SWAP_INDEX_NONE;
	int dump_n;

	if (!(named_swap_debug & NAMED_SWAP_DBG_SEGV))
		return;
	if (!user_mode(regs))
		return;

	dump_n = atomic_inc_return(&dumps);
	if (dump_n > 16 && !printk_ratelimit())
		return;

	ip = instruction_pointer(regs);
	if (vma)
		index = named_swap_vma_index(vma);

	pr_err("named_swap SEGV #%d pid=%d tgid=%d comm=%s addr=%lx ip=%lx err=0x%lx si=%d index=%llu\n",
	       dump_n, current->pid, current->tgid, current->comm,
	       address, ip, error_code, si_code, index);
	named_swap_debug_dump_vma("fault_vma", address, vma);
	named_swap_debug_dump_hist(current->tgid);
	dump_stack();
}
EXPORT_SYMBOL_GPL(named_swap_debug_user_segv);

static int __init named_swap_debug_setup(char *str)
{
	unsigned long val;
	int ret;

	if (!str)
		return 0;
	ret = kstrtoul(str, 0, &val);
	if (!ret) {
		named_swap_debug = (int)val;
		pr_info("named_swap.debug=0x%x\n", named_swap_debug);
	}
	return 1;
}
__setup("named_swap.debug=", named_swap_debug_setup);


static struct file *named_swap_file_peek(u64 index);

static struct file *named_swap_lower(struct file *file)
{
	struct named_swap_file *ns = file->private_data;

	if (ns && ns->lower)
		return ns->lower;
	return file;
}

char *named_swap_file_path(struct file *file, char *buf, int buflen)
{
	struct named_swap_file *ns;

	if (!file)
		return ERR_PTR(-EINVAL);

	ns = file->private_data;
	if (!ns || !ns->lower)
		return file_path(file, buf, buflen);

	return file_path(ns->lower, buf, buflen);
}
EXPORT_SYMBOL_GPL(named_swap_file_path);

/* Indices are stored in the swap PTE offset field (see swp_offset()). */
#define NAMED_SWAP_INDEX_MAX	SWP_OFFSET_MASK

static bool named_swap_enabled;
static DEFINE_MUTEX(named_swap_lock);
static struct inode *named_swap_root_inode;
static atomic64_t named_swap_index_counter = ATOMIC64_INIT(0);

int named_swap_min_vma_size = 256; /* 256 pages = 1MB */
EXPORT_SYMBOL_GPL(named_swap_min_vma_size);

/* Default matches today's file writeback + dirty throttle. */
int named_swap_flush = NAMED_SWAP_FLUSH_FILE;
EXPORT_SYMBOL_GPL(named_swap_flush);

static int __init named_swap_flush_setup(char *str)
{
	int mode;

	if (!str || kstrtoint(str, 0, &mode) ||
	    mode < NAMED_SWAP_FLUSH_ANON || mode > NAMED_SWAP_FLUSH_FILE) {
		pr_warn("named_swap.flush: invalid value '%s', keeping %d\n",
			str ? str : "", named_swap_flush);
		return 0;
	}
	named_swap_flush = mode;
	pr_info("named_swap.flush: using %d\n", named_swap_flush);
	return 1;
}
__setup("named_swap.flush=", named_swap_flush_setup);

static int named_swap_cache_root_inode(void);

/*
 * Allocate the next named swap file index.  Indices must fit in
 * SWP_OFFSET_MASK so they can be encoded in swap PTE offsets.
 */
static int get_named_swap_file_index(u64 *index)
{
	u64 idx = atomic64_fetch_add(1, &named_swap_index_counter);

	if (unlikely(idx > NAMED_SWAP_INDEX_MAX)) {
		atomic64_dec(&named_swap_index_counter);
		return -EOVERFLOW;
	}

	*index = idx;
	return 0;
}

struct named_swap_readdir {
	struct dir_context ctx;
	char name[NAME_MAX + 1];
	unsigned int type;
	bool found;
};

static struct file *named_swap_file_peek(u64 index)
{
	XA_STATE(xas, &named_swap_files, index);
	struct file *file;

	xa_lock(&named_swap_files);
	file = xas_load(&xas);
	xa_unlock(&named_swap_files);
	return file;
}

static bool named_swap_file_matches(struct file *file, u64 index)
{
	struct named_swap_file *ns;

	if (!file || !mapping_named_swap(file->f_mapping))
		return false;

	ns = file->private_data;
	return ns && ns->index == index;
}

int named_swap_file_index(struct file *file, u64 *index)
{
	struct named_swap_file *ns;

	if (!file || !mapping_named_swap(file->f_mapping))
		return -EINVAL;

	ns = file->private_data;
	if (!ns)
		return -EINVAL;

	*index = ns->index;
	return 0;
}
EXPORT_SYMBOL_GPL(named_swap_file_index);

u64 named_swap_mapping_index(struct address_space *mapping)
{
	struct anon_vma *anon_vma;
	struct file *file;
	u64 index = NAMED_SWAP_INDEX_NONE;

	if (!mapping || !mapping_named_swap(mapping))
		return index;

	anon_vma = READ_ONCE(mapping->anon_vma);
	if (!anon_vma)
		return index;

	file = READ_ONCE(anon_vma->named_swap_file);
	if (file && !named_swap_file_index(file, &index))
		return index;
	return NAMED_SWAP_INDEX_NONE;
}
EXPORT_SYMBOL_GPL(named_swap_mapping_index);

static int named_swap_xa_insert(struct file *file, u64 index)
{
	struct file *stored;
	int err;

	stored = get_file(file);
	mutex_lock(&named_swap_xa_lock);
	err = xa_err(xa_store(&named_swap_files, index, stored, GFP_KERNEL));
	mutex_unlock(&named_swap_xa_lock);
	if (err)
		fput(stored);
	return err;
}

static void named_swap_xa_remove(u64 index)
{
	struct file *file;

	mutex_lock(&named_swap_xa_lock);
	file = xa_erase(&named_swap_files, index);
	mutex_unlock(&named_swap_xa_lock);
	if (file)
		fput(file);
}

static void named_swap_xa_erase(u64 index)
{
	mutex_lock(&named_swap_xa_lock);
	xa_erase(&named_swap_files, index);
	mutex_unlock(&named_swap_xa_lock);
}

/*
 * Drop the xarray pin on a named-swap file that was prepared for mmap but
 * never installed as vma->vm_file (typically because the range merged into
 * an adjacent named-swap VMA). The caller still owns its struct file ref.
 */
void named_swap_drop_prepared_file(struct file *file)
{
	u64 index;

	if (!file || named_swap_file_index(file, &index))
		return;
	named_swap_xa_remove(index);
}

static bool named_swap_range_mapped(struct address_space *mapping,
				    pgoff_t start, pgoff_t last)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

	if (!mapping || last < start)
		return false;

	rcu_read_lock();
	xas_for_each(&xas, folio, last) {
		if (xas_retry(&xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;
		if (folio_mapped(folio)) {
			rcu_read_unlock();
			return true;
		}
	}
	rcu_read_unlock();
	return false;
}

loff_t named_swap_file_blocks(struct file *file) {
    struct file *lower;

    if (!file)
        return -EINVAL;

    lower = named_swap_lower(file);
    if (!lower)
        return -EINVAL;

    /* Return the number of 512-byte blocks allocated to the lower inode */
    return file_inode(lower)->i_blocks;
}
EXPORT_SYMBOL_GPL(named_swap_file_blocks);

loff_t named_swap_file_size(struct file *file){
	struct file *lower;

    if (!file)
        return -EINVAL;

    lower = named_swap_lower(file);
    if (!lower)
        return -EINVAL;

    // Use file_inode() to safely get the inode, 
    // and return it as a 64-bit loff_t
        return i_size_read(file_inode(lower));

}
EXPORT_SYMBOL(named_swap_file_size);

/*
 * Backing-file mutations are kernel-owned.  vfs_truncate() and
 * vfs_fallocate() revalidate the caller's LSM (AppArmor path_truncate /
 * file_permission) and deny /nswap even for root-confined daemons.
 */
static int named_swap_truncate_lower(struct file *lower, loff_t new_size)
{
	struct path *path = &lower->f_path;
	struct inode *inode = file_inode(lower);
	struct iattr newattrs = {
		.ia_size = new_size,
		.ia_valid = ATTR_SIZE | ATTR_FORCE,
	};
	const struct cred *old;
	int ret;

	if (new_size < 0)
		return -EINVAL;

	old = override_creds(&init_cred);
	ret = mnt_want_write(path->mnt);
	if (ret)
		goto out_creds;
	ret = get_write_access(inode);
	if (ret)
		goto out_drop_write;
	ret = break_lease(inode, O_WRONLY);
	if (ret)
		goto out_put_write;
	inode_lock(inode);
	ret = notify_change(mnt_idmap(path->mnt), path->dentry, &newattrs,
			    NULL);
	inode_unlock(inode);
out_put_write:
	put_write_access(inode);
out_drop_write:
	mnt_drop_write(path->mnt);
out_creds:
	revert_creds(old);
	return ret;
}

static long named_swap_fallocate_lower(struct file *lower, int mode,
				       loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(lower);
	const struct cred *old;
	loff_t sum;
	long ret;

	if (offset < 0 || len <= 0)
		return -EINVAL;
	if (!(lower->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!lower->f_op->fallocate)
		return -EOPNOTSUPP;
	if (check_add_overflow(offset, len, &sum))
		return -EFBIG;
	if (sum > inode->i_sb->s_maxbytes)
		return -EFBIG;

	old = override_creds(&init_cred);
	file_start_write(lower);
	ret = lower->f_op->fallocate(lower, mode, offset, len);
	if (!ret)
		fsnotify_modify(lower);
	file_end_write(lower);
	revert_creds(old);
	return ret;
}

static void named_swap_account_unreserve(struct named_swap_file *ns,
					 unsigned long pages)
{
	if (!ns || !pages)
		return;
	if (pages > ns->nr_pages)
		pages = ns->nr_pages;
	ns->nr_pages -= pages;
	named_swap_storage_release(pages, ns->pool);
}

/*
 * Charge only growth beyond ns->nr_pages. Create already reserved the
 * initial size; vfs_fallocate on the wrapper must not reserve it again.
 */
static int named_swap_account_reserve_end(struct named_swap_file *ns,
					  loff_t new_end, unsigned long *charged)
{
	unsigned long need;
	unsigned long extra;
	int ret;

	*charged = 0;
	if (!ns || new_end <= 0)
		return 0;
	need = DIV_ROUND_UP((unsigned long)new_end, PAGE_SIZE);
	if (need <= ns->nr_pages)
		return 0;
	extra = need - ns->nr_pages;
	*charged = extra;
	ret = named_swap_storage_reserve_pool(extra, ns->pool);
	if (ret)
		return ret;
	return 0;
}

struct named_swap_dirty {
	struct list_head list;
	struct task_struct *task;
	struct file *file;
};

static LIST_HEAD(named_swap_dirty_pending);
static DEFINE_SPINLOCK(named_swap_dirty_lock);

struct named_swap_cover {
	loff_t start;
	loff_t end;
};

static void named_swap_file_mark_dirty(struct file *file)
{
	struct named_swap_dirty *d, *old;

	if (!file)
		return;

	spin_lock(&named_swap_dirty_lock);
	list_for_each_entry(old, &named_swap_dirty_pending, list) {
		if (old->task == current && old->file == file) {
			spin_unlock(&named_swap_dirty_lock);
			return;
		}
	}
	spin_unlock(&named_swap_dirty_lock);

	d = kmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return;
	d->task = current;
	d->file = get_file(file);

	spin_lock(&named_swap_dirty_lock);
	list_for_each_entry(old, &named_swap_dirty_pending, list) {
		if (old->task == current && old->file == file) {
			spin_unlock(&named_swap_dirty_lock);
			fput(d->file);
			kfree(d);
			return;
		}
	}
	list_add_tail(&d->list, &named_swap_dirty_pending);
	spin_unlock(&named_swap_dirty_lock);
}

static void named_swap_file_publish_size(struct file *file, struct file *lower,
					 struct named_swap_file *ns,
					 loff_t new_size)
{
	i_size_write(file_inode(file), new_size);
	i_size_write(file_inode(lower), new_size);
	if (ns)
		ns->size_gen++;
}

static loff_t named_swap_vma_file_end(const struct vm_area_struct *vma)
{
	return ((loff_t)vma->vm_pgoff << PAGE_SHIFT) +
	       (vma->vm_end - vma->vm_start);
}

static loff_t named_swap_file_needed(struct mm_struct *mm, struct file *file)
{
	struct vm_area_struct *tmp;
	loff_t need = 0;
	VMA_ITERATOR(vmi, mm, 0);

	if (!mm || !file)
		return 0;

	for_each_vma(vmi, tmp) {
		loff_t end;

		if (!tmp->vm_file || tmp->vm_file->f_mapping != file->f_mapping)
			continue;
		if (!vma_pages(tmp))
			continue;
		end = named_swap_vma_file_end(tmp);
		if (end > need)
			need = end;
	}
	return need;
}

/*
 * mmap write (or a lock that blocks mmap write) is held. Reserve or
 * unreserve, publish i_size, mark dirty. No VFS.
 */
static int named_swap_file_set_size(struct file *file, loff_t new_size,
				    bool sparse_ok)
{
	struct file *lower;
	struct named_swap_file *ns;
	loff_t old_size;
	unsigned long pages = 0;
	int ret = 0;

	if (!file || new_size < 0)
		return -EINVAL;

	ns = file->private_data;
	lower = named_swap_lower(file);
	if (!lower)
		return -EINVAL;

	if (ns)
		mutex_lock(&ns->size_lock);

	old_size = i_size_read(file_inode(lower));
	if (new_size == old_size)
		goto out_unlock;

	if (new_size > old_size && ns && ns->artifact) {
		ret = -EPERM;
		goto out_unlock;
	}

	if (new_size > old_size) {
		ret = named_swap_account_reserve_end(ns, new_size, &pages);
		if (ret) {
			if (!sparse_ok || (ret != -ENOSPC && ret != -EDQUOT)) {
				if (ns)
					pr_warn_ratelimited(
						"named_swap_file_set_size: reserve %lu pages pool=%d file_pages=%lu err=%d\n",
						pages, ns->pool, ns->nr_pages,
						ret);
				goto out_unlock;
			}
			ret = 0;
			pages = 0;
		}
		if (ns && pages)
			ns->nr_pages += pages;
	} else if (ns) {
		named_swap_account_unreserve(ns,
			DIV_ROUND_UP((unsigned long)(old_size - new_size),
				     PAGE_SIZE));
	}

	named_swap_file_publish_size(file, lower, ns, new_size);
	if (ns)
		mutex_unlock(&ns->size_lock);
	named_swap_file_mark_dirty(file);
	return 0;

out_unlock:
	if (ns)
		mutex_unlock(&ns->size_lock);
	return ret;
}

/*
 * mmap is not held. Monotonic VFS grow: never shrinks, never lowers
 * another thread's i_size.
 */
static int __maybe_unused named_swap_file_grow(struct file *file, loff_t new_size)
{
	struct file *lower;
	struct named_swap_file *ns;
	loff_t old_size;
	unsigned long pages = 0;
	int ret = 0;

	if (!file || new_size <= 0)
		return new_size < 0 ? -EINVAL : 0;

	ns = file->private_data;
	if (ns && ns->artifact)
		return -EPERM;
	lower = named_swap_lower(file);
	if (!lower)
		return -EINVAL;

	if (ns)
		mutex_lock(&ns->size_lock);

	old_size = i_size_read(file_inode(lower));
	if (new_size <= old_size)
		goto out_unlock;

	ret = named_swap_account_reserve_end(ns, new_size, &pages);
	if (ret)
		goto out_unlock;
	if (ns && pages)
		ns->nr_pages += pages;

	ret = named_swap_fallocate_lower(lower, 0, 0, new_size);
	if (ret && ret != -ENOSPC && ret != -EDQUOT && ret != -EOPNOTSUPP &&
	    ret != -EROFS) {
		if (ns && pages)
			named_swap_account_unreserve(ns, pages);
		goto out_unlock;
	}
	named_swap_file_publish_size(file, lower, ns, new_size);
	if (ns)
		ns->vfs_size = new_size;
	ret = 0;

out_unlock:
	if (ns)
		mutex_unlock(&ns->size_lock);
	return ret;
}

static int named_swap_cover_cmp(const void *a, const void *b)
{
	const struct named_swap_cover *x = a;
	const struct named_swap_cover *y = b;

	if (x->start < y->start)
		return -1;
	if (x->start > y->start)
		return 1;
	return 0;
}

static bool named_swap_file_vma_covers(struct mm_struct *mm, struct file *file,
				       loff_t start, loff_t end)
{
	struct vm_area_struct *tmp;
	VMA_ITERATOR(vmi, mm, 0);

	if (!mm || !file || end <= start)
		return false;

	for_each_vma(vmi, tmp) {
		loff_t a, b;

		if (!tmp->vm_file || tmp->vm_file->f_mapping != file->f_mapping)
			continue;
		if (!vma_pages(tmp))
			continue;
		a = (loff_t)tmp->vm_pgoff << PAGE_SHIFT;
		b = named_swap_vma_file_end(tmp);
		if (a < end && b > start)
			return true;
	}
	return false;
}

static void named_swap_file_punch(struct file *file, loff_t start, loff_t len)
{
	struct file *lower;
	struct named_swap_file *ns;
	struct mm_struct *mm = current->mm;

	if (!file || len <= 0)
		return;
	lower = named_swap_lower(file);
	if (!lower)
		return;
	if (mm) {
		mmap_read_lock(mm);
		if (named_swap_file_vma_covers(mm, file, start, start + len)) {
			mmap_read_unlock(mm);
			return;
		}
		mmap_read_unlock(mm);
	}
	if (named_swap_range_mapped(file->f_mapping, start >> PAGE_SHIFT,
				    (start + len - 1) >> PAGE_SHIFT))
		return;
	lru_add_drain_all();
	if (named_swap_fallocate_lower(lower,
				       FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				       start, len))
		return;
	ns = file->private_data;
	if (ns) {
		mutex_lock(&ns->size_lock);
		named_swap_account_unreserve(ns,
			DIV_ROUND_UP((unsigned long)len, PAGE_SIZE));
		mutex_unlock(&ns->size_lock);
	}
}

static void named_swap_file_sync_holes(struct file *file)
{
	struct mm_struct *mm = current->mm;
	struct named_swap_cover stack[16];
	struct named_swap_cover *cover = stack;
	struct vm_area_struct *tmp;
	loff_t isz;
	int n = 0, cap = ARRAY_SIZE(stack), i;
	bool truncated = false;

	if (!file || !mm)
		return;

	mmap_read_lock(mm);
	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, tmp) {
			if (!tmp->vm_file ||
			    tmp->vm_file->f_mapping != file->f_mapping)
				continue;
			if (vma_pages(tmp))
				n++;
		}
	}
	mmap_read_unlock(mm);

	if (n > ARRAY_SIZE(stack)) {
		cap = n + 8;
		cover = kmalloc_array(cap, sizeof(*cover), GFP_KERNEL);
		if (!cover)
			return;
	}

	n = 0;
	mmap_read_lock(mm);
	isz = named_swap_file_size(file);
	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, tmp) {
			if (!tmp->vm_file ||
			    tmp->vm_file->f_mapping != file->f_mapping)
				continue;
			if (!vma_pages(tmp))
				continue;
			if (n == cap) {
				truncated = true;
				break;
			}
			cover[n].start = (loff_t)tmp->vm_pgoff << PAGE_SHIFT;
			cover[n].end = named_swap_vma_file_end(tmp);
			n++;
		}
	}
	mmap_read_unlock(mm);

	if (truncated) {
		if (cover != stack)
			kfree(cover);
		return;
	}

	if (isz <= 0)
		goto out_free;
	if (!n) {
		named_swap_file_punch(file, 0, isz);
		goto out_free;
	}

	sort(cover, n, sizeof(*cover), named_swap_cover_cmp, NULL);
	{
		int w = 0;

		for (i = 1; i < n; i++) {
			if (cover[i].start <= cover[w].end) {
				if (cover[i].end > cover[w].end)
					cover[w].end = cover[i].end;
			} else {
				w++;
				cover[w] = cover[i];
			}
		}
		n = w + 1;
	}

	if (cover[0].start > 0)
		named_swap_file_punch(file, 0, cover[0].start);
	for (i = 0; i < n - 1; i++) {
		if (cover[i + 1].start > cover[i].end)
			named_swap_file_punch(file, cover[i].end,
					      cover[i + 1].start - cover[i].end);
	}
	if (isz > cover[n - 1].end)
		named_swap_file_punch(file, cover[n - 1].end,
				      isz - cover[n - 1].end);

out_free:
	if (cover != stack)
		kfree(cover);
}

static void named_swap_file_sync(struct file *file)
{
	struct file *lower;
	struct named_swap_file *ns;
	loff_t target;
	bool holes = false;
	int ret;

	if (!file)
		return;
	ns = file->private_data;
	lower = named_swap_lower(file);
	if (!lower)
		return;

	if (ns)
		mutex_lock(&ns->size_lock);

	target = i_size_read(file_inode(lower));
	if (ns) {
		holes = ns->holes_dirty;
		ns->holes_dirty = false;
	}

	/*
	 * i_size is published under mmap before VFS. Always fallocate
	 * 0..target so PROT_NONE→RW (i_size already covers) gets blocks.
	 * Truncate only when i_size shrank: ext4 setattr to the current
	 * i_size still runs truncate_pagecache() and drains lru_add.
	 */
	if (target > 0) {
		ret = named_swap_fallocate_lower(lower, 0, 0, target);
		if (ret && ret != -ENOSPC && ret != -EDQUOT &&
		    ret != -EOPNOTSUPP && ret != -EROFS)
			pr_warn_ratelimited(
				"named_swap_file_sync: fallocate %lld err=%d\n",
				target, ret);
		else if (ns && target > ns->vfs_size)
			ns->vfs_size = target;
	}
	if (ns && target < ns->vfs_size) {
		lru_add_drain_all();
		ret = named_swap_truncate_lower(lower, target);
		if (!ret) {
			i_size_write(file_inode(file),
				     i_size_read(file_inode(lower)));
			ns->vfs_size = target;
		} else {
			pr_warn_ratelimited(
				"named_swap_file_sync: truncate %lld err=%d\n",
				target, ret);
		}
	} else if (!ns) {
		ret = named_swap_truncate_lower(lower, target);
		if (!ret)
			i_size_write(file_inode(file),
				     i_size_read(file_inode(lower)));
		else
			pr_warn_ratelimited(
				"named_swap_file_sync: truncate %lld err=%d\n",
				target, ret);
	}

	if (ns)
		mutex_unlock(&ns->size_lock);

	if (holes)
		named_swap_file_sync_holes(file);
}

void named_swap_fs_flush(void)
{
	LIST_HEAD(local);
	struct named_swap_dirty *d, *tmp;

	spin_lock(&named_swap_dirty_lock);
	if (list_empty(&named_swap_dirty_pending)) {
		spin_unlock(&named_swap_dirty_lock);
		return;
	}
	list_for_each_entry_safe(d, tmp, &named_swap_dirty_pending, list) {
		if (d->task == current)
			list_move_tail(&d->list, &local);
	}
	spin_unlock(&named_swap_dirty_lock);

	list_for_each_entry_safe(d, tmp, &local, list) {
		list_del(&d->list);
		named_swap_file_sync(d->file);
		fput(d->file);
		kfree(d);
	}
}

int named_swap_enlarge(struct vm_area_struct *vma, unsigned long delta)
{
	struct file *file;
	loff_t old_size;
	int ret;

	if (!vma || !vma->vm_file)
		return -EINVAL;

	file = vma->vm_file;
	old_size = named_swap_file_size(file);
	if (old_size < 0)
		return old_size;

	ret = named_swap_file_set_size(file, old_size + (loff_t)delta, false);
	named_swap_hist_record(vma, NAMED_SWAP_RESIZE_ENLARGE, vma->vm_start,
			       delta, old_size,
			       ret ? old_size : old_size + (loff_t)delta,
			       named_swap_vma_index(vma), ret);
	if (!ret)
		named_swap_assert_vma_size(vma, "enlarge");
	return ret;
}

int named_swap_shrink(struct vm_area_struct *vma, unsigned long delta)
{
	struct file *file;
	loff_t old_size;
	loff_t needed;
	int ret;

	if (!vma || !vma->vm_file)
		return -EINVAL;

	file = vma->vm_file;
	old_size = named_swap_file_size(file);
	if (old_size < 0)
		return old_size;

	/*
	 * Retreat uses live VMA need, not remembered old_size. A failed
	 * merge must not truncate a file another VMA still needs.
	 */
	needed = named_swap_file_needed(vma->vm_mm, file);
	ret = named_swap_file_set_size(file, needed, false);
	named_swap_hist_record(vma, NAMED_SWAP_RESIZE_SHRINK, vma->vm_start,
			       delta, old_size, needed,
			       named_swap_vma_index(vma), ret);
	return ret;
}

int named_swap_deallocate(struct vm_area_struct *vma, unsigned long start,
			  unsigned long end)
{
	struct file *file;
	struct file *lower;
	loff_t offset;
	loff_t len;
	long ret;

	if (!vma || start >= end)
		return -EINVAL;

	file = vma->vm_file;
	if (!file)
		return -EINVAL;

	lower = named_swap_lower(file);
	if (!lower)
		return -EINVAL;

	offset = ((loff_t)start - vma->vm_start) +
		 ((loff_t)vma->vm_pgoff << PAGE_SHIFT);
	len = (loff_t)(end - start);

	if (named_swap_range_mapped(file->f_mapping,
				    offset >> PAGE_SHIFT,
				    (offset + len - 1) >> PAGE_SHIFT))
		return 0;

	named_swap_assert_drop_rw(vma, file, offset >> PAGE_SHIFT,
				  (offset + len - 1) >> PAGE_SHIFT,
				  "dealloc-punch");

	ret = named_swap_fallocate_lower(lower,
			     FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			     offset, len);
	named_swap_hist_record(vma, NAMED_SWAP_RESIZE_DEALLOC, start,
			       end - start, named_swap_file_size(file),
			       named_swap_file_size(file),
			       named_swap_vma_index(vma), ret);
	if (ret)
		return ret;
	named_swap_account_unreserve(file->private_data,
				     DIV_ROUND_UP((unsigned long)len, PAGE_SIZE));
	return 0;
}

/*
 * True when another VMA in this mm still maps overlapping file offsets.
 * mremap MAYMOVE installs the dest VMA before unmapping the source; that
 * munmap must not punch or shrink the file the dest still owns.
 */
static bool named_swap_file_range_mapped_elsewhere(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct vm_area_struct *tmp;
	unsigned long nr;
	pgoff_t start, last;
	VMA_ITERATOR(vmi, mm, 0);

	if (!mm || !vma->vm_file)
		return false;
	nr = vma_pages(vma);
	if (!nr)
		return false;
	start = vma->vm_pgoff;
	last = start + nr - 1;

	for_each_vma(vmi, tmp) {
		unsigned long n = vma_pages(tmp);
		pgoff_t a, b;

		if (tmp == vma || tmp->vm_file != vma->vm_file || !n)
			continue;
		a = tmp->vm_pgoff;
		b = a + n - 1;
		if (a <= last && b >= start)
			return true;
	}
	return false;
}

/*
 * Drop backing for a named-swap VMA the same way munmap does: shrink the
 * file when this range is the tail, otherwise punch a hole and keep i_size.
 * Relocate (mremap MAYMOVE) is not a drop: skip if another VMA still maps
 * these offsets.
 */
void named_swap_uncommit_queue(struct vm_area_struct *vma)
{
	struct file *file;
	struct named_swap_file *ns;
	loff_t old_size;
	loff_t needed;

	if (!vma || !vma_is_named_swap(vma) || !vma->vm_file)
		return;

	file = vma->vm_file;
	old_size = named_swap_file_size(file);
	needed = named_swap_file_needed(vma->vm_mm, file);
	ns = file->private_data;
	if (ns) {
		mutex_lock(&ns->size_lock);
		ns->holes_dirty = true;
		mutex_unlock(&ns->size_lock);
	}
	named_swap_file_set_size(file, needed, false);
	named_swap_file_mark_dirty(file);
	named_swap_hist_record(vma, NAMED_SWAP_RESIZE_UNCOMMIT, vma->vm_start,
			       vma->vm_end - vma->vm_start, old_size, needed,
			       named_swap_vma_index(vma), 0);
}

int named_swap_uncommit(struct vm_area_struct *vma)
{
	named_swap_uncommit_queue(vma);
	return 0;
}

int named_swap_allocate_vma(struct vm_area_struct *vma,
			    unsigned long start, unsigned long end)
{
	struct file *file;
	loff_t offset;
	loff_t len;
	loff_t old_size;
	loff_t need;
	int ret = 0;

	if (!vma || !vma_is_named_swap(vma) || start < vma->vm_start ||
	    end > vma->vm_end || start >= end)
		return -EINVAL;

	file = vma->vm_file;
	if (!file)
		return -EINVAL;

	/*
	 * Only the newly writable range. After mprotect, vma_modify_flags
	 * may merge this VMA with an already-RW neighbor; growing the
	 * whole merged VMA is unnecessary if i_size already covers it.
	 */
	offset = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) +
		 (start - vma->vm_start);
	len = (loff_t)(end - start);
	need = offset + len;
	old_size = named_swap_file_size(file);
	if (old_size < 0)
		return old_size;
	if (need > old_size)
		ret = named_swap_file_set_size(file, need, true);
	else
		named_swap_file_mark_dirty(file);
	named_swap_hist_record(vma, NAMED_SWAP_RESIZE_ALLOC, start, end - start,
			       old_size, named_swap_file_size(file),
			       named_swap_vma_index(vma), ret);
	return ret;
}

static void named_swap_xa_destroy(void)
{
	struct file *file;
	unsigned long index;

	mutex_lock(&named_swap_xa_lock);
	xa_for_each(&named_swap_files, index, file) {
		xa_erase(&named_swap_files, index);
		fput(file);
	}
	mutex_unlock(&named_swap_xa_lock);
}

static pte_t named_swap_pte_for_file(struct file *file)
{
	struct named_swap_file *ns = file->private_data;

	VM_BUG_ON(!ns);
	return make_named_swap_pte(ns->index);
}

void named_swap_store_pte(struct mm_struct *mm, struct vm_area_struct *vma,
			  unsigned long address, pte_t *pte)
{
	u64 index = NAMED_SWAP_INDEX_NONE;

	VM_BUG_ON_VMA(!vma_is_named_swap(vma), vma);
	named_swap_file_index(vma->vm_file, &index);
	trace_named_swap_unmap(vma, address, index);
	set_pte_at(mm, address, pte, named_swap_pte_for_file(vma->vm_file));
}

void setup_named_swap_vmf(struct vm_fault *vmf)
{
	swp_entry_t entry;
	struct file *file;
	u64 index;

	if (!is_named_swap_pte(vmf->orig_pte))
		return;

	entry = pte_to_swp_entry(vmf->orig_pte);
	index = named_swap_entry_index(entry);
	file = vmf->vma->vm_file;
	if (!named_swap_file_matches(file, index))
		file = named_swap_file_peek(index);
	if (!file)
		return;

	vmf->vm_file = file;
	pte_unmap(vmf->pte);
	vmf->pte = NULL;
}


static bool named_swap_filldir(struct dir_context *ctx, const char *name,
			       int namelen, loff_t offset, u64 ino,
			       unsigned int type)
{
	struct named_swap_readdir *iter =
		container_of(ctx, struct named_swap_readdir, ctx);

	if (namelen == 1 && name[0] == '.')
		return true;
	if (namelen == 2 && name[0] == '.' && name[1] == '.')
		return true;
	if (namelen > NAME_MAX)
		return true;

	memcpy(iter->name, name, namelen);
	iter->name[namelen] = '\0';
	iter->type = type;
	iter->found = true;
	return false;
}

static int named_swap_first_child(const char *path,
				  struct named_swap_readdir *iter)
{
	struct file *file;
	int ret;

	memset(iter, 0, sizeof(*iter));
	iter->ctx.actor = named_swap_filldir;

	file = filp_open(path, O_RDONLY | O_DIRECTORY | O_LARGEFILE, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = iterate_dir(file, &iter->ctx);
	fput(file);
	if (ret)
		return ret;

	return iter->found ? 1 : 0;
}

static int named_swap_remove_path(const char *path, bool dir)
{
	struct filename *filename;
	struct dentry *dentry;
	struct path parent;
	struct qstr last;
	unsigned int lookup_flags = 0;
	int type;
	int ret;

retry:
	filename = getname_kernel(path);
	if (IS_ERR(filename))
		return PTR_ERR(filename);

	ret = vfs_path_parent_lookup(filename, lookup_flags, &parent, &last,
				     &type, NULL);
	if (ret)
		goto out_name;
	if (type != LAST_NORM) {
		ret = -EINVAL;
		goto out_path;
	}

	ret = mnt_want_write(parent.mnt);
	if (ret)
		goto out_path;

	inode_lock_nested(d_inode(parent.dentry), I_MUTEX_PARENT);
	dentry = lookup_one_qstr_excl(&last, parent.dentry, lookup_flags);
	if (IS_ERR(dentry)) {
		ret = PTR_ERR(dentry);
		goto out_unlock;
	}

	if (d_is_negative(dentry)) {
		ret = -ENOENT;
		goto out_dput;
	}

	if (dir)
		ret = vfs_rmdir(mnt_idmap(parent.mnt), d_inode(parent.dentry),
				dentry);
	else
		ret = vfs_unlink(mnt_idmap(parent.mnt), d_inode(parent.dentry),
				 dentry, NULL);

out_dput:
	dput(dentry);
out_unlock:
	inode_unlock(d_inode(parent.dentry));
	mnt_drop_write(parent.mnt);
out_path:
	path_put(&parent);
out_name:
	putname(filename);
	if (retry_estale(ret, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return ret == -ENOENT ? 0 : ret;
}

static int named_swap_unlink_file(struct file *file)
{
	struct path *path = &file->f_path;
	struct dentry *dentry;
	struct dentry *parent;
	struct inode *dir;
	const struct cred *old;
	int ret;

	dentry = dget(path->dentry);
	if (IS_ROOT(dentry)) {
		ret = -EBUSY;
		goto out_dput;
	}

	old = override_creds(&init_cred);
	ret = mnt_want_write(path->mnt);
	if (ret)
		goto out_creds;

	parent = dget_parent(dentry);
	dir = d_inode(parent);
	inode_lock_nested(dir, I_MUTEX_PARENT);
	if (dentry->d_parent != parent || d_is_negative(dentry))
		ret = -ENOENT;
	else
		ret = vfs_unlink(mnt_idmap(path->mnt), dir, dentry, NULL);
	inode_unlock(dir);
	dput(parent);
	mnt_drop_write(path->mnt);
out_creds:
	revert_creds(old);
out_dput:
	dput(dentry);
	return ret == -ENOENT ? 0 : ret;
}

static loff_t named_swap_llseek(struct file *file, loff_t offset, int whence)
{
	return vfs_llseek(named_swap_lower(file), offset, whence);
}

static ssize_t named_swap_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct file *lower = named_swap_lower(file);
	const struct file_operations *fops = lower->f_op;
	ssize_t ret;

	if (!fops->read_iter)
		return -EINVAL;

	iocb->ki_filp = lower;
	ret = fops->read_iter(iocb, iter);
	iocb->ki_filp = file;
	return ret;
}

static ssize_t named_swap_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct file *lower = named_swap_lower(file);
	const struct file_operations *fops = lower->f_op;
	ssize_t ret;

	if (!fops->write_iter)
		return -EINVAL;

	iocb->ki_filp = lower;
	ret = fops->write_iter(iocb, iter);
	iocb->ki_filp = file;
	return ret;
}

static int named_swap_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *lower = named_swap_lower(file);

	if (!lower->f_op->mmap)
		return -ENODEV;

	return lower->f_op->mmap(lower, vma);
}

static int named_swap_fsync(struct file *file, loff_t start, loff_t end,
			    int datasync)
{
	struct file *lower = named_swap_lower(file);

	if (!lower->f_op->fsync)
		return -EINVAL;

	return lower->f_op->fsync(lower, start, end, datasync);
}

static int named_swap_release(struct inode *inode, struct file *file)
{
	struct named_swap_file *ns = file->private_data;
	struct file *lower;

	if (!ns)
		return 0;

	lower = ns->lower;
	if (lower) {
		trace_named_swap_file_release(file, ns->index, ns->nr_pages);
		/*
		 * ns->lower should be the sole reference (from filp_open).
		 * Extra refs mean a refcount bug in named_swap teardown.
		 */
		WARN_ON(file_count(lower) > 1);
		named_swap_storage_release(ns->nr_pages, ns->pool);
		/* xa_remove() fputs the wrapper; we are already in its release. */
		named_swap_xa_erase(ns->index);
		spin_lock(&ns->bind_lock);
		lower->f_mapping->anon_vma = NULL;
		spin_unlock(&ns->bind_lock);
		named_swap_unlink_file(lower);
		fput(lower);
		ns->lower = NULL;
	}

	kfree(ns);
	file->private_data = NULL;
	return 0;
}

static long named_swap_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct named_swap_file *ns = file->private_data;
	struct file *lower = named_swap_lower(file);
	loff_t old_size = named_swap_file_size(file);
	unsigned long pages = 0;
	long ret;

	if (old_size < 0)
		return old_size;
	if (!(mode & FALLOC_FL_PUNCH_HOLE)) {
		ret = named_swap_account_reserve_end(ns, offset + len, &pages);
		if (ret)
			return ret;
	}

	ret = named_swap_fallocate_lower(lower, mode, offset, len);
	if (ret) {
		if (pages)
			named_swap_storage_release(pages, ns->pool);
		pr_warn_ratelimited("named_swap_fallocate: vfs_fallocate failed: mode=%d offset=%llu len=%llu ret=%ld\n",
				    mode, (unsigned long long)offset,
				    (unsigned long long)len, ret);
		return ret;
	}
	if (ns && pages)
		ns->nr_pages += pages;
	file->f_inode->i_size = lower->f_inode->i_size;
	return ret;
}

static const struct file_operations named_swap_fops = {
	.llseek		= named_swap_llseek,
	.read_iter	= named_swap_read_iter,
	.write_iter	= named_swap_write_iter,
	.mmap		= named_swap_mmap,
	.fallocate	= named_swap_fallocate,
	.fsync		= named_swap_fsync,
	.release	= named_swap_release,
};


bool is_file_named_swap(struct file *file){
	
	if(!file)
		return false;

	return file->f_op == &named_swap_fops;
}

static int named_swap_wipe_dir(const char *path)
{
	struct named_swap_readdir iter;
	char child[PATH_MAX];
	struct path child_path;
	bool is_dir;
	int ret;

	while ((ret = named_swap_first_child(path, &iter)) > 0) {
		ret = scnprintf(child, sizeof(child), "%s/%s", path, iter.name);
		if (ret >= sizeof(child))
			return -ENAMETOOLONG;

		ret = kern_path(child, 0, &child_path);
		if (ret)
			return ret == -ENOENT ? 0 : ret;
		is_dir = d_is_dir(child_path.dentry);
		path_put(&child_path);

		if (is_dir) {
			ret = named_swap_wipe_dir(child);
			if (ret)
				return ret;
		}

		ret = named_swap_remove_path(child, is_dir);
		if (ret)
			return ret;
	}

	return ret;
}

static int named_swap_mkdir(const char *path, umode_t mode)
{
	struct dentry *dentry;
	struct path parent;
	int ret;

	ret = kern_path(path, LOOKUP_DIRECTORY, &parent);
	if (!ret) {
		path_put(&parent);
		return 0;
	}
	if (ret != -ENOENT)
		return ret;

	dentry = kern_path_create(AT_FDCWD, path, &parent, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	ret = vfs_mkdir(mnt_idmap(parent.mnt), d_inode(parent.dentry),
			dentry, mode);
	done_path_create(&parent, dentry);
	return ret == -EEXIST ? 0 : ret;
}

static int named_swap_cache_root_inode(void)
{
	struct path path;
	struct inode *inode;
	int ret;

	ret = kern_path(named_swap_storage_primary_dir(), LOOKUP_DIRECTORY,
			&path);
	if (ret)
		return ret;

	inode = d_inode(path.dentry);
	ihold(inode);
	path_put(&path);

	if (named_swap_root_inode)
		iput(named_swap_root_inode);
	named_swap_root_inode = inode;
	return 0;
}

static bool named_swap_sb_ephemeral(struct super_block *sb)
{
	if (!sb || !sb->s_type)
		return true;
	if (sb->s_magic == RAMFS_MAGIC || sb->s_magic == TMPFS_MAGIC)
		return true;
	return !strcmp(sb->s_type->name, "rootfs");
}

static bool named_swap_current_root_ephemeral(void)
{
	struct path root;
	bool ephemeral;

	if (!current->fs)
		return true;

	get_fs_root(current->fs, &root);
	ephemeral = named_swap_sb_ephemeral(root.dentry->d_sb);
	path_put(&root);
	return ephemeral;
}

static int named_swap_prepare_pool_dir(enum named_swap_storage_pool pool)
{
	const char *dir;
	int ret;

	if (!named_swap_storage_pool_used(pool))
		return 0;

	dir = named_swap_storage_pool_dir(pool);
	ret = named_swap_wipe_dir(dir);
	if (ret == -ENOENT)
		ret = 0;
	if (ret)
		return ret;
	return named_swap_mkdir(dir, 0700);
}

static int named_swap_enable_locked(void)
{
	const struct cred *old;
	int ret;

	if (READ_ONCE(named_swap_enabled))
		return 0;

	if (named_swap_current_root_ephemeral())
		return -EAGAIN;

	WRITE_ONCE(named_swap_enabled, false);
	if (named_swap_root_inode) {
		iput(named_swap_root_inode);
		named_swap_root_inode = NULL;
	}
	atomic64_set(&named_swap_index_counter, 0);
	named_swap_xa_destroy();

	old = override_creds(&init_cred);
	ret = named_swap_storage_setup();
	if (ret)
		goto out;

	ret = named_swap_prepare_pool_dir(NAMED_SWAP_POOL_SWAP);
	if (ret)
		goto out;
	ret = named_swap_prepare_pool_dir(NAMED_SWAP_POOL_FS);
	if (ret)
		goto out;

	ret = named_swap_cache_root_inode();
	if (ret)
		goto out;

	if (named_swap_sb_ephemeral(named_swap_root_inode->i_sb)) {
		ret = -EAGAIN;
		goto out_inode;
	}
	if (sb_rdonly(named_swap_root_inode->i_sb)) {
		ret = -EROFS;
		goto out_inode;
	}

	WRITE_ONCE(named_swap_enabled, true);
	pr_info("named_swap: enabled on %s (%s)\n",
		named_swap_storage_primary_dir(),
		named_swap_root_inode->i_sb->s_type->name);
out:
	revert_creds(old);
	return ret;

out_inode:
	iput(named_swap_root_inode);
	named_swap_root_inode = NULL;
	goto out;
}

static int named_swap_ensure_enabled(void)
{
	int ret;

	if (likely(READ_ONCE(named_swap_enabled)))
		return 0;

	/*
	 * Initramfs still has ramfs/tmpfs as '/'.  Skip the enable lock
	 * until switch_root; every anonymous mmap retries after that.
	 */
	if (named_swap_current_root_ephemeral())
		return -EAGAIN;

	mutex_lock(&named_swap_lock);
	ret = READ_ONCE(named_swap_enabled) ? 0 : named_swap_enable_locked();
	mutex_unlock(&named_swap_lock);
	return ret;
}

static struct file *named_swap_create_file(unsigned long len, bool allocate)
{
	char *path;
	struct file *lower;
	struct file *file;
	struct inode *inode;
	struct named_swap_file *ns;
	enum named_swap_storage_pool pool = NAMED_SWAP_POOL_FS;
	unsigned long pages = len >> PAGE_SHIFT;
	bool reserved = false;
	u64 index;
	int ret;
	const struct cred *old;

	ret = named_swap_ensure_enabled();
	if (ret) {
		/*
		 * Initramfs and a still-readonly backing dir are expected.
		 * Fall back to ordinary anonymous memory until named-swap
		 * can enable on the real root (and /nswap on the host).
		 */
		if (ret != -EROFS && ret != -EAGAIN)
			pr_warn_ratelimited("named_swap: enable failed (%d)\n",
					    ret);
		return ERR_PTR(ret);
	}

	ret = get_named_swap_file_index(&index);
	if (ret) {
		printk(KERN_ERR "named_swap_create_file: index overflow (> %lu)\n",
		       NAMED_SWAP_INDEX_MAX);
		return ERR_PTR(ret);
	}

	path = kmalloc(NAMED_SWAP_PATH_LEN, GFP_KERNEL);
	if (!path) {
		printk(KERN_ERR "named_swap_create_file: failed to allocate path");
		return ERR_PTR(-ENOMEM);
	}

	ret = named_swap_storage_reserve(pages, &pool);
	if (ret) {
		file = ERR_PTR(ret);
		goto out;
	}
	reserved = true;

	ret = named_swap_storage_build_path(index, pool, path,
					    NAMED_SWAP_PATH_LEN);
	if (ret) {
		printk(KERN_ERR "named_swap_create_file: failed to build path");
		file = ERR_PTR(ret);
		goto out;
	}

	old = override_creds(&init_cred);
	lower = filp_open(path, O_CREAT | O_EXCL | O_RDWR | O_LARGEFILE, 0600);
	revert_creds(old);
	if (IS_ERR(lower)) {
		long err = PTR_ERR(lower);

		if (err != -EACCES && err != -EROFS && err != -EEXIST)
			pr_warn_ratelimited(
				"named_swap_create_file: filp_open failed: path=%s err=%ld\n",
				path, err);
		file = lower;
		goto out;
	}

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns) {
		fput(lower);
		file = ERR_PTR(-ENOMEM);
		goto out;
	}
	spin_lock_init(&ns->bind_lock);
	mutex_init(&ns->size_lock);
	ns->lower = lower;
	ns->index = index;
	ns->pool = pool;
	ns->nr_pages = pages;

	file = anon_inode_create_getfile("[named_swap]", &named_swap_fops, ns,
				  O_RDWR | O_LARGEFILE, NULL);
	if (IS_ERR(file)) {
		fput(lower);
		kfree(ns);
		goto out;
	}
	/* named_swap_release() now owns the reservation. */
	reserved = false;
	inode = file_inode(file);
	inode->i_mode |= S_IFREG;


	/*
	 * Share the backing file's page cache; named_swap flags live on that
	 * mapping so vma_is_named_swap() and folio paths stay consistent.
	 */
	file->f_mapping = lower->f_mapping;
	file_ra_state_init(&file->f_ra, file->f_mapping);
	mapping_set_named_swap(lower->f_mapping);

	if (allocate) {
		/*
		 * Fallocate the lower file. vfs_fallocate() on the wrapper
		 * would named_swap_fallocate() and reserve the same pages
		 * again, filling the pool during boot/fork.
		 */
		ret = named_swap_fallocate_lower(lower, FALLOC_FL_ZERO_RANGE,
						 0, len);
		if (ret) {
			trace_named_swap_file_create(file, index, len, ret);
			if (ret != -EOPNOTSUPP && ret != -EROFS)
				pr_warn_ratelimited(
					"named_swap_create_file: vfs_fallocate failed: len=%lu ret=%d\n",
					len, ret);
			/*
			 * Keep the named-swap file. A full backing fs must not
			 * turn mmap/fork into -ENOMEM; sparse i_size still
			 * gives the VMA a stable identity, and later writes
			 * fallocate the range they touch.
			 */
			if (ret != -ENOSPC && ret != -EDQUOT) {
				fput(file);
				file = ERR_PTR(ret);
				goto out;
			}
			allocate = false;
		}
	}
	if (!allocate) {
		ret = named_swap_truncate_lower(lower, len);
		if (ret) {
			trace_named_swap_file_create(file, index, len, ret);
			pr_warn_ratelimited(
				"named_swap_create_file: truncate failed: len=%lu ret=%d\n",
				len, ret);
			fput(file);
			file = ERR_PTR(ret);
			goto out;
		}
	}
	i_size_write(inode, i_size_read(file_inode(lower)));
	trace_named_swap_file_create(file, index, len, 0);
	lower->f_mapping->anon_vma = NULL;

	ret = named_swap_xa_insert(file, index);
	if (ret) {
		printk(KERN_ERR "named_swap_create_file: xa_insert failed: index=%llu ret=%d\n",
		       index, ret);
		fput(file);
		file = ERR_PTR(ret);
		goto out;
	}
out:
	if (reserved)
		named_swap_storage_release(pages, pool);
	kfree(path);
	return file;
}

struct file *named_swap_prepare_mmap(unsigned long len, unsigned long *flag,
				     bool allocate)
{
	struct file *file = named_swap_create_file(len, allocate);
	
	if (IS_ERR(file))
		return NULL;
	if (flag)
		*flag |= MAP_NAMED_SWAP;
	return file;

}

/*
 * Link vma's anon_vma to its named swap backing file.  Per-vma lock lives
 * in named_swap_file (wrapper private_data).  Safe in page faults.
 * Call after anon_vma_prepare(); may take get_file() on first link.
 */

/*
 * One named-swap file mapping <-> one anon_vma. The first creator
 * publishes allocated; later faults adopt the winner.
 */
struct anon_vma *named_swap_claim_anon_vma(struct file *file,
					   struct anon_vma *allocated)
{
	struct named_swap_file *ns;
	struct address_space *mapping;
	struct anon_vma *existing;

	if (!file || !mapping_named_swap(file->f_mapping))
		return allocated;

	ns = file->private_data;
	mapping = file->f_mapping;
	if (!ns)
		return allocated;

	spin_lock(&ns->bind_lock);
	existing = mapping->anon_vma;
	if (!existing && allocated) {
		mapping->anon_vma = allocated;
		existing = allocated;
	}
	spin_unlock(&ns->bind_lock);
	return existing;
}

void named_swap_link(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	struct file *old_file = NULL;
	struct named_swap_file *ns = file->private_data;
	struct anon_vma *anon_vma = vma->anon_vma;
	struct address_space *mapping = file->f_mapping;
	u64 old_index = 0;
	bool drop_old_index = false;
	bool refresh_link;

	VM_BUG_ON_VMA(!anon_vma, vma);
	VM_BUG_ON(!ns || !mapping_named_swap(mapping));

	spin_lock(&ns->bind_lock);
	refresh_link = anon_vma->named_swap_file != file ||
		       mapping->anon_vma != anon_vma;
	if (mapping->anon_vma){
		if(mapping->anon_vma != anon_vma)
			VM_BUG_ON_VMA(mapping->anon_vma != anon_vma, vma);}
	else
		mapping->anon_vma = anon_vma;
	if (refresh_link) {
		old_file = anon_vma->named_swap_file;
		if (old_file && old_file != file) {
			struct named_swap_file *old_ns = old_file->private_data;
			struct address_space *old_mapping = old_file->f_mapping;

			if (old_mapping->anon_vma == anon_vma)
				old_mapping->anon_vma = NULL;
			if (old_ns) {
				old_index = old_ns->index;
				drop_old_index = true;
			}
		}
		get_file(file);
		anon_vma->named_swap_file = file;
	}
	spin_unlock(&ns->bind_lock);
	if (old_file) {
		fput(old_file);
		if (drop_old_index)
			named_swap_xa_remove(old_index);
	}
	trace_named_swap_link(file, vma, anon_vma, ns->index, refresh_link);
}

/*
 * Break the link when anon_vma is torn down.  fput() is outside the lock.
 */
void named_swap_unlink(struct anon_vma *anon_vma)
{
	struct file *file;
	struct named_swap_file *ns;
	struct address_space *mapping;

	file = anon_vma->named_swap_file;
	if (!file)
		return;

	ns = file->private_data;
	if (WARN_ON(!ns))
		return;

	spin_lock(&ns->bind_lock);
	file = anon_vma->named_swap_file;
	if (file) {
		u64 index = ns->index;

		mapping = file->f_mapping;
		if (mapping->anon_vma == anon_vma)
			mapping->anon_vma = NULL;
		anon_vma->named_swap_file = NULL;
		spin_unlock(&ns->bind_lock);
		fput(file);
		/*
		 * xa_insert() stored an extra get_file() ref.  Drop it so
		 * named_swap_release() runs and unlinks the backing file.
		 */
		named_swap_xa_remove(index);
	} else {
		spin_unlock(&ns->bind_lock);
	}
}

/*
 * Clean sequential reclaim helpers (see named_swap_seq_* traces).
 * Walk mapping->i_pages contiguously; only clean isolatable folios.
 */
struct named_swap_reclaim_stream {
	struct address_space *mapping;
	spinlock_t lock;
	pgoff_t window_start;
	pgoff_t window_end;
	unsigned int ahead_size;
	unsigned int isolated;
	void *stream_cookie;
	unsigned int seq_hits;
};

enum named_swap_seq_stop
named_swap_seq_classify(struct folio *folio, struct lruvec *lruvec,
			int type, int zone, int gen)
{
	if (!folio)
		return NAMED_SWAP_SEQ_NO_ENTRY;
	if (!folio_ref_count(folio))
		return NAMED_SWAP_SEQ_MAPPING_GONE;
	if (!folio_test_named_swap(folio))
		return NAMED_SWAP_SEQ_MAPPING_GONE;
	if (folio_test_dirty(folio))
		return NAMED_SWAP_SEQ_DIRTY;
	if (folio_test_writeback(folio))
		return NAMED_SWAP_SEQ_WRITEBACK;
	if (folio_test_unevictable(folio))
		return NAMED_SWAP_SEQ_UNEVICTABLE;
	if (folio_test_active(folio))
		return NAMED_SWAP_SEQ_ACTIVE;
	if (!folio_test_lru(folio))
		return NAMED_SWAP_SEQ_NOT_LRU;
	if (folio_memcg(folio) != lruvec_memcg(lruvec) ||
	    folio_lruvec(folio) != lruvec)
		return NAMED_SWAP_SEQ_WRONG_LRUVEC;
	if (folio_zonenum(folio) != zone)
		return NAMED_SWAP_SEQ_WRONG_ZONE;
	if (folio_lru_gen_type(folio) != type)
		return NAMED_SWAP_SEQ_WRONG_TYPE;
	if (folio_lru_gen(folio) != gen)
		return NAMED_SWAP_SEQ_WRONG_GEN;
	return NAMED_SWAP_SEQ_OK;
}

struct folio *named_swap_seq_go_back(struct address_space *mapping,
				    struct folio *folio, struct lruvec *lruvec,
				    int type, int zone, int gen,
				    pgoff_t *go_back_to)
{
	struct folio *first = folio;
	pgoff_t index = folio->index;
	unsigned long steps = 0;

	*go_back_to = index;
	if (!mapping)
		return first;

	while (steps < NAMED_SWAP_RECLAIM_GO_BACK) {
		XA_STATE(xas, &mapping->i_pages, index);
		void *entry;
		struct folio *prev;
		pgoff_t prev_index;
		enum named_swap_seq_stop cls;

		if (!index)
			break;

		rcu_read_lock();
		entry = xas_prev(&xas);
		if (!entry || xa_is_value(entry)) {
			rcu_read_unlock();
			break;
		}
		prev = entry;
		prev_index = prev->index;
		if (prev_index + folio_nr_pages(prev) != index) {
			rcu_read_unlock();
			break;
		}
		/* Classify under RCU so the folio cannot free mid-check. */
		cls = named_swap_seq_classify(prev, lruvec, type, zone, gen);
		rcu_read_unlock();
		if (cls != NAMED_SWAP_SEQ_OK)
			break;
		first = prev;
		index = prev_index;
		*go_back_to = index;
		steps++;
	}
	return first;
}

struct folio *named_swap_seq_next(struct address_space *mapping,
				 struct folio *folio, struct lruvec *lruvec,
				 int type, int zone, int gen,
				 enum named_swap_seq_stop *reason)
{
	XA_STATE(xas, &mapping->i_pages,
		 folio->index + folio_nr_pages(folio));
	void *entry;
	struct folio *next;
	pgoff_t expect = folio->index + folio_nr_pages(folio);
	pgoff_t next_index;

	if (!mapping) {
		*reason = NAMED_SWAP_SEQ_MAPPING_GONE;
		return NULL;
	}

	rcu_read_lock();
	entry = xas_next_entry(&xas, ULONG_MAX);
	if (!entry) {
		rcu_read_unlock();
		*reason = NAMED_SWAP_SEQ_NO_ENTRY;
		return NULL;
	}
	if (xa_is_value(entry)) {
		rcu_read_unlock();
		*reason = NAMED_SWAP_SEQ_VALUE_ENTRY;
		return NULL;
	}
	next = entry;
	next_index = next->index;
	if (next_index != expect) {
		rcu_read_unlock();
		*reason = NAMED_SWAP_SEQ_INDEX_GAP;
		return NULL;
	}
	*reason = named_swap_seq_classify(next, lruvec, type, zone, gen);
	rcu_read_unlock();
	if (*reason != NAMED_SWAP_SEQ_OK)
		return NULL;
	return next;
}

static int named_swap_convert_split_pmd(pmd_t *pmd, unsigned long addr,
					unsigned long next,
					struct mm_walk *walk)
{
	if (is_swap_pmd(*pmd) || pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
		split_huge_pmd(walk->vma, pmd, addr);
	return 0;
}

static const struct mm_walk_ops named_swap_convert_split_ops = {
	.pmd_entry	= named_swap_convert_split_pmd,
	.walk_lock	= PGWALK_WRLOCK_VERIFY,
};

static pmd_t *named_swap_convert_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return NULL;
	return pmd_offset(pud, addr);
}

static int named_swap_convert_populate(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;
	unsigned int flags = 0;

	if (!(vma->vm_flags & VM_ACCESS_FLAGS))
		return 0;

	if (vma->vm_flags & VM_WRITE)
		flags |= FAULT_FLAG_WRITE;

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		pmd_t *pmd = named_swap_convert_pmd(mm, addr);
		bool need_fault = true;
		vm_fault_t fault;

		if (pmd && !pmd_none(*pmd) && !pmd_trans_huge(*pmd) &&
		    !is_swap_pmd(*pmd) && !pmd_devmap(*pmd)) {
			pte_t *ptep = pte_offset_map(pmd, addr);

			if (ptep) {
				if (pte_present(ptep_get(ptep)))
					need_fault = false;
				pte_unmap(ptep);
			}
		}
		if (!need_fault)
			continue;

		fault = handle_mm_fault(vma, addr, flags, NULL);
		if (fault & VM_FAULT_ERROR)
			return vm_fault_to_errno(fault, 0);
	}
	return 0;
}

static bool named_swap_anon_vma_has_other(struct vm_area_struct *skip)
{
	struct mm_struct *mm = skip->vm_mm;
	struct vm_area_struct *tmp;
	struct anon_vma *anon_vma = skip->anon_vma;
	VMA_ITERATOR(vmi, mm, 0);

	if (!anon_vma)
		return false;

	for_each_vma(vmi, tmp) {
		if (tmp == skip)
			continue;
		if (vma_is_named_swap(tmp) && tmp->anon_vma == anon_vma)
			return true;
	}
	return false;
}

static void named_swap_convert_unbind_file(struct vm_area_struct *vma,
					   struct file *file)
{
	vma->vm_file = NULL;
	vma_set_anonymous(vma);
	if (vma->anon_vma && vma->anon_vma->named_swap_file == file &&
	    !named_swap_anon_vma_has_other(vma))
		named_swap_unlink(vma->anon_vma);
	if (file)
		fput(file);
}

static bool named_swap_pte_still_maps(pte_t pte, struct page *page)
{
	return pte_present(pte) && !is_zero_pfn(pte_pfn(pte)) &&
	       pfn_valid(pte_pfn(pte)) && pte_page(pte) == page;
}

static void named_swap_convert_rss(struct mm_struct *mm, int old, int new)
{
	if (old == new)
		return;
	dec_mm_counter(mm, old);
	inc_mm_counter(mm, new);
}

static bool named_swap_folio_off_lru(struct folio *folio)
{
	/*
	 * folio_add_lru() only queues on this CPU. A local drain leaves
	 * the folio in another CPU's batch with PG_lru clear, so isolate
	 * fails and a second folio_add_lru() double-queues it. The later
	 * drain hits VM_BUG_ON_FOLIO(folio_test_lru) in lru_add().
	 */
	lru_add_drain_all();
	return folio_isolate_lru(folio);
}

static void named_swap_folio_on_lru(struct folio *folio, bool isolated)
{
	if (isolated)
		folio_putback_lru(folio);
	/*
	 * Do not folio_add_lru() when isolate failed. The folio is still
	 * in a remote add batch, already isolated by reclaim, or was
	 * never on the LRU. A second queue is what trips lru_add().
	 */
}

static int named_swap_folio_lock_split(struct folio **foliop, struct page *page)
{
	struct folio *folio = *foliop;
	int err;

	folio_lock(folio);
	if (folio_test_writeback(folio)) {
		folio_unlock(folio);
		folio_wait_writeback(folio);
		folio_lock(folio);
	}

	if (!folio_test_large(folio))
		return 0;

	/*
	 * split_huge_page accounts only the folio lock as an extra pin.
	 * Drop the caller's reference for the split, then restore it on
	 * the resulting order-0 folio.
	 */
	folio_put(folio);
	err = split_folio(folio);
	if (err) {
		folio_unlock(folio);
		folio_get(page_folio(page));
		return err;
	}
	folio = page_folio(page);
	folio_get(folio);
	*foliop = folio;
	return 0;
}

static void named_swap_folio_detach_file(struct folio *folio)
{
	if (folio_needs_release(folio))
		folio_invalidate(folio, 0, folio_size(folio));
	folio_cancel_dirty(folio);
	if (folio_has_private(folio))
		filemap_release_folio(folio, GFP_KERNEL);
}

static void named_swap_folio_prepare_writeback(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;
	unsigned int blocksize;

	if (!folio_test_uptodate(folio))
		folio_mark_uptodate(folio);
	if (!mapping || !mapping->host || !mapping_can_writeback(mapping))
		return;
	if (folio_buffers(folio))
		return;
	blocksize = 1U << mapping->host->i_blkbits;
	if (!blocksize)
		return;
	create_empty_buffers(folio, blocksize, (1 << BH_Uptodate));
}

static void named_swap_restore_anon_rmap(struct folio *folio, struct page *page,
					 struct vm_area_struct *vma,
					 unsigned long addr)
{
	__folio_set_swapbacked(folio);
	folio->index = linear_page_index(vma, addr);
	folio_move_anon_rmap(folio, vma);
	folio_add_anon_rmap_pte(folio, page, vma, addr, RMAP_EXCLUSIVE);
}

static int named_swap_folio_to_anon(struct vm_area_struct *vma,
				    unsigned long addr, struct folio *folio,
				    struct page *page)
{
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;
	int old_rss;
	bool isolated;
	int err;

	err = named_swap_folio_lock_split(&folio, page);
	if (err)
		return err;

	old_rss = mm_counter(folio);
	isolated = named_swap_folio_off_lru(folio);

	pmd = named_swap_convert_pmd(mm, addr);
	if (!pmd) {
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep || !named_swap_pte_still_maps(ptep_get(ptep), page)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	folio_remove_rmap_pte(folio, page, vma);
	pte_unmap_unlock(ptep, ptl);

	if (folio_mapped(folio) || folio_test_anon(folio) || !folio->mapping) {
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (ptep) {
			folio_add_file_rmap_pte(folio, page, vma);
			pte_unmap_unlock(ptep, ptl);
		}
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	named_swap_folio_detach_file(folio);
	filemap_remove_folio(folio);

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep || !named_swap_pte_still_maps(ptep_get(ptep), page)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	named_swap_convert_rss(mm, old_rss, mm_counter(folio));
	pte_unmap_unlock(ptep, ptl);
	named_swap_folio_on_lru(folio, isolated);
	folio_unlock(folio);
	return 0;
}

static int named_swap_folio_to_file(struct vm_area_struct *vma,
				    unsigned long addr, struct folio *folio,
				    struct page *page)
{
	struct mm_struct *mm = vma->vm_mm;
	struct address_space *mapping = vma->vm_file->f_mapping;
	pgoff_t index = linear_page_index(vma, addr);
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;
	int old_rss;
	bool isolated;
	int err;

	err = named_swap_folio_lock_split(&folio, page);
	if (err)
		return err;

	old_rss = mm_counter(folio);
	isolated = named_swap_folio_off_lru(folio);

	pmd = named_swap_convert_pmd(mm, addr);
	if (!pmd) {
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep || !named_swap_pte_still_maps(ptep_get(ptep), page)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	folio_remove_rmap_pte(folio, page, vma);
	pte_unmap_unlock(ptep, ptl);

	ClearPageAnonExclusive(page);
	folio_clear_swapbacked(folio);
	folio->mapping = NULL;
	err = __filemap_add_folio(mapping, folio, index, GFP_KERNEL, NULL);
	if (err) {
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (ptep) {
			named_swap_restore_anon_rmap(folio, page, vma, addr);
			pte_unmap_unlock(ptep, ptl);
		}
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return err;
	}

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep || !named_swap_pte_still_maps(ptep_get(ptep), page)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		filemap_remove_folio(folio);
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (ptep) {
			if (named_swap_pte_still_maps(ptep_get(ptep), page))
				named_swap_restore_anon_rmap(folio, page, vma,
							     addr);
			pte_unmap_unlock(ptep, ptl);
		}
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	pte_unmap_unlock(ptep, ptl);

	/*
	 * create_empty_buffers() may sleep. Do it after dropping the
	 * PTE lock; re-check the PTE before installing file rmap.
	 */
	named_swap_folio_prepare_writeback(folio);

	ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!ptep || !named_swap_pte_still_maps(ptep_get(ptep), page)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		filemap_remove_folio(folio);
		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (ptep) {
			if (named_swap_pte_still_maps(ptep_get(ptep), page))
				named_swap_restore_anon_rmap(folio, page, vma,
							     addr);
			pte_unmap_unlock(ptep, ptl);
		}
		named_swap_folio_on_lru(folio, isolated);
		folio_unlock(folio);
		return -EBUSY;
	}
	folio_add_file_rmap_pte(folio, page, vma);
	folio_mark_dirty(folio);
	{
		pte_t pte = ptep_get(ptep);

		pte = pte_mkdirty(pte);
		pte = maybe_mkwrite(pte, vma);
		set_pte_at(mm, addr, ptep, pte);
		update_mmu_cache(vma, addr, ptep);
	}
	named_swap_convert_rss(mm, old_rss, mm_counter(folio));
	pte_unmap_unlock(ptep, ptl);
	named_swap_folio_on_lru(folio, isolated);
	folio_unlock(folio);
	return 0;
}

static int named_swap_convert_ptes(struct vm_area_struct *vma, bool to_named_swap)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr;
	int err = 0;

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		spinlock_t *ptl;
		pte_t *ptep;
		pte_t pte;
		struct folio *folio;
		struct page *page;
		pmd_t *pmd;

		pmd = named_swap_convert_pmd(mm, addr);
		if (!pmd || pmd_none(*pmd))
			continue;

		ptep = pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!ptep)
			continue;
		pte = ptep_get(ptep);
		if (!pte_present(pte) || is_zero_pfn(pte_pfn(pte))) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}
		page = vm_normal_page(vma, addr, pte);
		if (!page) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}
		folio = page_folio(page);
		folio_get(folio);
		pte_unmap_unlock(ptep, ptl);

		if (to_named_swap) {
			if (!folio_test_anon(folio)) {
				folio_put(folio);
				continue;
			}
			if (folio_mapcount(folio) != 1) {
				folio_put(folio);
				err = -EBUSY;
				break;
			}
			err = named_swap_folio_to_file(vma, addr, folio, page);
		} else {
			if (!folio_test_named_swap(folio)) {
				folio_put(folio);
				continue;
			}
			if (folio_mapcount(folio) != 1) {
				folio_put(folio);
				err = -EBUSY;
				break;
			}
			err = named_swap_folio_to_anon(vma, addr, folio, page);
		}
		folio_put(folio);
		if (err)
			break;
	}
	return err;
}

static int named_swap_convert_to_anon(struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	int err;

	if (!vma_is_named_swap(vma))
		return 0;

	err = named_swap_convert_populate(vma);
	if (err)
		return err;

	if (anon_vma_prepare(vma))
		return -ENOMEM;

	vma_start_write(vma);
	named_swap_convert_unbind_file(vma, file);
	return named_swap_convert_ptes(vma, false);
}

static int named_swap_convert_to_named(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct file *file;
	unsigned long start = vma->vm_start;
	unsigned long end = vma->vm_end;
	unsigned long flags = vma->vm_flags;
	pgoff_t pgoff = vma->vm_pgoff;
	unsigned long vma_len = end - start;
	loff_t need;
	unsigned long len;
	int err;

	if (vma_is_named_swap(vma))
		return 0;
	if (vma->vm_file)
		return -EINVAL;

	err = named_swap_convert_populate(vma);
	if (err)
		return err;

	if (anon_vma_prepare(vma))
		return -ENOMEM;

	need = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + vma_len;
	if (need < 0 || (loff_t)(unsigned long)need != need)
		return -EFBIG;
	len = (unsigned long)need;

	/*
	 * filp_open/fallocate take sb_writers. Drop mmap so we do not
	 * invert inode → mmap (GUP takes mmap under the inode lock).
	 */
	mmap_write_unlock(mm);
	file = named_swap_prepare_mmap(len, NULL,
				       !!(flags & VM_ACCESS_FLAGS));
	mmap_write_lock(mm);
	vma = find_vma(mm, start);
	if (!vma || vma->vm_start != start || vma->vm_end != end ||
	    vma->vm_pgoff != pgoff || vma->vm_file) {
		if (!IS_ERR_OR_NULL(file))
			fput(file);
		return -EAGAIN;
	}
	if (IS_ERR_OR_NULL(file))
		return file ? PTR_ERR(file) : -ENOMEM;

	vma_start_write(vma);
	vma->vm_file = get_file(file);
	err = mmap_file(file, vma);
	if (err) {
		fput(vma->vm_file);
		vma->vm_file = NULL;
		vma_set_anonymous(vma);
		fput(file);
		return err;
	}
	named_swap_link(vma);
	err = named_swap_convert_ptes(vma, true);
	fput(file);
	if (err) {
		struct file *drop = vma->vm_file;

		named_swap_convert_unbind_file(vma, drop);
	}
	return err;
}

int named_swap_convert_vma(struct vm_area_struct *vma, bool to_named_swap)
{
	struct mm_struct *mm = vma->vm_mm;
	int err;

	mmap_assert_write_locked(mm);

	if (!can_modify_vma(vma))
		return -EINVAL;
	if (is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_file && !vma_is_named_swap(vma))
		return -EINVAL;

	if (to_named_swap == vma_is_named_swap(vma))
		return 0;

	vma_start_write(vma);
	err = walk_page_vma(vma, &named_swap_convert_split_ops, NULL);
	if (err)
		return err;

	if (to_named_swap)
		return named_swap_convert_to_named(vma);
	return named_swap_convert_to_anon(vma);
}
EXPORT_SYMBOL_GPL(named_swap_convert_vma);

