/*
 * device-mapper.c
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This software is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Changelog
 *
 *    14/08/2001 - First Version [Joe Thornber]
 */

#include "dm.h"

/* defines for blk.h */
#define MAJOR_NR DM_BLK_MAJOR
#define DEVICE_NR(device) MINOR(device)	/* has no partition bits */
#define DEVICE_NAME "device-mapper"	/* name for messaging */
#define DEVICE_NO_RANDOM		/* no entropy to contribute */
#define DEVICE_OFF(d)			/* do-nothing */

#include <linux/blk.h>
#include <linux/blkpg.h>
#include <linux/hdreg.h>
#include <linux/lvm.h>
#include <linux/kmod.h>

#define MAX_DEVICES 64
#define DEFAULT_READ_AHEAD 64

const char *_name = "device-mapper";
int _version[3] = { 0, 1, 0 };

struct io_hook {
	struct dm_table *table;
	struct target *target;
	int rw;
	void (*end_io) (struct buffer_head * bh, int uptodate);
	void *context;
};

kmem_cache_t *_io_hook_cache;

struct rw_semaphore _dev_lock;
static struct mapped_device *_devs[MAX_DEVICES];

/* block device arrays */
static int _block_size[MAX_DEVICES];
static int _blksize_size[MAX_DEVICES];
static int _hardsect_size[MAX_DEVICES];

const char *_fs_dir = "device-mapper";
static devfs_handle_t _dev_dir;

static int request(request_queue_t *q, int rw, struct buffer_head *bh);
static int dm_user_bmap(struct inode *inode, struct lv_bmap *lvb);

/*
 * setup and teardown the driver
 */
static int dm_init(void)
{
	int ret;

	init_rwsem(&_dev_lock);

	if (!(_io_hook_cache =
	      kmem_cache_create("dm io hooks", sizeof (struct io_hook),
				0, 0, NULL, NULL)))
		return -ENOMEM;

	if ((ret = dmfs_init()) || (ret = dm_target_init())
	    || (ret = dm_init_blkdev()))
		return ret;

	/* set up the arrays */
	read_ahead[MAJOR_NR] = DEFAULT_READ_AHEAD;
	blk_size[MAJOR_NR] = _block_size;
	blksize_size[MAJOR_NR] = _blksize_size;
	hardsect_size[MAJOR_NR] = _hardsect_size;

	if (devfs_register_blkdev(MAJOR_NR, _name, &dm_blk_dops) < 0) {
		printk(KERN_ERR "%s -- register_blkdev failed\n", _name);
		return -EIO;
	}

	blk_queue_make_request(BLK_DEFAULT_QUEUE(MAJOR_NR), request);

	_dev_dir = devfs_mk_dir(0, _fs_dir, NULL);

	printk(KERN_INFO "%s %d.%d.%d initialised\n", _name,
	       _version[0], _version[1], _version[2]);
	return 0;
}

static void dm_exit(void)
{
	if (kmem_cache_destroy(_io_hook_cache))
		WARN("it looks like there are still some io_hooks allocated");

	dmfs_exit();
	dm_cleanup_blkdev();

	if (devfs_unregister_blkdev(MAJOR_NR, _name) < 0)
		printk(KERN_ERR "%s -- unregister_blkdev failed\n", _name);

	read_ahead[MAJOR_NR] = 0;
	blk_size[MAJOR_NR] = 0;
	blksize_size[MAJOR_NR] = 0;
	hardsect_size[MAJOR_NR] = 0;

	printk(KERN_INFO "%s %d.%d.%d cleaned up\n", _name,
	       _version[0], _version[1], _version[2]);
}

/*
 * block device functions
 */
static int dm_blk_open(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	down_write(&_dev_lock);
	md = _devs[minor];

	if (!md || !is_active(md)) {
		up_write(&_dev_lock);
		return -ENXIO;
	}

	md->use_count++;
	up_write(&_dev_lock);

	MOD_INC_USE_COUNT;
	return 0;
}

static int dm_blk_close(struct inode *inode, struct file *file)
{
	int minor = MINOR(inode->i_rdev);
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	down_write(&_dev_lock);
	md = _devs[minor];
	if (!md || md->use_count < 1) {
		WARN("reference count in mapped_device incorrect");
		up_write(&_dev_lock);
		return -ENXIO;
	}

	md->use_count--;
	up_write(&_dev_lock);

	MOD_DEC_USE_COUNT;
	return 0;
}

#define VOLUME_SIZE(minor) ((_block_size[(minor)] << 10) / \
			    _hardsect_size[(minor)])

static int dm_blk_ioctl(struct inode *inode, struct file *file,
			uint command, ulong a)
{
	int minor = MINOR(inode->i_rdev);
	long size;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	switch (command) {
	case BLKSSZGET:
	case BLKROGET:
	case BLKROSET:
#if 0
	case BLKELVSET:
	case BLKELVGET:
#endif
		return blk_ioctl(inode->i_dev, command, a);
		break;

	case HDIO_GETGEO:
		{
			struct hd_geometry tmp = { heads:64, sectors:32 };

			tmp.cylinders = VOLUME_SIZE(minor) / tmp.heads /
			    tmp.sectors;

			if (copy_to_user((char *) a, &tmp, sizeof (tmp)))
				return -EFAULT;
			break;
		}

	case HDIO_GETGEO_BIG:
		{
			struct hd_big_geometry tmp = { heads:64, sectors:32 };

			tmp.cylinders = VOLUME_SIZE(minor) / tmp.heads /
			    tmp.sectors;

			if (copy_to_user((char *) a, &tmp, sizeof (tmp)))
				return -EFAULT;
			break;
		}

	case BLKGETSIZE:
		size = VOLUME_SIZE(minor);
		if (copy_to_user((void *) a, &size, sizeof (long)))
			return -EFAULT;
		break;

	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		fsync_dev(inode->i_rdev);
		invalidate_buffers(inode->i_rdev);
		return 0;

	case BLKRAGET:
		if (copy_to_user
		    ((void *) a, &read_ahead[MAJOR(inode->i_rdev)],
		     sizeof (long)))
			return -EFAULT;
		return 0;

	case BLKRASET:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		read_ahead[MAJOR(inode->i_rdev)] = a;
		return 0;

	case BLKRRPART:
		return -EINVAL;

	case LV_BMAP:
		return dm_user_bmap(inode, (struct lv_bmap *)a);

	default:
		printk(KERN_WARNING "%s - unknown block ioctl %d",
		       _name, command);
		return -EINVAL;
	}

	return 0;
}

static inline struct io_hook *alloc_io_hook(void)
{
	return kmem_cache_alloc(_io_hook_cache, GFP_NOIO);
}

static inline void free_io_hook(struct io_hook *ih)
{
	kmem_cache_free(_io_hook_cache, ih);
}

/*
 * FIXME: need to decide if deferred_io's need
 * their own slab, I say no for now since they are
 * only used when the device is suspended.
 */
static inline struct deferred_io *alloc_deferred(void)
{
	return kmalloc(sizeof (struct deferred_io), GFP_NOIO);
}

static inline void free_deferred(struct deferred_io *di)
{
	kfree(di);
}

/*
 * bh->b_end_io routine that decrements the
 * pending count and then calls the original
 * bh->b_end_io fn.
 */
static void dec_pending(struct buffer_head *bh, int uptodate)
{
	struct io_hook *ih = bh->b_private;

	if (!uptodate && ih->target->type->err) {
		if (ih->target->type->err(bh, ih->rw, ih->target->private))
			return;
	}

	if (atomic_dec_and_test(&ih->table->pending))
		/* nudge anyone waiting on suspend queue */
		wake_up(&ih->table->wait);

	bh->b_end_io = ih->end_io;
	bh->b_private = ih->context;
	free_io_hook(ih);

	bh->b_end_io(bh, uptodate);
}

/*
 * add the bh to the list of deferred io.
 */
static int queue_io(struct mapped_device *md, struct buffer_head *bh, int rw)
{
	struct deferred_io *di = alloc_deferred();

	if (!di)
		return -ENOMEM;

	down_write(&_dev_lock);
	if (test_bit(DM_ACTIVE, &md->state)) {
		up_write(&_dev_lock);
		return 0;
	}

	di->bh = bh;
	di->rw = rw;
	di->next = md->deferred;
	md->deferred = di;
	up_write(&_dev_lock);

	return 1;
}

/*
 * do the bh mapping for a given leaf
 */
static inline int __map_buffer(struct mapped_device *md,
			       struct buffer_head *bh, int rw, int leaf)
{
	dm_map_fn fn;
	void *context;
	struct io_hook *ih = NULL;
	int r;
	struct target *ti = md->map->targets + leaf;

	fn = ti->type->map;
	context = ti->private;

	ih = alloc_io_hook();

	if (!ih)
		return 0;

	ih->table = md->map;
	ih->rw = rw;
	ih->target = ti;
	ih->end_io = bh->b_end_io;
	ih->context = bh->b_private;

	r = fn(bh, rw, context);

	if (r > 0) {
		/* hook the end io request fn */
		atomic_inc(&md->map->pending);
		bh->b_end_io = dec_pending;
		bh->b_private = ih;

	} else if (r == 0)
		/* we don't need to hook */
		free_io_hook(ih);

	else if (r < 0) {
		free_io_hook(ih);
		return 0;
	}

	return 1;
}

/*
 * search the btree for the correct target.
 */
static inline int __find_node(struct dm_table *t, struct buffer_head *bh)
{
	int l, n = 0, k = 0;
	offset_t *node;

	for (l = 0; l < t->depth; l++) {
		n = get_child(n, k);
		node = get_node(t, l, n);

		for (k = 0; k < KEYS_PER_NODE; k++)
			if (node[k] >= bh->b_rsector)
				break;
	}

	return (KEYS_PER_NODE * n) + k;
}

static int dm_user_bmap(struct inode *inode, struct lv_bmap *lvb)
{
	struct buffer_head bh;
	struct mapped_device *md;
	unsigned long block;
	int minor = MINOR(inode->i_rdev);
	int err;

	if (minor >= MAX_DEVICES)
		return -ENXIO;

	md = _devs[minor];
	if (md == NULL)
		return -ENXIO;

	if (get_user(block, &lvb->lv_block))
		return -EFAULT;

	memset(&bh, 0, sizeof(bh));
	bh.b_blocknr = block;
	bh.b_dev = bh.b_rdev = inode->i_rdev;
	bh.b_size = _blksize_size[minor];
	bh.b_rsector = block * (bh.b_size >> 9);

	err = -EINVAL;
	down_read(&_dev_lock);
	if (test_bit(DM_ACTIVE, &md->state)) {
		struct target *t = md->map->targets + __find_node(md->map, &bh);
		struct target_type *target = t->type;
		if (target->flags & TF_BMAP) {
			err = target->map(&bh, READ, t->private);
			if (bh.b_private) {
				struct io_hook *ih = (struct io_hook *)bh.b_private;
				free_io_hook(ih);
			}
			err = (err == 0) ? -EINVAL : 0;
		}
	}
	up_read(&_dev_lock);

	if (err == 0) {
		if (put_user(kdev_t_to_nr(bh.b_rdev), &lvb->lv_dev))
			return -EFAULT;
		if (put_user(bh.b_rsector / (bh.b_size >> 9), &lvb->lv_dev))
			return -EFAULT;
	}

	return err;
}

static int request(request_queue_t *q, int rw, struct buffer_head *bh)
{
	struct mapped_device *md;
	int r, minor = MINOR(bh->b_rdev);

	if (minor >= MAX_DEVICES)
		goto bad_no_lock;

	down_read(&_dev_lock);
	md = _devs[minor];

	if (!md || !md->map)
		goto bad;

	/* if we're suspended we have to queue this io for later */
	if (!test_bit(DM_ACTIVE, &md->state)) {
		up_read(&_dev_lock);
		r = queue_io(md, bh, rw);

		if (r < 0)
			goto bad_no_lock;

		else if (r > 0)
			return 0;	/* deferred successfully */

		down_read(&_dev_lock);	/* FIXME: there's still a race here */
	}

	if (!__map_buffer(md, bh, rw, __find_node(md->map, bh)))
		goto bad;

	up_read(&_dev_lock);
	return 1;

      bad:
	up_read(&_dev_lock);

      bad_no_lock:
	buffer_IO_error(bh);
	return 0;
}

/*
 * see if the device with a specific minor # is
 * free.
 */
static inline int __specific_dev(int minor)
{
	if (minor > MAX_DEVICES) {
		WARN("request for a mapped_device > than MAX_DEVICES");
		return 0;
	}

	if (!_devs[minor])
		return minor;

	return -1;
}

/*
 * find the first free device.
 */
static inline int __any_old_dev(void)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++)
		if (!_devs[i])
			return i;

	return -1;
}

/*
 * allocate and initialise a blank device.
 */
static struct mapped_device *alloc_dev(int minor)
{
	struct mapped_device *md = kmalloc(sizeof (*md), GFP_KERNEL);

	if (!md)
		return 0;

	memset(md, 0, sizeof (*md));

	down_write(&_dev_lock);
	minor = (minor < 0) ? __any_old_dev() : __specific_dev(minor);

	if (minor < 0) {
		WARN("no free devices available");
		up_write(&_dev_lock);
		kfree(md);
		return 0;
	}

	md->dev = MKDEV(DM_BLK_MAJOR, minor);
	md->name[0] = '\0';
	md->state = 0;

	_devs[minor] = md;
	up_write(&_dev_lock);

	return md;
}

struct mapped_device *dm_find_by_minor(int minor)
{
	struct mapped_device *md;

	down_read(&_dev_lock);
	md = _devs[minor];
	up_read(&_dev_lock);

	return md;
}

static int register_device(struct mapped_device *md)
{
	md->devfs_entry =
	    devfs_register(_dev_dir, md->name, DEVFS_FL_CURRENT_OWNER,
			   MAJOR(md->dev), MINOR(md->dev),
			   S_IFBLK | S_IRUSR | S_IWUSR | S_IRGRP,
			   &dm_blk_dops, NULL);

	if (!md->devfs_entry)
		return -ENOMEM;

	return 0;
}

static int unregister_device(struct mapped_device *md)
{
	devfs_unregister(md->devfs_entry);
	return 0;
}

#ifdef CONFIG_HOTPLUG
static void dm_sbin_hotplug(struct mapped_device *md, int create)
{
	int i;
	char *argv[3];
	char *envp[5];
	char name[DM_NAME_LEN + 16];

	if (!hotplug_path[0])
		return;

	sprintf(name, "DMNAME=%s\n", md->name);

	i = 0;
	argv[i++] = hotplug_path;
	argv[i++] = "devmap";
	argv[i] = 0;

	i = 0;
	envp[i++] = "HOME=/";
	envp[i++] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
	envp[i++] = name;
	if (create)
		envp[i++] = "ACTION=add";
	else
		envp[i++] = "ACTION=remove";
	envp[i] = 0;

	call_usermodehelper(argv[0], argv, envp);
}
#else
#define dm_sbin_hotplug(md, create) do { } while(0)
#endif /* CONFIG_HOTPLUG */

/*
 * constructor for a new device
 */
struct mapped_device *dm_create(const char *name, int minor)
{
	int r;
	struct mapped_device *md;

	if (minor >= MAX_DEVICES)
		return ERR_PTR(-ENXIO);

	if (!(md = alloc_dev(minor)))
		return ERR_PTR(-ENXIO);

	down_write(&_dev_lock);
	strcpy(md->name, name);
	if ((r = register_device(md))) {
		up_write(&_dev_lock);
		return ERR_PTR(r);
	}
	up_write(&_dev_lock);

	dm_sbin_hotplug(md, 1);

	return md;
}

/*
 * destructor for the device.  md->map is
 * deliberately not destroyed, dm-fs should manage
 * table objects.
 */
int dm_remove(struct mapped_device *md)
{
	int minor, r;

	down_write(&_dev_lock);
	if (md->use_count) {
		up_write(&_dev_lock);
		return -EPERM;
	}

	if ((r = unregister_device(md))) {
		up_write(&_dev_lock);
		return r;
	}

	minor = MINOR(md->dev);
	_devs[minor] = 0;
	up_write(&_dev_lock);

	dm_sbin_hotplug(md, 0);
	kfree(md);

	return 0;
}

/*
 * Bind a table to the device.
 */
void __bind(struct mapped_device *md, struct dm_table *t)
{
	int minor = MINOR(md->dev);

	md->map = t;

	_block_size[minor] = (t->highs[t->num_targets - 1] + 1) >> 1;

	/* FIXME: block size depends on the mapping table */
	_blksize_size[minor] = BLOCK_SIZE;
	_hardsect_size[minor] = t->hardsect_size;
	register_disk(NULL, md->dev, 1, &dm_blk_dops, _block_size[minor]);
}

/*
 * requeue the deferred buffer_heads by calling
 * generic_make_request.
 */
static void __flush_deferred_io(struct mapped_device *md)
{
	struct deferred_io *c, *n;

	for (c = md->deferred, md->deferred = 0; c; c = n) {
		n = c->next;
		generic_make_request(c->rw, c->bh);
		free_deferred(c);
	}
}

/*
 * make the device available for use, if was
 * previously suspended rather than newly created
 * then all queued io is flushed
 */
int dm_activate(struct mapped_device *md, struct dm_table *table)
{
	/* check that the mapping has at least been loaded. */
	if (!table->num_targets)
		return -EINVAL;

	down_write(&_dev_lock);

	/* you must be deactivated first */
	if (is_active(md)) {
		up_write(&_dev_lock);
		return -EPERM;
	}

	__bind(md, table);

	set_bit(DM_ACTIVE, &md->state);
	__flush_deferred_io(md);
	up_write(&_dev_lock);

	return 0;
}

/*
 * Deactivate the device, the device must not be
 * opened by anyone.
 */
int dm_deactivate(struct mapped_device *md)
{
	down_read(&_dev_lock);
	if (md->use_count) {
		up_read(&_dev_lock);
		return -EPERM;
	}

	fsync_dev(md->dev);

	up_read(&_dev_lock);

	down_write(&_dev_lock);
	if (md->use_count) {
		/* drat, somebody got in quick ... */
		up_write(&_dev_lock);
		return -EPERM;
	}

	md->map = 0;
	clear_bit(DM_ACTIVE, &md->state);
	up_write(&_dev_lock);

	return 0;
}

/*
 * We need to be able to change a mapping table
 * under a mounted filesystem.  for example we
 * might want to move some data in the background.
 * Before the table can be swapped with
 * dm_bind_table, dm_suspend must be called to
 * flush any in flight buffer_heads and ensure
 * that any further io gets deferred.
 */
void dm_suspend(struct mapped_device *md)
{
	DECLARE_WAITQUEUE(wait, current);

	down_write(&_dev_lock);
	if (!is_active(md)) {
		up_write(&_dev_lock);
		return;
	}

	clear_bit(DM_ACTIVE, &md->state);
	up_write(&_dev_lock);

	/* wait for all the pending io to flush */
	add_wait_queue(&md->map->wait, &wait);
	current->state = TASK_UNINTERRUPTIBLE;
	do {
		down_write(&_dev_lock);
		if (!atomic_read(&md->map->pending))
			break;

		up_write(&_dev_lock);
		schedule();

	} while (1);

	current->state = TASK_RUNNING;
	remove_wait_queue(&md->map->wait, &wait);

	md->map = 0;
	up_write(&_dev_lock);
}

struct block_device_operations dm_blk_dops = {
	open:	  dm_blk_open,
	release:  dm_blk_close,
	ioctl:	  dm_blk_ioctl
};

/*
 * module hooks
 */
module_init(dm_init);
module_exit(dm_exit);

MODULE_DESCRIPTION("device-mapper driver");
MODULE_AUTHOR("Joe Thornber <thornber@btconnect.com>");

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
