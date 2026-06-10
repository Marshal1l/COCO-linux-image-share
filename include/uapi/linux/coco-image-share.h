/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_COCO_IMAGE_SHARE_H
#define _UAPI_LINUX_COCO_IMAGE_SHARE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define COCO_IMAGE_SHARE_PATH_MAX 256
#define COCO_IMAGE_SHARE_FLAG_RO 0x1

struct coco_image_share_window {
	__u64 ipa_start;
	__u64 size;
};

struct coco_image_share_create_from_file {
	char path[COCO_IMAGE_SHARE_PATH_MAX];
	__u64 flags;
	__u64 share_id;
	__u64 source_rd_addr;
	__u64 image_size;
	__u64 page_count;
};

struct coco_image_share_attach {
	__u64 share_id;
	__u64 source_rd_addr;
	__u64 window_offset;
	__u64 file_offset;
	__u64 size;
	__u64 flags;
	__u64 target_ipa;
	__u64 mapped_pages;
};

struct coco_image_share_detach {
	__u64 window_offset;
	__u64 size;
	__u64 target_ipa;
};

struct coco_image_share_destroy {
	__u64 share_id;
};

struct coco_image_share_device {
	__u64 share_id;
	__u64 source_rd_addr;
	__u64 image_size;
};

#define COCO_IMAGE_SHARE_IOC_MAGIC 'C'

#define COCO_IMAGE_SHARE_IOC_GET_RD_ADDR \
	_IOR(COCO_IMAGE_SHARE_IOC_MAGIC, 0x01, __u64)
#define COCO_IMAGE_SHARE_IOC_GET_WINDOW \
	_IOR(COCO_IMAGE_SHARE_IOC_MAGIC, 0x02, struct coco_image_share_window)
#define COCO_IMAGE_SHARE_IOC_CREATE_FROM_FILE \
	_IOWR(COCO_IMAGE_SHARE_IOC_MAGIC, 0x03, struct coco_image_share_create_from_file)
#define COCO_IMAGE_SHARE_IOC_ATTACH_WINDOW \
	_IOWR(COCO_IMAGE_SHARE_IOC_MAGIC, 0x04, struct coco_image_share_attach)
#define COCO_IMAGE_SHARE_IOC_DETACH_WINDOW \
	_IOWR(COCO_IMAGE_SHARE_IOC_MAGIC, 0x05, struct coco_image_share_detach)
#define COCO_IMAGE_SHARE_IOC_DESTROY \
	_IOW(COCO_IMAGE_SHARE_IOC_MAGIC, 0x06, struct coco_image_share_destroy)
#define COCO_IMAGE_SHARE_IOC_CREATE_DEVICE \
	_IOW(COCO_IMAGE_SHARE_IOC_MAGIC, 0x07, struct coco_image_share_device)
#define COCO_IMAGE_SHARE_IOC_DESTROY_DEVICE \
	_IO(COCO_IMAGE_SHARE_IOC_MAGIC, 0x08)

#endif /* _UAPI_LINUX_COCO_IMAGE_SHARE_H */
