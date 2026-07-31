/*
 * (C) Copyright 2019 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */
#include <common.h>
#include <boot_rkimg.h>
#include <image.h>
#include <malloc.h>
#include <sysmem.h>
#include <asm/arch/fit.h>
#include <asm/arch/resource_img.h>

DECLARE_GLOBAL_DATA_PTR;

#define FIT_PLACEHOLDER_ADDR		0xffffff00

/*
 * Must use args '-E -p' for mkimage to generate FIT image, 4K as max assumption.
 */
#define FIT_FDT_MAX_SIZE		SZ_4K

/*
 * DLCVCAM boot-try (anti-brick for "FIT OK but kernel fails to stay up"):
 * Stored on misc at a fixed byte offset past Android BCB (0 / 16KiB) and
 * virtual-AB metadata (32KiB). Userspace clears try_count after multi-user.
 *
 * Layout (little-endian on disk as raw bytes written by blk_dwrite):
 *   magic 'DCBT' (0x54424344 LE), ver, try_count, limit, flags, reserved...
 */
#define DLCVCAM_BOOTTRY_MAGIC		0x54424344u	/* 'DCBT' */
#define DLCVCAM_BOOTTRY_VERSION		1u
#define DLCVCAM_BOOTTRY_OFF_BYTES	(48 * 1024)
#define DLCVCAM_BOOTTRY_DEFAULT_LIMIT	2u
#define DLCVCAM_BOOTTRY_F_STICKY	(1u << 0)

struct dlcvcam_boot_try {
	u32 magic;
	u32 version;
	u32 try_count;
	u32 limit;
	u32 flags;
	u32 reserved[3];
};

/* Read/write a small record at byte_off inside misc (RMW one block). */
static int dlcvcam_misc_rw(int write, u32 byte_off, void *buf, u32 len)
{
	struct blk_desc *dev_desc;
	disk_partition_t part;
	lbaint_t start;
	u32 blksz, blk_off, within;
	void *blkbuf;
	int ret = -1;

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc)
		return -ENODEV;
	if (part_get_info_by_name(dev_desc, PART_MISC, &part) < 0)
		return -ENOENT;

	blksz = dev_desc->blksz;
	if (!blksz || !len || len > blksz)
		return -EINVAL;

	blk_off = byte_off / blksz;
	within = byte_off % blksz;
	if (within + len > blksz)
		return -EINVAL;
	if (blk_off >= part.size)
		return -EINVAL;

	start = part.start + blk_off;
	blkbuf = memalign(ARCH_DMA_MINALIGN, blksz);
	if (!blkbuf)
		return -ENOMEM;

	if (blk_dread(dev_desc, start, 1, blkbuf) != 1)
		goto out;

	if (write) {
		memcpy((u8 *)blkbuf + within, buf, len);
		if (blk_dwrite(dev_desc, start, 1, blkbuf) == 1)
			ret = 0;
	} else {
		memcpy(buf, (u8 *)blkbuf + within, len);
		ret = 0;
	}
out:
	free(blkbuf);
	return ret;
}

static void dlcvcam_boot_try_sanitize(struct dlcvcam_boot_try *t)
{
	if (t->magic != DLCVCAM_BOOTTRY_MAGIC ||
	    t->version != DLCVCAM_BOOTTRY_VERSION ||
	    t->limit == 0 || t->limit > 16) {
		memset(t, 0, sizeof(*t));
		t->magic = DLCVCAM_BOOTTRY_MAGIC;
		t->version = DLCVCAM_BOOTTRY_VERSION;
		t->try_count = 0;
		t->limit = DLCVCAM_BOOTTRY_DEFAULT_LIMIT;
		t->flags = 0;
	}
}

static void dlcvcam_write_bcb_recovery(void)
{
	struct bootloader_message bmsg;
	struct blk_desc *dev_desc;
	disk_partition_t part;
	u32 bcb_offset, cnt;

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc)
		return;
	if (part_get_info_by_name(dev_desc, PART_MISC, &part) < 0)
		return;

#ifdef CONFIG_ANDROID_BOOT_IMAGE
	bcb_offset = android_bcb_msg_sector_offset();
#else
	bcb_offset = BCB_MESSAGE_BLK_OFFSET;
#endif
	memset(&bmsg, 0, sizeof(bmsg));
	strncpy(bmsg.command, "boot-recovery", sizeof(bmsg.command) - 1);
	cnt = DIV_ROUND_UP(sizeof(bmsg), dev_desc->blksz);
	if (blk_dwrite(dev_desc, part.start + bcb_offset, cnt, &bmsg) != cnt)
		FIT_I("boot-try: BCB write failed\n");
}

/*
 * Decide boot vs recovery for this power-cycle. Called once; result cached
 * because fit_get_blob() runs multiple times (kernel/fdt/resource).
 *
 * - Already in recovery boot mode (BCB/key/env): do not touch counter.
 * - Sticky flag or try_count >= limit: force recovery, latch sticky + BCB.
 * - Else: try_count++, attempt boot (FIT-unavailable path may still fall back).
 */
static const char *dlcvcam_select_boot_part(void)
{
	static int decided;
	static const char *part_name = PART_BOOT;
	struct dlcvcam_boot_try t;
	u32 limit;

	if (decided)
		return part_name;
	decided = 1;

#ifndef CONFIG_ANDROID_AB
	if (rockchip_get_boot_mode() == BOOT_MODE_RECOVERY) {
		part_name = PART_RECOVERY;
		FIT_I("boot-try: mode=recovery (bcb/env/key), skip counter\n");
		return part_name;
	}
#endif

	memset(&t, 0, sizeof(t));
	if (dlcvcam_misc_rw(0, DLCVCAM_BOOTTRY_OFF_BYTES, &t, sizeof(t))) {
		FIT_I("boot-try: misc read fail, use boot\n");
		return part_name;
	}
	dlcvcam_boot_try_sanitize(&t);
	limit = t.limit ? t.limit : DLCVCAM_BOOTTRY_DEFAULT_LIMIT;

	if ((t.flags & DLCVCAM_BOOTTRY_F_STICKY) || t.try_count >= limit) {
		FIT_I("boot-try: force recovery (try=%u limit=%u sticky=%u)\n",
		      t.try_count, limit,
		      (t.flags & DLCVCAM_BOOTTRY_F_STICKY) ? 1 : 0);
		t.flags |= DLCVCAM_BOOTTRY_F_STICKY;
		if (t.try_count < limit)
			t.try_count = limit;
		dlcvcam_misc_rw(1, DLCVCAM_BOOTTRY_OFF_BYTES, &t, sizeof(t));
		dlcvcam_write_bcb_recovery();
		part_name = PART_RECOVERY;
		return part_name;
	}

	t.try_count++;
	FIT_I("boot-try: attempt boot try=%u/%u\n", t.try_count, limit);
	if (dlcvcam_misc_rw(1, DLCVCAM_BOOTTRY_OFF_BYTES, &t, sizeof(t)))
		FIT_I("boot-try: misc write fail (continue boot)\n");

	part_name = PART_BOOT;
	return part_name;
}

static int fit_is_ext_type(const void *fit)
{
	return fdt_totalsize(fit) < FIT_FDT_MAX_SIZE;
}

static int fit_is_signed(const void *fit, const void *sig_blob)
{
	return fdt_subnode_offset(sig_blob, 0, FIT_SIG_NODENAME) < 0 ? 0 : 1;
}

static inline int fit_image_addr_is_placeholder(ulong addr)
{
	return (addr & 0xffffff00) == FIT_PLACEHOLDER_ADDR;
}

static int fit_sig_require_conf(const void *fit, const void *sig_blob)
{
	const char *required;
	int sig_node;
	int noffset;

	sig_node = fdt_subnode_offset(sig_blob, 0, FIT_SIG_NODENAME);
	if (sig_node < 0)
		return 0;

	fdt_for_each_subnode(noffset, sig_blob, sig_node) {
		required = fdt_getprop(sig_blob, noffset, "required", NULL);
		if (required && !strcmp(required, "conf"))
			return 1;
	}

	return 0;
}

int fit_default_conf_get_node(const void *fit, const char *prop_name)
{
	int conf_noffset;

	conf_noffset = fit_conf_get_node(fit, NULL); /* NULL for default conf */
	if (conf_noffset < 0)
		return conf_noffset;

	return fit_conf_get_prop_node(fit, conf_noffset, prop_name);
}

int fix_image_set_addr(const void *fit, const char *prop_name,
		       ulong old, ulong new)
{
	int noffset;

	/* do not fix if verified-boot */
	if (!fit_image_addr_is_placeholder(old) ||
	     fit_sig_require_conf(fit, gd_fdt_blob()))
		return 0;

	noffset = fit_default_conf_get_node(fit, prop_name);
	if (noffset < 0)
		return noffset;

	/* optional */
	fit_image_set_entry(fit, noffset, new);

	return fit_image_set_load(fit, noffset, new);
}

static int fdt_image_get_offset_size(const void *fit, const char *prop_name,
				     int *offset, int *size)
{
	int sz, offs;
	int noffset;
	int ret;

	noffset = fit_default_conf_get_node(fit, prop_name);
	if (noffset < 0)
		return noffset;

	ret = fit_image_get_data_size(fit, noffset, &sz);
	if (ret)
		return ret;

	ret = fit_image_get_data_position(fit, noffset, &offs);
	if (!ret)
		offs -= fdt_totalsize(fit);
	else
		ret = fit_image_get_data_offset(fit, noffset, &offs);

	*offset = offs;
	*size = sz;

	return ret;
}

static int fdt_image_get_load(const void *fit, const char *prop_name,
			      ulong *load)
{
	int noffset;

	noffset = fit_default_conf_get_node(fit, prop_name);
	if (noffset < 0)
		return noffset;

	return fit_image_get_load(fit, noffset, load);
}

static int fit_image_get_param(const void *fit, const char *prop_name,
			       ulong *load, int *offset, int *size)
{
	int ret;

	ret = fdt_image_get_offset_size(fit, prop_name, offset, size);
	if (ret < 0)
		return ret;

	return fdt_image_get_load(fit, prop_name, load);
}

static void *fit_get_blob_from_part(struct blk_desc *dev_desc,
				    disk_partition_t *out_part,
				    bool verify,
				    const char *part_name)
{
	__maybe_unused int conf_noffset;
	disk_partition_t part;
	void *fit, *fdt;
	int blk_num;

	if (part_get_info_by_name(dev_desc, part_name, &part) < 0) {
		FIT_I("No %s partition\n", part_name);
		return NULL;
	}

	*out_part = part;
	blk_num = DIV_ROUND_UP(sizeof(struct fdt_header), dev_desc->blksz);
	fdt = memalign(ARCH_DMA_MINALIGN, blk_num * dev_desc->blksz);
	if (!fdt)
		return NULL;

	if (blk_dread(dev_desc, part.start, blk_num, fdt) != blk_num) {
		debug("Failed to read fdt header\n");
		goto fail;
	}

	if (fdt_check_header(fdt)) {
		debug("No fdt header\n");
		goto fail;
	}

	if (!fit_is_ext_type(fdt)) {
		debug("Not external type\n");
		goto fail;
	}

	blk_num = DIV_ROUND_UP(fdt_totalsize(fdt), dev_desc->blksz);
	fit = memalign(ARCH_DMA_MINALIGN, blk_num * dev_desc->blksz);
	if (!fit) {
		debug("No memory\n");
		goto fail;
	}

	if (blk_dread(dev_desc, part.start, blk_num, fit) != blk_num) {
		free(fit);
		debug("Failed to read fit blob\n");
		goto fail;
	}

#ifdef CONFIG_FIT_SIGNATURE
	if (!verify)
		return fit;

	conf_noffset = fit_conf_get_node(fit, NULL); /* NULL for default conf */
	if (conf_noffset < 0)
		goto fail;

	printf("%s: ", fdt_get_name(fit, conf_noffset, NULL));
	if (fit_config_verify(fit, conf_noffset)) {
		puts("\n");
		/* don't remove this failure handle */
		run_command("download", 0);
		hang();
	}
	puts("\n");
#endif
	return fit;

fail:
	free(fdt);
	return NULL;
}

static void *fit_get_blob(struct blk_desc *dev_desc,
			  disk_partition_t *out_part,
			  bool verify)
{
	const char *part_name;
	void *fit;

	/*
	 * Boot-try counter (misc@48KiB) may force recovery when previous
	 * boot attempts did not reach userspace confirm. Still fall back if
	 * the chosen partition has no usable FIT.
	 */
	part_name = dlcvcam_select_boot_part();

	fit = fit_get_blob_from_part(dev_desc, out_part, verify, part_name);
	/*
	 * Anti-brick: if normal boot partition has no usable FIT, fall back to
	 * recovery (factory/golden kernel). Skip when already selecting recovery.
	 */
	if (!fit && strcmp(part_name, PART_RECOVERY) != 0) {
		FIT_I("boot FIT unavailable, try recovery\n");
		fit = fit_get_blob_from_part(dev_desc, out_part, verify,
					     PART_RECOVERY);
	}

	return fit;
}

static int fit_image_fixup_alloc(const void *fit, const char *prop_name,
				 const char *addr_name, enum memblk_id mem)
{
	ulong load, addr;
	int offset, size = 0;
	int ret;

	addr = env_get_ulong(addr_name, 16, 0);
	if (!addr)
		return -EINVAL;

	ret = fit_image_get_param(fit, prop_name, &load, &offset, &size);
	if (ret)
		return (ret == -FDT_ERR_NOTFOUND) ? 0 : ret;

	if (!size)
		return 0;

	ret = fix_image_set_addr(fit, prop_name, load, addr);
	if (ret)
		return ret;

/*
 * 1. When need load HWID dtb, gd->fdt_blob points to HWID dtb
 *    and U-Boot will re-alloc MEM_FDT based on fdt node in
 *    ITB instead of resource. So alloc the larger size to
 *    avoid fail in sysmem. It will already skip load DTB in fdt node.
 *
 * 2. Additionally increase size with CONFIG_SYS_FDT_PAD to reserve
 *    some space for adding more props to dtb afterwards.
 */
	if (!strcmp(prop_name, FIT_FDT_PROP) && !fdt_check_header(gd->fdt_blob))
		size = ((size > fdt_totalsize(gd->fdt_blob)) ?
			 size : fdt_totalsize(gd->fdt_blob)) +
			 CONFIG_SYS_FDT_PAD;

	if (!sysmem_alloc_base(mem, (phys_addr_t)addr,
			       ALIGN(size, RK_BLK_SIZE)))
		return -ENOMEM;

	return 0;
}

int fit_image_pre_process(const void *fit)
{
	int ret;

	/* free for fit_image_fixup_alloc(FIT_FDT_PROP) to re-alloc */
	if ((gd->flags & GD_FLG_KDTB_READY) && !gd->fdt_blob_kern)
		sysmem_free((phys_addr_t)gd->fdt_blob);

	ret = fit_image_fixup_alloc(fit, FIT_FDT_PROP,
				    "fdt_addr_r", MEM_FDT);
	if (ret < 0) {
		return ret;
	}

#if !defined(CONFIG_ARM64) && defined(CONFIG_CMD_BOOTZ)
	int cfg_noffset, noffset;
	const void *buf;
	ulong start, end;
	size_t size;

	cfg_noffset = fit_conf_get_node(fit, NULL);
	if (cfg_noffset < 0) {
		printf("Could not find configuration node\n");
		return -ENOENT;
	}

	noffset = fit_conf_get_prop_node_index(fit, cfg_noffset, FIT_KERNEL_PROP, 0);
	if (noffset < 0) {
		printf("Could not find subimage node\n");
		return -ENOENT;
	}

	/* get image data address and length */
	if (fit_image_get_data(fit, noffset, &buf, &size)) {
		printf("Could not find %s subimage data!\n", FIT_KERNEL_PROP);
		return -ENOENT;
	}

	if (!bootz_setup((ulong)buf, &start, &end))
		ret = fit_image_fixup_alloc(fit, FIT_KERNEL_PROP,
					    "kernel_addr_c", MEM_KERNEL);
	else
#endif
		ret = fit_image_fixup_alloc(fit, FIT_KERNEL_PROP,
					    "kernel_addr_r", MEM_KERNEL);
	if (ret < 0)
		return ret;

	return fit_image_fixup_alloc(fit, FIT_RAMDISK_PROP,
				     "ramdisk_addr_r", MEM_RAMDISK);
}

int fit_image_fail_process(const void *fit)
{
	ulong raddr, kaddr, faddr;

	raddr = env_get_ulong("ramdisk_addr_r", 16, 0);
	kaddr = env_get_ulong("kernel_addr_r", 16, 0);
	faddr = env_get_ulong("fdt_addr_r", 16, 0);

	sysmem_free((phys_addr_t)fit);
	sysmem_free((phys_addr_t)raddr);
	sysmem_free((phys_addr_t)kaddr);
	sysmem_free((phys_addr_t)faddr);

	return 0;
}

static int fit_image_get_subnode(const void *fit, int noffset, const char *name)
{
	int sub_noffset;

	fdt_for_each_subnode(sub_noffset, fit, noffset) {
		if (!strncmp(fit_get_name(fit, sub_noffset, NULL),
			     name, strlen(name)))
			return sub_noffset;
	}

	return -ENOENT;
}

static int fit_image_load_one(const void *fit, struct blk_desc *dev_desc,
			      disk_partition_t *part, char *prop_name,
			      void *data, int check_hash)
{
	u32 blk_num, blk_off;
	int offset, size;
	int noffset, ret;
	char *msg = "";

	ret = fdt_image_get_offset_size(fit, prop_name, &offset, &size);
	if (ret)
		return ret;

	blk_off = (FIT_ALIGN(fdt_totalsize(fit)) + offset) / dev_desc->blksz;
	blk_num = DIV_ROUND_UP(size, dev_desc->blksz);
	if (blk_dread(dev_desc, part->start + blk_off, blk_num, data) != blk_num)
		return -EIO;

	if (check_hash) {
		int hash_noffset;

		noffset = fit_default_conf_get_node(fit, prop_name);
		if (noffset < 0)
			return noffset;

		hash_noffset = fit_image_get_subnode(fit, noffset,
						     FIT_HASH_NODENAME);
		if (hash_noffset < 0)
			return hash_noffset;

		printf("%s: ", fdt_get_name(fit, noffset, NULL));
		ret = fit_image_check_hash(fit, hash_noffset, data, size, &msg);
		if (ret)
			return ret;

		puts("+\n");
	}

	return 0;
}

/* Calculate what we really need */
ulong fit_image_get_bootables_size(const void *fit)
{
	ulong off[3] = { 0, 0, 0 };
	ulong max_off, load;
	int offset, size;

#if 0
	if (!fit_get_totalsize(fit, &size))
		return size;
#endif

	if (!fit_image_get_param(fit, FIT_FDT_PROP, &load, &offset, &size))
		off[0] = offset + FIT_ALIGN(size);

	if (!fit_image_get_param(fit, FIT_KERNEL_PROP, &load, &offset, &size))
		off[1] = offset + FIT_ALIGN(size);

	if (!fit_image_get_param(fit, FIT_RAMDISK_PROP, &load, &offset, &size))
		off[2] = offset + FIT_ALIGN(size);

	max_off = max(off[0],  off[1]);
	max_off = max(max_off, off[2]);

	return FIT_ALIGN(fdt_totalsize(fit)) + max_off;
}

void *fit_image_load_bootables(ulong *size)
{
	struct blk_desc *dev_desc;
	disk_partition_t part;
	int blk_num;
	void *fit;

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc)
		return NULL;

	fit = fit_get_blob(dev_desc, &part, false);
	if (!fit) {
		FIT_I("No fit blob\n");
		return NULL;
	}

	*size = fit_image_get_bootables_size(fit);
	if (*size == 0) {
		FIT_I("No bootable image\n");
		return NULL;
	}

	blk_num = DIV_ROUND_UP(*size, dev_desc->blksz);
	fit = sysmem_alloc(MEM_FIT, blk_num * dev_desc->blksz);
	if (!fit)
		return NULL;

	if (blk_dread(dev_desc, part.start, blk_num, fit) != blk_num) {
		FIT_I("Failed to load bootable images\n");
		return NULL;
	}

	return fit;
}

static void fit_msg(const void *fit)
{
	FIT_I("%ssigned, %sconf required\n",
	      fit_is_signed(fit, gd_fdt_blob()) ? "" : "no ",
	      fit_sig_require_conf(fit, gd_fdt_blob()) ? "" : "no ");
}

#ifdef CONFIG_ROCKCHIP_RESOURCE_IMAGE
ulong fit_image_init_resource(struct blk_desc *dev_desc)
{
	disk_partition_t part;
	void *fit, *buf;
	int offset, size;
	int ret = 0;

	if (!dev_desc)
		return -ENODEV;

	fit = fit_get_blob(dev_desc, &part, true);
	if (!fit)
		return -EAGAIN;

	ret = fdt_image_get_offset_size(fit, FIT_MULTI_PROP, &offset, &size);
	if (ret)
		return -EINVAL;

	buf = memalign(ARCH_DMA_MINALIGN, ALIGN(size, dev_desc->blksz));
	if (!buf)
		return -ENOMEM;

	printf("RESC: '%s', blk@0x%08lx\n", part.name,
	       part.start + ((FIT_ALIGN(fdt_totalsize(fit)) + offset) / dev_desc->blksz));
	ret = fit_image_load_one(fit, dev_desc, &part, FIT_MULTI_PROP, buf, 1);
	if (ret)
		return ret;

	ret = resource_setup_ram_list(dev_desc, buf);
	if (ret) {
		FIT_I("Failed to setup resource ram list, ret=%d\n", ret);
		free(fit);
		return ret;
	}

	fit_msg(fit);
	free(fit);

	return 0;
}
#else
int fit_image_read_dtb(void *fdt_addr)
{
	struct blk_desc *dev_desc;
	disk_partition_t part;
	void *fit;

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc) {
		FIT_I("No dev_desc!\n");
		return -ENODEV;;
	}

	fit = fit_get_blob(dev_desc, &part, true);
	if (!fit)
		return -EINVAL;

	fit_msg(fit);

	return fit_image_load_one(fit, dev_desc, &part, FIT_FDT_PROP,
				  (void *)fdt_addr, 1);
}
#endif
