// SPDX-License-Identifier: GPL-2.0
#include <asm/cacheflush.h>
#include <asm/rsi.h>
#include <asm/rsi_cmds.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/coco-image-share.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define DRIVER_NAME "coco-image-share"
#define DRIVER_VERSION "0.1"
#define COCO_IMG_SHARE_MAGIC 0x43494d47U
#define COCO_IMG_SHARE_VERSION 1U
#define COCO_IMG_SHARE_MAX_PAGES 32768UL
#define COCO_IMG_MAX_WINDOW_SIZE SZ_256K
#define COCO_IMG_BLOCK_NAME "cocoimg0"

struct rsi_img_share_desc {
	u32 magic;
	u32 version;
	u64 image_size;
	u64 page_count;
	u64 flags;
};

struct rsi_img_share_meta {
	u32 magic;
	u32 version;
	u64 image_size;
	u64 page_count;
	u64 source_page_list_ipa;
	u64 flags;
};

struct rsi_img_page_desc {
	u64 source_ipa;
	u64 file_offset;
};

struct coco_img_share {
	struct list_head node;
	u64 share_id;
	u64 source_rd_addr;
	u64 image_size;
	u64 page_count;
	struct page **data_pages;
	struct file *source_file;
	void *page_list;
	size_t page_list_size;
	struct page *desc_page;
	struct page *meta_page;
};

struct coco_img_device {
	u64 share_id;
	u64 source_rd_addr;
	u64 image_size;
	u64 mapped_file_offset;
	u64 mapped_size;
	struct gendisk *disk;
};

static DEFINE_MUTEX(shares_lock);
static DEFINE_MUTEX(window_lock);
static DEFINE_MUTEX(coco_img_device_lock);
static LIST_HEAD(shares);
static phys_addr_t window_ipa_start;
static u64 window_size;
static void __iomem *window_mapping;
static struct coco_img_device *coco_img_dev;

static int rsi_status_to_errno(unsigned long status)
{
	switch (status) {
	case RSI_SUCCESS:
		return 0;
	case RSI_ERROR_INPUT:
		return -EINVAL;
	case RSI_ERROR_STATE:
		return -EBUSY;
	case RSI_INCOMPLETE:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

static void flush_linear_pages(void *addr, size_t size)
{
	size_t offset;

	for (offset = 0; offset < size; offset += PAGE_SIZE)
		flush_dcache_page(virt_to_page((char *)addr + offset));
}

static struct coco_img_share *find_share_locked(u64 share_id)
{
	struct coco_img_share *share;

	list_for_each_entry(share, &shares, node) {
		if (share->share_id == share_id)
			return share;
	}

	return NULL;
}

static void free_share(struct coco_img_share *share)
{
	unsigned long i;

	if (!share)
		return;

	if (share->data_pages) {
		for (i = 0; i < share->page_count; i++) {
			if (share->data_pages[i])
				put_page(share->data_pages[i]);
		}
		kvfree(share->data_pages);
	}

	if (share->source_file)
		filp_close(share->source_file, NULL);
	if (share->page_list)
		free_pages_exact(share->page_list, share->page_list_size);
	if (share->desc_page)
		__free_page(share->desc_page);
	if (share->meta_page)
		__free_page(share->meta_page);

	kfree(share);
}

static int pin_file_pages(struct file *filp, struct coco_img_share *share)
{
	unsigned long i;

	for (i = 0; i < share->page_count; i++) {
		struct page *page;

		page = read_mapping_page(filp->f_mapping, i, filp);
		if (IS_ERR(page))
			return PTR_ERR(page);

		share->data_pages[i] = page;
		flush_dcache_page(page);
	}

	return 0;
}

static int populate_page_list(struct coco_img_share *share)
{
	struct rsi_img_page_desc *descs = share->page_list;
	unsigned long i;

	for (i = 0; i < share->page_count; i++) {
		descs[i].source_ipa = page_to_phys(share->data_pages[i]);
		descs[i].file_offset = i * PAGE_SIZE;
	}

	flush_linear_pages(share->page_list, share->page_list_size);
	return 0;
}

static int fill_desc_page(struct coco_img_share *share)
{
	struct rsi_img_share_desc *desc;

	desc = kmap_local_page(share->desc_page);
	memset(desc, 0, PAGE_SIZE);
	desc->magic = COCO_IMG_SHARE_MAGIC;
	desc->version = COCO_IMG_SHARE_VERSION;
	desc->image_size = share->image_size;
	desc->page_count = share->page_count;
	desc->flags = COCO_IMAGE_SHARE_FLAG_RO;
	kunmap_local(desc);
	flush_dcache_page(share->desc_page);

	return 0;
}

static int fill_meta_page(struct coco_img_share *share)
{
	struct rsi_img_share_meta *meta;

	meta = kmap_local_page(share->meta_page);
	memset(meta, 0, PAGE_SIZE);
	meta->magic = COCO_IMG_SHARE_MAGIC;
	meta->version = COCO_IMG_SHARE_VERSION;
	meta->image_size = share->image_size;
	meta->page_count = share->page_count;
	meta->source_page_list_ipa = virt_to_phys(share->page_list);
	meta->flags = COCO_IMAGE_SHARE_FLAG_RO;
	kunmap_local(meta);
	flush_dcache_page(share->meta_page);

	return 0;
}

static int create_share_from_file(struct coco_image_share_create_from_file *req)
{
	struct coco_img_share *share;
	struct file *filp;
	unsigned long status;
	unsigned long share_id = 0;
	unsigned long added_pages = 0;
	unsigned long source_rd_addr = 0;
	loff_t size;
	int ret;

	if (req->flags & ~COCO_IMAGE_SHARE_FLAG_RO)
		return -EINVAL;

	req->path[COCO_IMAGE_SHARE_PATH_MAX - 1] = '\0';
	filp = filp_open(req->path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	size = i_size_read(file_inode(filp));
	if ((size <= 0) ||
	    (DIV_ROUND_UP_ULL(size, PAGE_SIZE) > COCO_IMG_SHARE_MAX_PAGES)) {
		ret = -E2BIG;
		goto out_close;
	}

	share = kzalloc(sizeof(*share), GFP_KERNEL);
	if (!share) {
		ret = -ENOMEM;
		goto out_close;
	}

	share->image_size = size;
	share->page_count = DIV_ROUND_UP_ULL(share->image_size, PAGE_SIZE);
	share->page_list_size = PAGE_ALIGN(share->page_count *
					   sizeof(struct rsi_img_page_desc));
	share->data_pages = kvcalloc(share->page_count,
				     sizeof(*share->data_pages), GFP_KERNEL);
	share->page_list = alloc_pages_exact(share->page_list_size,
					     GFP_KERNEL | __GFP_ZERO);
	share->desc_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	share->meta_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!share->data_pages || !share->page_list ||
	    !share->desc_page || !share->meta_page) {
		ret = -ENOMEM;
		goto out_free_share;
	}

	ret = pin_file_pages(filp, share);
	if (ret)
		goto out_free_share;
	share->source_file = filp;
	filp = NULL;

	ret = populate_page_list(share);
	if (ret)
		goto out_free_share;

	fill_desc_page(share);
	fill_meta_page(share);

	status = rsi_get_rd_addr(&source_rd_addr);
	ret = rsi_status_to_errno(status);
	if (ret)
		goto out_free_share;

	status = rsi_img_share_create(page_to_phys(share->desc_page), &share_id);
	ret = rsi_status_to_errno(status);
	if (ret)
		goto out_free_share;

	status = rsi_img_share_add_pages(share_id, virt_to_phys(share->page_list),
					0, share->page_count, &added_pages);
	ret = rsi_status_to_errno(status);
	if (ret || added_pages != share->page_count) {
		if (!ret)
			ret = -EIO;
		goto out_destroy_rmm;
	}
	pr_info("%s: share %lu added %lu pages from %s\n", DRIVER_NAME,
		share_id, added_pages, req->path);

	status = rsi_img_share_seal(share_id, page_to_phys(share->meta_page),
				    0, 0);
	ret = rsi_status_to_errno(status);
	if (ret)
		goto out_destroy_rmm;
	pr_info("%s: share %lu sealed size=%llu pages=%llu source_rd=0x%lx\n",
		DRIVER_NAME, share_id, share->image_size, share->page_count,
		source_rd_addr);

	share->share_id = share_id;
	share->source_rd_addr = source_rd_addr;

	mutex_lock(&shares_lock);
	list_add_tail(&share->node, &shares);
	mutex_unlock(&shares_lock);

	req->share_id = share->share_id;
	req->source_rd_addr = share->source_rd_addr;
	req->image_size = share->image_size;
	req->page_count = share->page_count;

	return 0;

out_destroy_rmm:
	rsi_img_share_destroy(share_id);
out_free_share:
	free_share(share);
out_close:
	if (filp)
		filp_close(filp, NULL);
	return ret;
}

static int destroy_share(u64 share_id)
{
	struct coco_img_share *share;
	unsigned long status;
	int ret;

	mutex_lock(&shares_lock);
	share = find_share_locked(share_id);
	if (!share) {
		mutex_unlock(&shares_lock);
		return -ENOENT;
	}
	list_del(&share->node);
	mutex_unlock(&shares_lock);

	status = rsi_img_share_destroy(share_id);
	ret = rsi_status_to_errno(status);
	if (ret) {
		mutex_lock(&shares_lock);
		list_add_tail(&share->node, &shares);
		mutex_unlock(&shares_lock);
		return ret;
	}

	free_share(share);
	return 0;
}

static bool window_range_valid(u64 offset, u64 size)
{
	return window_size && size && offset < window_size &&
	       size <= window_size - offset;
}

static int attach_window(struct coco_image_share_attach *req)
{
	unsigned long mapped_pages = 0;
	unsigned long status;
	int ret;

	if ((req->flags & ~COCO_IMAGE_SHARE_FLAG_RO) ||
	    !IS_ALIGNED(req->window_offset, PAGE_SIZE) ||
	    !IS_ALIGNED(req->file_offset, PAGE_SIZE) ||
	    !IS_ALIGNED(req->size, PAGE_SIZE) ||
	    !window_range_valid(req->window_offset, req->size))
		return -EINVAL;

	req->target_ipa = window_ipa_start + req->window_offset;
	status = rsi_img_share_attach(req->share_id, req->source_rd_addr,
				      req->target_ipa, req->file_offset,
				      req->size, req->flags, &mapped_pages);
	ret = rsi_status_to_errno(status);
	req->mapped_pages = mapped_pages;
	if (ret)
			pr_err_ratelimited("%s: attach failed share=%llu source_rd=0x%llx target=0x%llx file_off=0x%llx size=0x%llx flags=0x%llx status=%lu ret=%d mapped=%lu\n",
					    DRIVER_NAME, req->share_id,
					    req->source_rd_addr, req->target_ipa,
					    req->file_offset, req->size, req->flags,
					    status, ret, mapped_pages);

	return ret;
}

static int detach_window(struct coco_image_share_detach *req)
{
	unsigned long status;

	if (!IS_ALIGNED(req->window_offset, PAGE_SIZE) ||
	    !IS_ALIGNED(req->size, PAGE_SIZE) ||
	    !window_range_valid(req->window_offset, req->size))
		return -EINVAL;

	req->target_ipa = window_ipa_start + req->window_offset;
	status = rsi_img_share_detach(req->target_ipa, req->size);

	return rsi_status_to_errno(status);
}

static int detach_device_window(struct coco_img_device *dev)
{
	struct coco_image_share_detach detach = {
		.window_offset = 0,
		.size = dev->mapped_size,
	};
	int ret;

	if (!dev->mapped_size)
		return 0;

	ret = detach_window(&detach);
	if (ret) {
		pr_warn_ratelimited("%s: detach device window failed share=%llu file_off=0x%llx size=0x%llx ret=%d\n",
				    DRIVER_NAME, dev->share_id,
				    dev->mapped_file_offset, dev->mapped_size,
				    ret);
		return ret;
	}

	dev->mapped_file_offset = 0;
	dev->mapped_size = 0;
	return 0;
}

static int map_device_window(struct coco_img_device *dev, u64 file_offset)
{
	struct coco_image_share_attach attach;
	u64 map_offset, map_size, max_window;
	int ret;

	if (file_offset >= dev->image_size)
		return -EINVAL;
	if (!window_mapping || !window_size)
		return -ENODEV;

	if (dev->mapped_size &&
	    file_offset >= dev->mapped_file_offset &&
	    file_offset < dev->mapped_file_offset + dev->mapped_size)
		return 0;

	max_window = min_t(u64, window_size, COCO_IMG_MAX_WINDOW_SIZE);
	max_window = round_down(max_window, PAGE_SIZE);
	if (!max_window)
		return -ENODEV;

	map_offset = round_down(file_offset, max_window);
	map_size = min_t(u64, max_window, PAGE_ALIGN(dev->image_size - map_offset));
	if (!map_size)
		return -EINVAL;

	ret = detach_device_window(dev);
	if (ret)
		return ret;

	attach.share_id = dev->share_id;
	attach.source_rd_addr = dev->source_rd_addr;
	attach.window_offset = 0;
	attach.file_offset = map_offset;
	attach.size = map_size;
	attach.flags = COCO_IMAGE_SHARE_FLAG_RO;
	attach.target_ipa = 0;
	attach.mapped_pages = 0;

	ret = attach_window(&attach);
	if (ret) {
			pr_err_ratelimited("%s: map window failed share=%llu source_rd=0x%llx map_off=0x%llx map_size=0x%llx image_size=0x%llx ret=%d mapped=%llu\n",
					    DRIVER_NAME, dev->share_id,
					    dev->source_rd_addr, map_offset, map_size,
					    dev->image_size, ret, attach.mapped_pages);
			return ret;
	}
	if (attach.mapped_pages != map_size / PAGE_SIZE) {
		struct coco_image_share_detach detach = {
			.window_offset = 0,
			.size = map_size,
		};

			pr_err_ratelimited("%s: short map share=%llu map_off=0x%llx map_size=0x%llx mapped=%llu expected=%llu\n",
					    DRIVER_NAME, dev->share_id, map_offset,
					    map_size, attach.mapped_pages,
					    map_size / PAGE_SIZE);
		detach_window(&detach);
		return -EIO;
	}

	dev->mapped_file_offset = map_offset;
	dev->mapped_size = map_size;
	pr_info_ratelimited("%s: mapped share=%llu source_rd=0x%llx file_off=0x%llx size=0x%llx target=0x%llx\n",
			    DRIVER_NAME, dev->share_id, dev->source_rd_addr,
			    map_offset, map_size, attach.target_ipa);
	return 0;
}

static int copy_mapped_window_to_page(struct coco_img_device *dev, u64 file_offset,
				      struct page *page, unsigned int page_offset,
				      size_t size)
{
	size_t chunk = size;
	u64 window_offset;
	void *dst;

	if (!window_mapping || !dev->mapped_size)
		return -ENODEV;
	if (file_offset >= dev->image_size)
		return -EINVAL;
	if (file_offset < dev->mapped_file_offset ||
	    file_offset >= dev->mapped_file_offset + dev->mapped_size)
		return -EINVAL;

	window_offset = file_offset - dev->mapped_file_offset;
	chunk = min_t(size_t, chunk, dev->image_size - file_offset);
	chunk = min_t(size_t, chunk, dev->mapped_size - window_offset);
	if (!chunk)
		return -EIO;

	dst = kmap_local_page(page);
	memcpy_fromio((char *)dst + page_offset,
		      (const void __iomem *)((const char __iomem *)window_mapping +
					     window_offset),
		      chunk);
	kunmap_local(dst);
	flush_dcache_page(page);

	return 0;
}

static int coco_img_read_bvec(struct coco_img_device *dev, struct bio_vec *bvec,
			      u64 pos)
{
	size_t done = 0;

	while (done < bvec->bv_len) {
		u64 file_offset = pos + done;
		u64 window_remaining;
		size_t chunk;
		int ret;

		ret = map_device_window(dev, file_offset);
		if (ret)
			return ret;

		window_remaining = dev->mapped_file_offset + dev->mapped_size -
				   file_offset;
		chunk = min_t(size_t, bvec->bv_len - done, window_remaining);
		while (chunk) {
			unsigned int dst_offset = bvec->bv_offset + done;
			unsigned int page_offset = offset_in_page(dst_offset);
			struct page *page = bvec->bv_page + (dst_offset >> PAGE_SHIFT);
			size_t page_chunk = min_t(size_t, chunk,
						  PAGE_SIZE - page_offset);

			ret = copy_mapped_window_to_page(dev, file_offset, page,
							 page_offset, page_chunk);
			if (ret)
				return ret;

			file_offset += page_chunk;
			done += page_chunk;
			chunk -= page_chunk;
		}
	}

	return 0;
}

static void coco_img_process_bio(struct coco_img_device *dev, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	u64 pos = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	int ret;

	if (bio_op(bio) != REQ_OP_READ) {
		pr_warn_ratelimited("%s: rejecting non-read bio op=%u sector=%llu\n",
				    DRIVER_NAME, bio_op(bio),
				    (u64)bio->bi_iter.bi_sector);
		bio_io_error(bio);
		return;
	}

	mutex_lock(&window_lock);
	bio_for_each_segment(bvec, bio, iter) {
		if (pos >= dev->image_size) {
			pr_warn_ratelimited("%s: read beyond image share=%llu pos=0x%llx image_size=0x%llx\n",
					    DRIVER_NAME, dev->share_id, pos,
					    dev->image_size);
			mutex_unlock(&window_lock);
			bio_io_error(bio);
			return;
		}

		if (pos + bvec.bv_len > dev->image_size)
			bvec.bv_len = dev->image_size - pos;

		ret = coco_img_read_bvec(dev, &bvec, pos);
		if (ret) {
				pr_err_ratelimited("%s: read failed share=%llu pos=0x%llx len=0x%x image_size=0x%llx mapped_off=0x%llx mapped_size=0x%llx ret=%d\n",
						    DRIVER_NAME, dev->share_id, pos,
						    bvec.bv_len, dev->image_size,
						    dev->mapped_file_offset,
					    dev->mapped_size, ret);
			mutex_unlock(&window_lock);
			bio_io_error(bio);
			return;
		}
		pos += bvec.bv_len;
	}
	mutex_unlock(&window_lock);

	bio_endio(bio);
}

static void coco_img_submit_bio(struct bio *bio)
{
	struct coco_img_device *dev = bio->bi_bdev->bd_disk->private_data;

	pr_notice_once("%s: first bio op=%u sector=%llu size=0x%x logical=%u\n",
		       DRIVER_NAME, bio_op(bio), (u64)bio->bi_iter.bi_sector,
		       bio->bi_iter.bi_size,
		       bdev_logical_block_size(bio->bi_bdev));

	coco_img_process_bio(dev, bio);
}

static const struct block_device_operations coco_img_blk_fops = {
	.owner = THIS_MODULE,
	.submit_bio = coco_img_submit_bio,
};

static int create_block_device(struct coco_image_share_device *req)
{
	struct queue_limits lim = {
		.physical_block_size = PAGE_SIZE,
		.logical_block_size = SECTOR_SIZE,
		.io_min = PAGE_SIZE,
	};
	struct coco_img_device *dev;
	struct gendisk *disk;
	int ret;

	if (!req->share_id || !req->source_rd_addr || !req->image_size)
		return -EINVAL;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->share_id = req->share_id;
	dev->source_rd_addr = req->source_rd_addr;
	dev->image_size = req->image_size;

	if (!window_mapping || !window_size) {
		ret = -ENODEV;
		goto out_free_dev;
	}
	if (window_size < PAGE_SIZE) {
		ret = -ENODEV;
		goto out_free_dev;
	}
	if (min_t(u64, window_size, COCO_IMG_MAX_WINDOW_SIZE) < PAGE_SIZE) {
		ret = -E2BIG;
		goto out_free_dev;
	}
	pr_notice("%s: created lazy window device share=%llu source_rd=0x%llx image_size=0x%llx window=0x%llx max_attach=0x%llx logical=512 physical=4096\n",
		  DRIVER_NAME, dev->share_id, dev->source_rd_addr,
		  dev->image_size, window_size, (u64)COCO_IMG_MAX_WINDOW_SIZE);

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto out_free_dev;
	}

	disk->first_minor = 0;
	disk->flags = GENHD_FL_NO_PART;
	disk->fops = &coco_img_blk_fops;
	disk->private_data = dev;
	strscpy(disk->disk_name, COCO_IMG_BLOCK_NAME, DISK_NAME_LEN);
	set_capacity(disk, DIV_ROUND_UP_ULL(req->image_size, SECTOR_SIZE));
	set_disk_ro(disk, 1);
	dev->disk = disk;

	ret = add_disk(disk);
	if (ret)
		goto out_put_disk;

	mutex_lock(&coco_img_device_lock);
	if (coco_img_dev != NULL) {
		mutex_unlock(&coco_img_device_lock);
		ret = -EBUSY;
		goto out_del_disk;
	}
	coco_img_dev = dev;
	mutex_unlock(&coco_img_device_lock);

	return 0;

out_del_disk:
	del_gendisk(disk);
out_put_disk:
	put_disk(disk);
out_free_dev:
	kfree(dev);
	return ret;
}

static int destroy_block_device(void)
{
	struct coco_img_device *dev;

	mutex_lock(&coco_img_device_lock);
	dev = coco_img_dev;
	if (!dev) {
		mutex_unlock(&coco_img_device_lock);
		return -ENOENT;
	}
	coco_img_dev = NULL;
	mutex_unlock(&coco_img_device_lock);

	del_gendisk(dev->disk);
	put_disk(dev->disk);
	mutex_lock(&window_lock);
	detach_device_window(dev);
	mutex_unlock(&window_lock);
	kfree(dev);
	return 0;
}

static long coco_image_share_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	unsigned long rd_addr = 0;
	unsigned long status;
	int ret;

	switch (cmd) {
	case COCO_IMAGE_SHARE_IOC_GET_RD_ADDR:
		status = rsi_get_rd_addr(&rd_addr);
		ret = rsi_status_to_errno(status);
		if (ret)
			return ret;
		if (put_user((u64)rd_addr, (u64 __user *)uarg))
			return -EFAULT;
		return 0;
	case COCO_IMAGE_SHARE_IOC_GET_WINDOW: {
		struct coco_image_share_window window = {
			.ipa_start = window_ipa_start,
			.size = window_size,
		};

		if (!window_size)
			return -ENODEV;
		if (copy_to_user(uarg, &window, sizeof(window)))
			return -EFAULT;
		return 0;
	}
	case COCO_IMAGE_SHARE_IOC_CREATE_FROM_FILE: {
		struct coco_image_share_create_from_file req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = create_share_from_file(&req);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &req, sizeof(req))) {
			destroy_share(req.share_id);
			return -EFAULT;
		}
		return 0;
	}
	case COCO_IMAGE_SHARE_IOC_ATTACH_WINDOW: {
		struct coco_image_share_attach req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = attach_window(&req);
		if (copy_to_user(uarg, &req, sizeof(req)))
			return -EFAULT;
		return ret;
	}
	case COCO_IMAGE_SHARE_IOC_DETACH_WINDOW: {
		struct coco_image_share_detach req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		ret = detach_window(&req);
		if (copy_to_user(uarg, &req, sizeof(req)))
			return -EFAULT;
		return ret;
	}
	case COCO_IMAGE_SHARE_IOC_DESTROY: {
		struct coco_image_share_destroy req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		return destroy_share(req.share_id);
	}
	case COCO_IMAGE_SHARE_IOC_CREATE_DEVICE: {
		struct coco_image_share_device req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		return create_block_device(&req);
	}
	case COCO_IMAGE_SHARE_IOC_DESTROY_DEVICE:
		return destroy_block_device();
	default:
		return -ENOTTY;
	}
}

static const struct file_operations coco_image_share_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = coco_image_share_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice coco_image_share_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &coco_image_share_fops,
};

static void find_image_share_window(void)
{
	struct device_node *mem_np;
	struct resource mem_res;
	int ret;

	mem_np = of_find_compatible_node(NULL, NULL, "coco,imgshare-window");
	if (!mem_np)
		mem_np = of_find_compatible_node(NULL, NULL, "shared-dma-pool");
	if (!mem_np)
		return;

	ret = of_address_to_resource(mem_np, 0, &mem_res);
	of_node_put(mem_np);
	if (ret)
		return;

	window_ipa_start = mem_res.start;
	window_size = resource_size(&mem_res);
	ret = rsi_set_reserved_memory(window_ipa_start, window_size);
	if (ret)
		pr_warn("%s: failed to set RSI reserved window: %d\n",
			DRIVER_NAME, ret);

	window_mapping = ioremap_cache(window_ipa_start, window_size);
	if (!window_mapping)
		pr_warn("%s: failed to map image-share window\n", DRIVER_NAME);

	pr_info("%s: image-share window ipa=0x%llx size=0x%llx\n",
		DRIVER_NAME, (u64)window_ipa_start, window_size);
}

static int __init coco_image_share_init(void)
{
	find_image_share_window();
	return misc_register(&coco_image_share_miscdev);
}

static void __exit coco_image_share_exit(void)
{
	struct coco_img_share *share, *tmp;

	destroy_block_device();
	misc_deregister(&coco_image_share_miscdev);

	mutex_lock(&shares_lock);
	list_for_each_entry_safe(share, tmp, &shares, node) {
		list_del(&share->node);
		rsi_img_share_destroy(share->share_id);
		free_share(share);
	}
	mutex_unlock(&shares_lock);

	if (window_mapping)
		iounmap(window_mapping);
}

module_init(coco_image_share_init);
module_exit(coco_image_share_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MZH");
MODULE_DESCRIPTION("CoCo image-share RMM prototype driver");
MODULE_VERSION(DRIVER_VERSION);
