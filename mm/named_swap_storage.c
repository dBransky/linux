// SPDX-License-Identifier: GPL-2.0
/*
 * Named-swap storage pools: where new backing files may be created and
 * how much disk they may consume.  Create/link/fault/rmap stay in
 * named_swap.c; this file owns mode, device, directories, and the
 * reserve/release gate.
 */
#include <linux/atomic.h>
#include <linux/blkdev.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/overflow.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/sysctl.h>

#define NAMED_SWAP_INDEX_DIGITS	20
#define NAMED_SWAP_ROOT_MAX	(NAMED_SWAP_PATH_LEN - 1 - NAMED_SWAP_INDEX_DIGITS)

char named_swap_root[NAMED_SWAP_PATH_LEN] = "/nswap";
EXPORT_SYMBOL_GPL(named_swap_root);

char named_swap_fs_root[NAMED_SWAP_PATH_LEN] = "/.named_swap";
EXPORT_SYMBOL_GPL(named_swap_fs_root);

char named_swap_device[NAMED_SWAP_PATH_LEN];
EXPORT_SYMBOL_GPL(named_swap_device);

int named_swap_fs_free = 10;
EXPORT_SYMBOL_GPL(named_swap_fs_free);

int named_swap_freerun;
EXPORT_SYMBOL_GPL(named_swap_freerun);

static char named_swap_mode_name[16];
static bool named_swap_mode_explicit;
static enum named_swap_storage_mode named_swap_mode_parsed;
static enum named_swap_storage_mode named_swap_mode_resolved;
static bool storage_ready;

struct named_swap_pool {
	struct path path;
	struct super_block *sb;
	atomic_long_t usage;
	u64 total_pages;
	u64 free_pages;
	u64 hard_pages;
	bool bound;
};

static struct named_swap_pool pools[2];
static DEFINE_MUTEX(named_swap_storage_lock);

static const char *pool_dir(enum named_swap_storage_pool pool)
{
	if (pool == NAMED_SWAP_POOL_SWAP)
		return named_swap_root;
	return named_swap_fs_root;
}

static bool pool_used_by_mode(enum named_swap_storage_mode mode,
			      enum named_swap_storage_pool pool)
{
	switch (mode) {
	case NAMED_SWAP_STORAGE_SWAP:
		return pool == NAMED_SWAP_POOL_SWAP;
	case NAMED_SWAP_STORAGE_FS:
		return pool == NAMED_SWAP_POOL_FS;
	case NAMED_SWAP_STORAGE_HYBRID:
		return true;
	}
	return false;
}

static const char *mode_to_name(enum named_swap_storage_mode mode)
{
	switch (mode) {
	case NAMED_SWAP_STORAGE_SWAP:
		return "swap";
	case NAMED_SWAP_STORAGE_HYBRID:
		return "hybrid";
	case NAMED_SWAP_STORAGE_FS:
	default:
		return "fs";
	}
}

static int parse_mode_name(const char *str, enum named_swap_storage_mode *mode)
{
	if (!strcmp(str, "fs"))
		*mode = NAMED_SWAP_STORAGE_FS;
	else if (!strcmp(str, "swap"))
		*mode = NAMED_SWAP_STORAGE_SWAP;
	else if (!strcmp(str, "hybrid"))
		*mode = NAMED_SWAP_STORAGE_HYBRID;
	else
		return -EINVAL;
	return 0;
}

static int resolve_mode(enum named_swap_storage_mode *mode)
{
	if (named_swap_mode_explicit) {
		*mode = named_swap_mode_parsed;
		if ((*mode == NAMED_SWAP_STORAGE_SWAP ||
		     *mode == NAMED_SWAP_STORAGE_HYBRID) &&
		    !named_swap_device[0])
			return -EINVAL;
		return 0;
	}

	*mode = named_swap_device[0] ? NAMED_SWAP_STORAGE_SWAP :
				       NAMED_SWAP_STORAGE_FS;
	return 0;
}

static int named_swap_validate_root(const char *path)
{
	size_t len;

	if (!path || path[0] != '/')
		return -EINVAL;

	len = strlen(path);
	if (!len || len > NAMED_SWAP_ROOT_MAX)
		return -EINVAL;

	if (len > 1 && path[len - 1] == '/')
		return -EINVAL;

	if (strchr(path, '\n'))
		return -EINVAL;

	return 0;
}

static int named_swap_set_path(char *dst, size_t dst_len, const char *path)
{
	int ret;

	ret = named_swap_validate_root(path);
	if (ret)
		return ret;

	strscpy(dst, path, dst_len);
	return 0;
}

static int named_swap_set_device(const char *path)
{
	size_t len;

	if (!path || !path[0]) {
		named_swap_device[0] = '\0';
		return 0;
	}
	if (path[0] != '/')
		return -EINVAL;
	len = strlen(path);
	if (!len || len >= sizeof(named_swap_device))
		return -EINVAL;
	if (path[len - 1] == '/' || strchr(path, '\n'))
		return -EINVAL;
	strscpy(named_swap_device, path, sizeof(named_swap_device));
	return 0;
}

static int named_swap_set_mode_name(const char *str)
{
	enum named_swap_storage_mode mode;
	int ret;

	if (!str || !str[0]) {
		named_swap_mode_name[0] = '\0';
		named_swap_mode_explicit = false;
		return 0;
	}

	ret = parse_mode_name(str, &mode);
	if (ret)
		return ret;

	strscpy(named_swap_mode_name, str, sizeof(named_swap_mode_name));
	named_swap_mode_parsed = mode;
	named_swap_mode_explicit = true;
	return 0;
}

static int __init named_swap_root_setup(char *str)
{
	int ret;

	if (!str || !*str) {
		pr_warn("named_swap.root: empty path ignored\n");
		return 0;
	}

	ret = named_swap_set_path(named_swap_root, sizeof(named_swap_root), str);
	if (ret)
		pr_warn("named_swap.root: invalid path '%s' (%d), keeping '%s'\n",
			str, ret, named_swap_root);
	else
		pr_info("named_swap.root: using '%s'\n", named_swap_root);
	return 1;
}
__setup("named_swap.root=", named_swap_root_setup);

static int __init named_swap_fs_root_setup(char *str)
{
	int ret;

	if (!str || !*str) {
		pr_warn("named_swap.fs_root: empty path ignored\n");
		return 0;
	}

	ret = named_swap_set_path(named_swap_fs_root,
				  sizeof(named_swap_fs_root), str);
	if (ret)
		pr_warn("named_swap.fs_root: invalid path '%s' (%d), keeping '%s'\n",
			str, ret, named_swap_fs_root);
	else
		pr_info("named_swap.fs_root: using '%s'\n", named_swap_fs_root);
	return 1;
}
__setup("named_swap.fs_root=", named_swap_fs_root_setup);

static int __init named_swap_device_setup(char *str)
{
	int ret;

	if (!str) {
		pr_warn("named_swap.device: empty path ignored\n");
		return 0;
	}

	ret = named_swap_set_device(str);
	if (ret)
		pr_warn("named_swap.device: invalid path '%s' (%d)\n", str, ret);
	else if (named_swap_device[0])
		pr_info("named_swap.device: using '%s'\n", named_swap_device);
	return 1;
}
__setup("named_swap.device=", named_swap_device_setup);

static int __init named_swap_mode_setup(char *str)
{
	int ret;

	if (!str || !*str) {
		pr_warn("named_swap.mode: empty value ignored\n");
		return 0;
	}

	ret = named_swap_set_mode_name(str);
	if (ret)
		pr_warn("named_swap.mode: invalid value '%s'\n", str);
	else
		pr_info("named_swap.mode: using '%s'\n", named_swap_mode_name);
	return 1;
}
__setup("named_swap.mode=", named_swap_mode_setup);

static int __init named_swap_fs_free_setup(char *str)
{
	int val;

	if (!str || kstrtoint(str, 0, &val) || val < 0 || val > 99) {
		pr_warn("named_swap.fs_free: invalid value '%s', keeping %d\n",
			str ? str : "", named_swap_fs_free);
		return 0;
	}
	named_swap_fs_free = val;
	pr_info("named_swap.fs_free: using %d\n", named_swap_fs_free);
	return 1;
}
__setup("named_swap.fs_free=", named_swap_fs_free_setup);

static int __init named_swap_freerun_setup(char *str)
{
	int val;

	if (!str || kstrtoint(str, 0, &val) || (val != 0 && val != 1)) {
		pr_warn("named_swap.freerun: invalid value '%s', keeping %d\n",
			str ? str : "", named_swap_freerun);
		return 0;
	}
	named_swap_freerun = val;
	pr_info("named_swap.freerun: using %d\n", named_swap_freerun);
	return 1;
}
__setup("named_swap.freerun=", named_swap_freerun_setup);

static int proc_named_swap_set_string(const struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos,
				      int (*setter)(const char *))
{
	char tmp[NAMED_SWAP_PATH_LEN];
	struct ctl_table fake;
	int ret;

	if (!write)
		return proc_dostring(table, write, buffer, lenp, ppos);

	if (READ_ONCE(storage_ready))
		return -EBUSY;

	fake = *table;
	fake.data = tmp;
	fake.maxlen = sizeof(tmp);
	memset(tmp, 0, sizeof(tmp));
	ret = proc_dostring(&fake, 1, buffer, lenp, ppos);
	if (ret)
		return ret;

	if (tmp[0] && tmp[strlen(tmp) - 1] == '\n')
		tmp[strlen(tmp) - 1] = '\0';

	mutex_lock(&named_swap_storage_lock);
	if (READ_ONCE(storage_ready)) {
		mutex_unlock(&named_swap_storage_lock);
		return -EBUSY;
	}
	ret = setter(tmp);
	mutex_unlock(&named_swap_storage_lock);
	return ret;
}

static int set_root_sysctl(const char *path)
{
	return named_swap_set_path(named_swap_root, sizeof(named_swap_root),
				   path);
}

static int set_fs_root_sysctl(const char *path)
{
	return named_swap_set_path(named_swap_fs_root,
				  sizeof(named_swap_fs_root), path);
}

int proc_named_swap_root(const struct ctl_table *table, int write,
			 void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_named_swap_set_string(table, write, buffer, lenp, ppos,
					  set_root_sysctl);
}
EXPORT_SYMBOL_GPL(proc_named_swap_root);

int proc_named_swap_fs_root(const struct ctl_table *table, int write,
			    void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_named_swap_set_string(table, write, buffer, lenp, ppos,
					  set_fs_root_sysctl);
}
EXPORT_SYMBOL_GPL(proc_named_swap_fs_root);

int proc_named_swap_device(const struct ctl_table *table, int write,
			   void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_named_swap_set_string(table, write, buffer, lenp, ppos,
					  named_swap_set_device);
}
EXPORT_SYMBOL_GPL(proc_named_swap_device);

int proc_named_swap_mode(const struct ctl_table *table, int write,
			 void *buffer, size_t *lenp, loff_t *ppos)
{
	char tmp[16];
	struct ctl_table fake;
	enum named_swap_storage_mode mode;
	int ret;

	if (!write) {
		fake = *table;
		if (READ_ONCE(storage_ready))
			fake.data = (void *)mode_to_name(named_swap_mode_resolved);
		else if (named_swap_mode_explicit)
			fake.data = named_swap_mode_name;
		else if (!resolve_mode(&mode))
			fake.data = (void *)mode_to_name(mode);
		else
			fake.data = named_swap_mode_name;
		fake.maxlen = 16;
		return proc_dostring(&fake, 0, buffer, lenp, ppos);
	}

	if (READ_ONCE(storage_ready))
		return -EBUSY;

	fake = *table;
	fake.data = tmp;
	fake.maxlen = sizeof(tmp);
	memset(tmp, 0, sizeof(tmp));
	ret = proc_dostring(&fake, 1, buffer, lenp, ppos);
	if (ret)
		return ret;
	if (tmp[0] && tmp[strlen(tmp) - 1] == '\n')
		tmp[strlen(tmp) - 1] = '\0';

	mutex_lock(&named_swap_storage_lock);
	if (READ_ONCE(storage_ready)) {
		mutex_unlock(&named_swap_storage_lock);
		return -EBUSY;
	}
	ret = named_swap_set_mode_name(tmp);
	mutex_unlock(&named_swap_storage_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(proc_named_swap_mode);

static u64 kstatfs_to_pages(u64 blocks, long bsize)
{
	u64 bytes;

	if (bsize <= 0)
		bsize = PAGE_SIZE;
	if (check_mul_overflow(blocks, (u64)bsize, &bytes))
		return U64_MAX >> PAGE_SHIFT;
	return bytes >> PAGE_SHIFT;
}

static int pool_refresh_locked(enum named_swap_storage_pool id)
{
	struct named_swap_pool *p = &pools[id];
	struct kstatfs st;
	u64 total, avail, hard;
	unsigned long usage;
	long bsize;
	unsigned int reserve;
	int ret;

	if (!p->bound) {
		p->total_pages = 0;
		p->free_pages = 0;
		p->hard_pages = 0;
		return 0;
	}

	ret = vfs_statfs(&p->path, &st);
	if (ret)
		return ret;

	bsize = st.f_frsize ? st.f_frsize : st.f_bsize;
	total = kstatfs_to_pages(st.f_blocks, bsize);
	avail = kstatfs_to_pages(st.f_bavail, bsize);
	usage = atomic_long_read(&p->usage);
	p->total_pages = total;

	if (id == NAMED_SWAP_POOL_SWAP) {
		p->hard_pages = total;
		p->free_pages = total > usage ? total - usage : 0;
		return 0;
	}

	reserve = named_swap_fs_free;
	if (reserve > 99)
		reserve = 99;
	if (check_mul_overflow(total, (u64)(100 - reserve), &hard))
		hard = U64_MAX;
	else
		hard = div_u64(hard, 100);
	p->hard_pages = hard;
	p->free_pages = avail;
	return 0;
}

static void pool_unbind(enum named_swap_storage_pool id)
{
	struct named_swap_pool *p = &pools[id];

	if (!p->bound)
		return;
	path_put(&p->path);
	memset(&p->path, 0, sizeof(p->path));
	p->sb = NULL;
	p->bound = false;
	p->total_pages = 0;
	p->free_pages = 0;
	p->hard_pages = 0;
}

static void storage_unbind_all(void)
{
	pool_unbind(NAMED_SWAP_POOL_SWAP);
	pool_unbind(NAMED_SWAP_POOL_FS);
	WRITE_ONCE(storage_ready, false);
}

static bool sb_ephemeral(struct super_block *sb)
{
	if (!sb || !sb->s_type)
		return true;
	if (sb->s_magic == RAMFS_MAGIC || sb->s_magic == TMPFS_MAGIC)
		return true;
	return !strcmp(sb->s_type->name, "rootfs");
}

static int storage_mkdir(const char *path)
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
			dentry, 0700);
	done_path_create(&parent, dentry);
	return ret == -EEXIST ? 0 : ret;
}

static int pool_bind(enum named_swap_storage_pool id)
{
	struct named_swap_pool *p = &pools[id];
	struct path path;
	dev_t want;
	int ret;

	ret = storage_mkdir(pool_dir(id));
	if (ret)
		return ret == -ENOENT ? -EAGAIN : ret;

	ret = kern_path(pool_dir(id), LOOKUP_DIRECTORY, &path);
	if (ret)
		return ret == -ENOENT ? -EAGAIN : ret;

	if (sb_ephemeral(path.dentry->d_sb)) {
		path_put(&path);
		return -EAGAIN;
	}
	if (sb_rdonly(path.dentry->d_sb)) {
		path_put(&path);
		return -EROFS;
	}

	if (id == NAMED_SWAP_POOL_SWAP && named_swap_device[0]) {
		ret = lookup_bdev(named_swap_device, &want);
		if (ret) {
			path_put(&path);
			return ret == -ENOENT ? -EAGAIN : ret;
		}
		if (path.dentry->d_sb->s_dev != want) {
			path_put(&path);
			return -EAGAIN;
		}
	}

	p->path = path;
	p->sb = path.dentry->d_sb;
	p->bound = true;
	ret = pool_refresh_locked(id);
	if (ret) {
		pool_unbind(id);
		return ret;
	}
	return 0;
}

int named_swap_storage_setup(void)
{
	enum named_swap_storage_mode mode;
	int ret;

	mutex_lock(&named_swap_storage_lock);
	if (READ_ONCE(storage_ready)) {
		mutex_unlock(&named_swap_storage_lock);
		return 0;
	}

	ret = resolve_mode(&mode);
	if (ret) {
		pr_warn("named_swap: invalid storage mode (device required for swap/hybrid)\n");
		goto out;
	}

	if (pool_used_by_mode(mode, NAMED_SWAP_POOL_SWAP)) {
		ret = pool_bind(NAMED_SWAP_POOL_SWAP);
		if (ret)
			goto out_unbind;
	}
	if (pool_used_by_mode(mode, NAMED_SWAP_POOL_FS)) {
		ret = pool_bind(NAMED_SWAP_POOL_FS);
		if (ret)
			goto out_unbind;
	}

	named_swap_mode_resolved = mode;
	strscpy(named_swap_mode_name, mode_to_name(mode),
		sizeof(named_swap_mode_name));
	WRITE_ONCE(storage_ready, true);
	pr_info("named_swap: storage mode=%s device=%s swap=%s fs=%s\n",
		mode_to_name(mode),
		named_swap_device[0] ? named_swap_device : "(none)",
		pool_used_by_mode(mode, NAMED_SWAP_POOL_SWAP) ?
			named_swap_root : "(n/a)",
		pool_used_by_mode(mode, NAMED_SWAP_POOL_FS) ?
			named_swap_fs_root : "(n/a)");
	ret = 0;
	goto out;

out_unbind:
	storage_unbind_all();
out:
	mutex_unlock(&named_swap_storage_lock);
	return ret;
}

static int reserve_on_pool(enum named_swap_storage_pool pool,
			   unsigned long pages)
{
	struct named_swap_pool *p = &pools[pool];
	unsigned long usage;
	int ret;

	if (!pages)
		return 0;
	if (!p->bound)
		return -ENOSPC;

	ret = pool_refresh_locked(pool);
	if (ret)
		return ret;

	usage = atomic_long_read(&p->usage);
	if (!named_swap_freerun && usage + pages > p->hard_pages)
		return -ENOSPC;
	atomic_long_add(pages, &p->usage);
	return 0;
}

int named_swap_storage_reserve_pool(unsigned long pages,
				    enum named_swap_storage_pool pool)
{
	int ret;

	if (pool != NAMED_SWAP_POOL_SWAP && pool != NAMED_SWAP_POOL_FS)
		return -EINVAL;

	mutex_lock(&named_swap_storage_lock);
	if (!READ_ONCE(storage_ready)) {
		mutex_unlock(&named_swap_storage_lock);
		return -EINVAL;
	}
	ret = reserve_on_pool(pool, pages);
	mutex_unlock(&named_swap_storage_lock);
	return ret;
}

int named_swap_storage_reserve(unsigned long pages,
			       enum named_swap_storage_pool *chosen)
{
	int ret;

	mutex_lock(&named_swap_storage_lock);
	if (!READ_ONCE(storage_ready)) {
		mutex_unlock(&named_swap_storage_lock);
		return -EINVAL;
	}

	switch (named_swap_mode_resolved) {
	case NAMED_SWAP_STORAGE_SWAP:
		ret = reserve_on_pool(NAMED_SWAP_POOL_SWAP, pages);
		if (!ret && chosen)
			*chosen = NAMED_SWAP_POOL_SWAP;
		break;
	case NAMED_SWAP_STORAGE_FS:
		ret = reserve_on_pool(NAMED_SWAP_POOL_FS, pages);
		if (!ret && chosen)
			*chosen = NAMED_SWAP_POOL_FS;
		break;
	case NAMED_SWAP_STORAGE_HYBRID:
		ret = reserve_on_pool(NAMED_SWAP_POOL_SWAP, pages);
		if (!ret) {
			if (chosen)
				*chosen = NAMED_SWAP_POOL_SWAP;
			break;
		}
		if (ret != -ENOSPC)
			break;
		ret = reserve_on_pool(NAMED_SWAP_POOL_FS, pages);
		if (!ret && chosen)
			*chosen = NAMED_SWAP_POOL_FS;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&named_swap_storage_lock);
	return ret;
}

void named_swap_storage_release(unsigned long pages,
				enum named_swap_storage_pool pool)
{
	struct named_swap_pool *p;

	if (!pages)
		return;
	if (pool != NAMED_SWAP_POOL_SWAP && pool != NAMED_SWAP_POOL_FS)
		return;

	p = &pools[pool];
	if (WARN_ON_ONCE(atomic_long_read(&p->usage) < pages)) {
		atomic_long_set(&p->usage, 0);
		return;
	}
	atomic_long_sub(pages, &p->usage);
}

int named_swap_storage_build_path(u64 index, enum named_swap_storage_pool pool,
				  char *path, size_t len)
{
	int ret;

	if (pool != NAMED_SWAP_POOL_SWAP && pool != NAMED_SWAP_POOL_FS)
		return -EINVAL;

	ret = scnprintf(path, len, "%s/%llu", pool_dir(pool), index);
	if (ret >= len)
		return -ENAMETOOLONG;
	return 0;
}

unsigned long named_swap_total_pages(void)
{
	return atomic_long_read(&pools[NAMED_SWAP_POOL_SWAP].usage) +
	       atomic_long_read(&pools[NAMED_SWAP_POOL_FS].usage);
}

bool named_swap_storage_pool_used(enum named_swap_storage_pool pool)
{
	if (pool != NAMED_SWAP_POOL_SWAP && pool != NAMED_SWAP_POOL_FS)
		return false;
	return pools[pool].bound;
}

const char *named_swap_storage_pool_dir(enum named_swap_storage_pool pool)
{
	return pool_dir(pool);
}

const char *named_swap_storage_primary_dir(void)
{
	if (READ_ONCE(storage_ready) &&
	    named_swap_mode_resolved == NAMED_SWAP_STORAGE_FS)
		return named_swap_fs_root;
	return named_swap_root;
}

enum named_swap_storage_mode named_swap_storage_mode(void)
{
	enum named_swap_storage_mode mode;

	if (READ_ONCE(storage_ready))
		return named_swap_mode_resolved;
	if (!resolve_mode(&mode))
		return mode;
	return NAMED_SWAP_STORAGE_FS;
}

static int pool_stat(enum named_swap_storage_pool pool, int which,
		     unsigned long *val)
{
	struct named_swap_pool *p;
	int ret = 0;

	if (pool != NAMED_SWAP_POOL_SWAP && pool != NAMED_SWAP_POOL_FS)
		return -EINVAL;

	mutex_lock(&named_swap_storage_lock);
	p = &pools[pool];
	if (p->bound)
		ret = pool_refresh_locked(pool);
	if (ret) {
		mutex_unlock(&named_swap_storage_lock);
		return ret;
	}

	switch (which) {
	case NAMED_SWAP_POOL_STAT_USAGE:
		*val = atomic_long_read(&p->usage);
		break;
	case NAMED_SWAP_POOL_STAT_FREE:
		*val = p->bound ? p->free_pages : 0;
		break;
	case NAMED_SWAP_POOL_STAT_TOTAL:
		*val = p->bound ? p->total_pages : 0;
		break;
	case NAMED_SWAP_POOL_STAT_HARD:
		*val = p->bound ? p->hard_pages : 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&named_swap_storage_lock);
	return ret;
}

int proc_named_swap_pool_stat(const struct ctl_table *table, int write,
			      void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = 0;
	struct ctl_table fake;
	enum named_swap_storage_pool pool;
	int which;
	int ret;

	if (write)
		return -EPERM;

	pool = (enum named_swap_storage_pool)(unsigned long)table->extra1;
	which = (int)(unsigned long)table->extra2;
	ret = pool_stat(pool, which, &val);
	if (ret)
		return ret;

	fake = *table;
	fake.data = &val;
	fake.maxlen = sizeof(val);
	fake.extra1 = SYSCTL_LONG_ZERO;
	fake.extra2 = SYSCTL_LONG_MAX;
	return proc_doulongvec_minmax(&fake, 0, buffer, lenp, ppos);
}
EXPORT_SYMBOL_GPL(proc_named_swap_pool_stat);

int proc_named_swap_storage(const struct ctl_table *table, int write,
			    void *buffer, size_t *lenp, loff_t *ppos)
{
	char buf[512];
	struct ctl_table fake;
	unsigned long su = 0, sf = 0, st = 0, sh = 0;
	unsigned long fu = 0, ff = 0, ft = 0, fh = 0;

	if (write)
		return -EPERM;

	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_USAGE, &su);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_FREE, &sf);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_TOTAL, &st);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_HARD, &sh);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_USAGE, &fu);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_FREE, &ff);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_TOTAL, &ft);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_HARD, &fh);

	scnprintf(buf, sizeof(buf),
		  "mode=%s device=%s root=%s fs_root=%s fs_free=%d freerun=%d\n"
		  "swap: usage=%lu free=%lu total=%lu hard=%lu\n"
		  "fs: usage=%lu free=%lu total=%lu hard=%lu\n",
		  mode_to_name(named_swap_storage_mode()),
		  named_swap_device[0] ? named_swap_device : "",
		  named_swap_root, named_swap_fs_root,
		  named_swap_fs_free, named_swap_freerun,
		  su, sf, st, sh, fu, ff, ft, fh);

	fake = *table;
	fake.data = buf;
	fake.maxlen = sizeof(buf);
	return proc_dostring(&fake, 0, buffer, lenp, ppos);
}
EXPORT_SYMBOL_GPL(proc_named_swap_storage);

#ifdef CONFIG_DEBUG_FS
DEFINE_SHOW_ATTRIBUTE(named_swap_hist);

static int named_swap_usage_show(struct seq_file *m, void *v)
{
	unsigned long su = 0, sf = 0, st = 0, sh = 0;
	unsigned long fu = 0, ff = 0, ft = 0, fh = 0;

	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_USAGE, &su);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_FREE, &sf);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_TOTAL, &st);
	pool_stat(NAMED_SWAP_POOL_SWAP, NAMED_SWAP_POOL_STAT_HARD, &sh);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_USAGE, &fu);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_FREE, &ff);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_TOTAL, &ft);
	pool_stat(NAMED_SWAP_POOL_FS, NAMED_SWAP_POOL_STAT_HARD, &fh);

	seq_printf(m, "mode=%s device=%s fs_free=%d freerun=%d\n",
		   mode_to_name(named_swap_storage_mode()),
		   named_swap_device[0] ? named_swap_device : "",
		   named_swap_fs_free, named_swap_freerun);
	seq_printf(m, "%s: usage=%lu free=%lu total=%lu hard=%lu%s\n",
		   named_swap_root, su, sf, st, sh,
		   pools[NAMED_SWAP_POOL_SWAP].bound ? "" : " (unused)");
	seq_printf(m, "%s: usage=%lu free=%lu total=%lu hard=%lu%s\n",
		   named_swap_fs_root, fu, ff, ft, fh,
		   pools[NAMED_SWAP_POOL_FS].bound ? "" : " (unused)");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(named_swap_usage);

static int __init named_swap_debugfs_init(void)
{
	struct dentry *dir;

	if (!debugfs_initialized())
		return 0;

	dir = debugfs_create_dir("named_swap", NULL);
	debugfs_create_file("usage", 0444, dir, NULL, &named_swap_usage_fops);
	debugfs_create_file("hist", 0444, dir, NULL, &named_swap_hist_fops);
	return 0;
}
late_initcall(named_swap_debugfs_init);
#endif
