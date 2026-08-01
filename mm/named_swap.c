// SPDX-License-Identifier: GPL-2.0
/*
 * Named swap backing files.
 *
 * Creates a root directory and one backing file per anon_vma under
 * <named_swap_root>/<index>.  The root defaults to /.named_swap and can be
 * overridden before first use via named_swap.root= on the kernel cmdline or
 * /proc/sys/vm/named_swap_root.
 */
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/dcache.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/hash.h>
#include <linux/ktime.h>
#include <linux/ptrace.h>
#include <linux/sched/debug.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
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

static atomic_long_t named_swap_pages = ATOMIC_LONG_INIT(0);

static struct file *named_swap_file_peek(u64 index);

unsigned long named_swap_total_pages(void)
{
	return atomic_long_read(&named_swap_pages);
}

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

/*
 * Room for root + '/' + decimal u64 index + NUL.  Keep the root short enough
 * that named_swap_build_path() cannot overflow NAMED_SWAP_PATH_LEN.
 */
#define NAMED_SWAP_INDEX_DIGITS	20
#define NAMED_SWAP_ROOT_MAX	(NAMED_SWAP_PATH_LEN - 1 - NAMED_SWAP_INDEX_DIGITS)
/* Indices are stored in the swap PTE offset field (see swp_offset()). */
#define NAMED_SWAP_INDEX_MAX	SWP_OFFSET_MASK

char named_swap_root[NAMED_SWAP_PATH_LEN] = "/.named_swap";
EXPORT_SYMBOL_GPL(named_swap_root);

static bool named_swap_enabled;
static DEFINE_MUTEX(named_swap_lock);
static struct inode *named_swap_root_inode;
static atomic64_t named_swap_index_counter = ATOMIC64_INIT(0);

int named_swap_min_vma_size = 256; /* 256 pages = 1MB */
EXPORT_SYMBOL_GPL(named_swap_min_vma_size);

static int named_swap_cache_root_inode(void);

static int named_swap_validate_root(const char *path)
{
	size_t len;

	if (!path || path[0] != '/')
		return -EINVAL;

	len = strlen(path);
	if (!len || len > NAMED_SWAP_ROOT_MAX)
		return -EINVAL;

	/* Reject trailing slash except for the filesystem root itself. */
	if (len > 1 && path[len - 1] == '/')
		return -EINVAL;

	if (strchr(path, '\n'))
		return -EINVAL;

	return 0;
}

static int named_swap_set_root(const char *path)
{
	int ret;

	ret = named_swap_validate_root(path);
	if (ret)
		return ret;

	strscpy(named_swap_root, path, sizeof(named_swap_root));
	return 0;
}

static int __init named_swap_root_setup(char *str)
{
	int ret;

	if (!str || !*str) {
		pr_warn("named_swap.root: empty path ignored\n");
		return 0;
	}

	ret = named_swap_set_root(str);
	if (ret)
		pr_warn("named_swap.root: invalid path '%s' (%d), keeping '%s'\n",
			str, ret, named_swap_root);
	else
		pr_info("named_swap.root: using '%s'\n", named_swap_root);
	return 1;
}
__setup("named_swap.root=", named_swap_root_setup);

int proc_named_swap_root(const struct ctl_table *table, int write,
			 void *buffer, size_t *lenp, loff_t *ppos)
{
	char tmp[NAMED_SWAP_PATH_LEN];
	struct ctl_table fake;
	int ret;

	if (!write)
		return proc_dostring(table, write, buffer, lenp, ppos);

	if (READ_ONCE(named_swap_enabled))
		return -EBUSY;

	fake = *table;
	fake.data = tmp;
	fake.maxlen = sizeof(tmp);
	memset(tmp, 0, sizeof(tmp));
	ret = proc_dostring(&fake, 1, buffer, lenp, ppos);
	if (ret)
		return ret;

	/* proc_dostring may leave a trailing newline. */
	if (tmp[0] && tmp[strlen(tmp) - 1] == '\n')
		tmp[strlen(tmp) - 1] = '\0';

	mutex_lock(&named_swap_lock);
	if (READ_ONCE(named_swap_enabled)) {
		mutex_unlock(&named_swap_lock);
		return -EBUSY;
	}
	ret = named_swap_set_root(tmp);
	mutex_unlock(&named_swap_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proc_named_swap_root);

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
	int ret;

	dentry = dget(path->dentry);
	if (IS_ROOT(dentry)) {
		ret = -EBUSY;
		goto out_dput;
	}

	ret = mnt_want_write(path->mnt);
	if (ret)
		goto out_dput;

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
		atomic_long_sub(ns->nr_pages, &named_swap_pages);
		named_swap_xa_remove(ns->index);
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
	long ret = vfs_fallocate(named_swap_lower(file), mode, offset, len);
	if (ret) {
		printk(KERN_ERR "named_swap_fallocate: vfs_fallocate failed: file=%px mode=%d offset=%llu len=%llu ret=%ld\n", file, mode, offset, len, ret);
	}
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

	ret = kern_path(named_swap_root, LOOKUP_DIRECTORY, &path);
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

static int named_swap_enable_locked(void)
{
	int ret;

	WRITE_ONCE(named_swap_enabled, false);
	if (named_swap_root_inode) {
		iput(named_swap_root_inode);
		named_swap_root_inode = NULL;
	}
	atomic64_set(&named_swap_index_counter, 0);
	named_swap_xa_destroy();

	ret = named_swap_wipe_dir(named_swap_root);
	if (ret == -ENOENT)
		ret = 0;
	if (ret)
		return ret;

	ret = named_swap_mkdir(named_swap_root, 0700);
	if (ret)
		return ret;

	ret = named_swap_cache_root_inode();
	if (ret)
		return ret;

	WRITE_ONCE(named_swap_enabled, true);
	return 0;
}

static int named_swap_ensure_enabled(void)
{
	int ret;

	if (likely(READ_ONCE(named_swap_enabled)))
		return 0;

	mutex_lock(&named_swap_lock);
	ret = READ_ONCE(named_swap_enabled) ? 0 : named_swap_enable_locked();
	mutex_unlock(&named_swap_lock);
	return ret;
}

static int named_swap_build_path(u64 index, char *path, size_t len)
{
	int ret;

	ret = scnprintf(path, len, "%s/%llu", named_swap_root, index);
	if (ret >= len)
		return -ENAMETOOLONG;

	return 0;
}

static struct file *named_swap_create_file(unsigned long len)
{
	char *path;
	struct file *lower;
	struct file *file;
	struct inode *inode;
	struct named_swap_file *ns;
	u64 index;
	int ret;

	ret = named_swap_ensure_enabled();
	if (ret) {
		printk(KERN_ERR "named_swap_create_file: failed to ensure enabled");
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

	ret = named_swap_build_path(index, path, NAMED_SWAP_PATH_LEN);
	if (ret) {
		printk(KERN_ERR "named_swap_create_file: failed to build path");
		file = ERR_PTR(ret);
		goto out;
	}

	lower = filp_open(path, O_CREAT | O_EXCL | O_RDWR | O_LARGEFILE, 0600);
	if (IS_ERR(lower)) {
		long err = PTR_ERR(lower);

		printk(KERN_ERR
		       "named_swap_create_file: filp_open failed: path=%s err=%ld flags=0%o pid=%d comm=%s len=%lu file_index=%llu\n",
		       path, err, O_CREAT | O_EXCL | O_RDWR | O_LARGEFILE,
		       current->pid, current->comm,
		       len, index);
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
	ns->lower = lower;
	ns->index = index;

	file = anon_inode_create_getfile("[named_swap]", &named_swap_fops, ns,
				  O_RDWR | O_LARGEFILE, NULL);
	if (IS_ERR(file)) {
		fput(lower);
		kfree(ns);
		goto out;
	}
	inode = file_inode(file);
	inode->i_mode |= S_IFREG;


	/*
	 * Share the backing file's page cache; named_swap flags live on that
	 * mapping so vma_is_named_swap() and folio paths stay consistent.
	 */
	file->f_mapping = lower->f_mapping;
	file_ra_state_init(&file->f_ra, file->f_mapping);
	mapping_set_named_swap(lower->f_mapping);

	ret = vfs_fallocate(file, FALLOC_FL_ZERO_RANGE, 0, len);
	if (ret) {
		trace_named_swap_file_create(file, index, len, ret);
		printk(KERN_ERR "named_swap_create_file: vfs_fallocate failed: file=%px len=%lu ret=%d\n", file, len, ret);
		fput(file);
		file = ERR_PTR(ret);
		goto out;
	}
	i_size_write(inode, len);
	ns->nr_pages = len >> PAGE_SHIFT;
	atomic_long_add(ns->nr_pages, &named_swap_pages);
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
	kfree(path);
	return file;
}

struct file *named_swap_prepare_mmap(unsigned long len, unsigned long *flag) {

	struct file *file = named_swap_create_file(len);
	
	if (IS_ERR(file)){
		printk(KERN_ERR "named_swap_prepare_mmap: failed to create file (falling back to anon memory): file=%px len=%lu\n", file, len);
		return NULL;
	}
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