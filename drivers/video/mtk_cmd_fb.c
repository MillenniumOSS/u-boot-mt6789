/*
 * Copyright (C) 2026 MillenniumOSS
 * SPDX-License-Identifier: GPL-2.0+
 */
#include <asm/cache.h>
#include <asm/io.h>
#include <dm.h>
#include <fdtdec.h>
#include <log.h>
#include <video.h>
#include <linux/bitops.h>

#define DISP_MUTEX_CFG			0x8
#define DISP_MUTEX_EN(m)		(0x20 + 0x20 * (m))
#define DISP_MUTEX_MOD(m)		(0x30 + 0x20 * (m))

#define DISP_DSI_START			0x0
#define DISP_DSI_INTSTA			0xc
#define DISP_DSI_BUSY			BIT(31)

#define DISP_DSI_CMDQ_CON		0x60
#define DISP_DSI_CMDQ_DATA		0xd00
#define DSI_CMDQ_SEND_1_PKT		0x8001
#define DSI_DCS_WRITE_MEM_START_PKT	0x002c3909

#define OVL_L0_PITCH			0x44
#define OVL_L0_PITCH_MASK		0xffff

struct mtk_cmd_fb_conf {
	ulong mutex_base;
	ulong dsi_base;
	ulong ovl0_base;
	ulong ovl0_2l_base;
	u32 ovl_addr_reg;
	u8 max_mutex;
};

struct mtk_cmd_fb_priv {
	void __iomem *mutex;
	void __iomem *dsi;
	u32 mutex_en_reg;
};

static void mtk_cmd_fb_dsi_wait(void __iomem *dsi){
	int i;

	for (i = 0; i < 400000; i++) {
		if (!(readl(dsi + DISP_DSI_INTSTA) & DISP_DSI_BUSY))
			return;
	}
}

static int mtk_cmd_fb_video_sync(struct udevice *dev){
	struct mtk_cmd_fb_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	ulong fb = (ulong)uc_priv->fb;

	flush_dcache_range(fb, ALIGN(fb + uc_priv->fb_size, ARCH_DMA_MINALIGN));

	mtk_cmd_fb_dsi_wait(priv->dsi);

	writel(0, priv->mutex + DISP_MUTEX_CFG);
	writel(1, priv->mutex + priv->mutex_en_reg);

	mtk_cmd_fb_dsi_wait(priv->dsi);
	writel(DSI_DCS_WRITE_MEM_START_PKT, priv->dsi + DISP_DSI_CMDQ_DATA);
	writel(DSI_CMDQ_SEND_1_PKT, priv->dsi + DISP_DSI_CMDQ_CON);

	writel(0, priv->dsi + DISP_DSI_START);
	writel(1, priv->dsi + DISP_DSI_START);

	mtk_cmd_fb_dsi_wait(priv->dsi);

	return 0;
}

static void mtk_cmd_fb_setup_ovl(ulong base, u32 addr_reg, ulong fb_addr,
				 u32 pitch){
	void __iomem *ovl = (void __iomem *)base;

	if (!base)
		return;

	writel(fb_addr, ovl + addr_reg);
	writel((readl(ovl + OVL_L0_PITCH) & ~OVL_L0_PITCH_MASK) |
	       (pitch & OVL_L0_PITCH_MASK), ovl + OVL_L0_PITCH);
}

static int mtk_cmd_fb_probe(struct udevice *dev){
	const struct mtk_cmd_fb_conf *conf = (void *)dev_get_driver_data(dev);
	struct mtk_cmd_fb_priv *priv = dev_get_priv(dev);
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	ofnode node = dev_ofnode(dev);
	const char *format;
	fdt_addr_t base;
	fdt_size_t size;
	u32 width, height, stride;
	int m;

	base = dev_read_addr_size(dev, &size);
	if (base == FDT_ADDR_T_NONE)
		return -EINVAL;

	plat->base = base;
	plat->size = size;

	if (ofnode_read_u32(node, "width", &width) || !width)
		return -EINVAL;

	if (ofnode_read_u32(node, "height", &height) || !height)
		return -EINVAL;

	uc_priv->xsize = width;
	uc_priv->ysize = height;

	if (!ofnode_read_u32(node, "stride", &stride) && stride)
		uc_priv->line_length = stride;

	format = ofnode_read_string(node, "format");
	if (!format)
		return -EINVAL;

	if (!strcmp(format, "a8r8g8b8")) {
		uc_priv->bpix = VIDEO_BPP32;
		uc_priv->format = VIDEO_ARGB8888;
	} else if (!strcmp(format, "x8r8g8b8")) {
		uc_priv->bpix = VIDEO_BPP32;
		uc_priv->format = VIDEO_X8R8G8B8;
	} else {
		return -EINVAL;
	}

	priv->mutex = (void __iomem *)conf->mutex_base;
	priv->dsi = (void __iomem *)conf->dsi_base;

	for (m = 0; m < conf->max_mutex; m++) {
		if (readl(priv->mutex + DISP_MUTEX_MOD(m))) {
			priv->mutex_en_reg = DISP_MUTEX_EN(m);
			break;
		}
	}

	if (!priv->mutex_en_reg) {
		log_err("mtk_cmd_fb: display pipeline is not initialized\n");
		return -ENODEV;
	}

	mtk_cmd_fb_setup_ovl(conf->ovl0_base, conf->ovl_addr_reg,
			     plat->base, uc_priv->line_length);
	mtk_cmd_fb_setup_ovl(conf->ovl0_2l_base, conf->ovl_addr_reg,
			     plat->base, uc_priv->line_length);

	return 0;
}

static const struct mtk_cmd_fb_conf mt6789_fb_conf = {
	.mutex_base	= 0x14001000,
	.dsi_base	= 0x14013000,
	.ovl0_base	= 0x14005000,
	.ovl0_2l_base	= 0x14006000,
	.ovl_addr_reg	= 0xf40,
	.max_mutex	= 2,
};

static const struct udevice_id mtk_cmd_fb_ids[] = {
	{ .compatible = "mediatek,mt6789-cmd-framebuffer", .data = (ulong)&mt6789_fb_conf },
	{ .compatible = "mediatek,mtk-cmd-framebuffer", .data = (ulong)&mt6789_fb_conf },
	{ }
};

static struct video_ops mtk_cmd_fb_ops = {
	.video_sync = mtk_cmd_fb_video_sync,
};

U_BOOT_DRIVER(mtk_cmd_fb) = {
	.name		= "mtk_cmd_fb",
	.id		= UCLASS_VIDEO,
	.of_match	= mtk_cmd_fb_ids,
	.probe		= mtk_cmd_fb_probe,
	.ops		= &mtk_cmd_fb_ops,
	.priv_auto	= sizeof(struct mtk_cmd_fb_priv),
};
