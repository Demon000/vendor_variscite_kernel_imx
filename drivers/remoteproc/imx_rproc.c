// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
 */

#include <dt-bindings/firmware/imx/rsrc.h>
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware/imx/sci.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/workqueue.h>

#include "remoteproc_internal.h"

#define TCML_ADDR_IMX8M		0x7e0000
#define TCML_ADDR_IMX8_CM4_1		0x34fe0000
#define TCML_ADDR_IMX8_CM4_2		0x38fe0000

#define IMX7D_SRC_SCR			0x0C
#define IMX7D_ENABLE_M4			BIT(3)
#define IMX7D_SW_M4P_RST		BIT(2)
#define IMX7D_SW_M4C_RST		BIT(1)
#define IMX7D_SW_M4C_NON_SCLR_RST	BIT(0)

#define IMX7D_M4_RST_MASK		(IMX7D_ENABLE_M4 | IMX7D_SW_M4P_RST \
					 | IMX7D_SW_M4C_RST \
					 | IMX7D_SW_M4C_NON_SCLR_RST)

#define IMX7D_M4_START			(IMX7D_ENABLE_M4 | IMX7D_SW_M4P_RST \
					 | IMX7D_SW_M4C_RST)
#define IMX7D_M4_STOP			(IMX7D_ENABLE_M4 | IMX7D_SW_M4C_RST | \
					 IMX7D_SW_M4C_NON_SCLR_RST)

/* Address: 0x020D8000 */
#define IMX6SX_SRC_SCR			0x00
#define IMX6SX_ENABLE_M4		BIT(22)
#define IMX6SX_SW_M4P_RST		BIT(12)
#define IMX6SX_SW_M4C_NON_SCLR_RST	BIT(4)
#define IMX6SX_SW_M4C_RST		BIT(3)

#define IMX6SX_M4_START			(IMX6SX_ENABLE_M4 | IMX6SX_SW_M4P_RST \
					 | IMX6SX_SW_M4C_RST)
#define IMX6SX_M4_STOP			(IMX6SX_ENABLE_M4 | IMX6SX_SW_M4C_RST | \
					 IMX6SX_SW_M4C_NON_SCLR_RST)
#define IMX6SX_M4_RST_MASK		(IMX6SX_ENABLE_M4 | IMX6SX_SW_M4P_RST \
					 | IMX6SX_SW_M4C_NON_SCLR_RST \
					 | IMX6SX_SW_M4C_RST)

#define IMX_RPROC_MEM_MAX		32

#define IMX_SIP_RPROC			0xC2000005
#define IMX_SIP_RPROC_START		0x00
#define IMX_SIP_RPROC_STARTED		0x01
#define IMX_SIP_RPROC_STOP		0x02

#define REMOTE_IS_READY			BIT(0)
#define REMOTE_READY_WAIT_MAX_RETRIES	500

/**
 * struct imx_rproc_mem - slim internal memory structure
 * @cpu_addr: MPU virtual address of the memory region
 * @sys_addr: Bus address used to access the memory region
 * @size: Size of the memory region
 */
struct imx_rproc_mem {
	void __iomem *cpu_addr;
	phys_addr_t sys_addr;
	size_t size;
};

/* att flags */
/* M4 own area. Can be mapped at probe */
#define ATT_OWN		BIT(31)
#define ATT_IOMEM	BIT(30)
/* I = [0:7] */
#define ATT_CORE_MASK	0xffff
#define ATT_CORE(I)	BIT((I))

/* address translation table */
struct imx_rproc_att {
	u32 da;	/* device address (From Cortex M4 view)*/
	u32 sa;	/* system bus address */
	u32 size; /* size of reg range */
	int flags;
};

/* Remote core start/stop method */
enum imx_rproc_method {
	IMX_RPROC_NONE,
	/* Through syscon regmap */
	IMX_RPROC_MMIO,
	/* Through ARM SMCCC */
	IMX_RPROC_SMC,
	IMX_SCU_API,
};

struct imx_rproc_dcfg {
	u32				src_reg;
	u32				src_mask;
	u32				src_start;
	u32				src_stop;
	const struct imx_rproc_att	*att;
	size_t				att_size;
	enum imx_rproc_method		method;
};

struct imx_rproc {
	struct device			*dev;
	struct regmap			*regmap;
	struct rproc			*rproc;
	const struct imx_rproc_dcfg	*dcfg;
	struct imx_rproc_mem		mem[IMX_RPROC_MEM_MAX];
	struct clk			*clk;
	bool				early_boot;
	bool				ipc_only;
	struct mbox_client		cl;
	struct mbox_client		cl_rxdb;
	struct mbox_client		cl_txdb;
	struct mbox_chan		*tx_ch;
	struct mbox_chan		*rx_ch;
	struct mbox_chan		*rxdb_ch;
	struct mbox_chan		*txdb_ch;
	struct work_struct		rproc_work;
	struct workqueue_struct		*workqueue;
	void __iomem			*rsc_table;
	u32				flags;
	spinlock_t			mu_lock;
	u32				mub_partition;
	struct notifier_block		proc_nb;
	u32				rsrc;
	u32				id;
	int				num_domains;
	struct device			**pm_devices;
	struct device_link		**pm_devices_link;
	u32				startup_delay;
	u32				m_core_ddr_addr;
	u32 				last_load_addr;
	u32				m4_start_addr;
};

static struct imx_sc_ipc *ipc_handle;

static const struct imx_rproc_att imx_rproc_att_imx8qm[] = {
	/* dev addr , sys addr  , size	    , flags */
	{ 0x08000000, 0x08000000, 0x10000000, 0},
	/* TCML */
	{ 0x1FFE0000, 0x34FE0000, 0x00020000, ATT_OWN | ATT_CORE(0)},
	{ 0x1FFE0000, 0x38FE0000, 0x00020000, ATT_OWN | ATT_CORE(1)},
	/* TCMU */
	{ 0x20000000, 0x35000000, 0x00020000, ATT_OWN | ATT_CORE(0)},
	{ 0x20000000, 0x39000000, 0x00020000, ATT_OWN | ATT_CORE(1)},
	/* DDR (Data) */
	{ 0x80000000, 0x80000000, 0x60000000, 0 },
};

static const struct imx_rproc_att imx_rproc_att_imx8qxp[] = {
	/* dev addr , sys addr  , size	    , flags */
	{ 0x08000000, 0x08000000, 0x10000000, 0},
	/* TCML */
	{ 0x1FFE0000, 0x34FE0000, 0x00020000, ATT_OWN | ATT_IOMEM},
	/* TCMU */
	{ 0x20000000, 0x35000000, 0x00020000, ATT_OWN | ATT_IOMEM},
	/* OCRAM(Low 96KB) */
	{ 0x21000000, 0x00100000, 0x00018000, 0},
	/* OCRAM */
	{ 0x21100000, 0x00100000, 0x00040000, 0},
	/* DDR (Data) */
	{ 0x80000000, 0x80000000, 0x60000000, 0 },
};

static const struct imx_rproc_att imx_rproc_att_imx93[] = {
	/* dev addr , sys addr  , size	    , flags */
	/* TCM CODE NON-SECURE */
	{ 0x0FFC0000, 0x201C0000, 0x00040000, ATT_OWN | ATT_IOMEM },

	/* TCM CODE SECURE */
	{ 0x1FFC0000, 0x201C0000, 0x00040000, ATT_OWN | ATT_IOMEM },

	/* TCM SYS NON-SECURE*/
	{ 0x20000000, 0x20200000, 0x00040000, ATT_OWN | ATT_IOMEM },

	/* TCM SYS SECURE*/
	{ 0x30000000, 0x20200000, 0x00040000, ATT_OWN | ATT_IOMEM },

	/* DDR */
	{ 0x80000000, 0x80000000, 0x10000000, 0 },
	{ 0x90000000, 0x80000000, 0x10000000, 0 },

	{ 0xC0000000, 0xc0000000, 0x10000000, 0 },
	{ 0xD0000000, 0xc0000000, 0x10000000, 0 },
};

static const struct imx_rproc_att imx_rproc_att_imx8mn[] = {
	/* dev addr , sys addr  , size	    , flags */
	/* ITCM   */
	{ 0x00000000, 0x007E0000, 0x00020000, ATT_OWN | ATT_IOMEM },
	/* OCRAM_S */
	{ 0x00180000, 0x00180000, 0x00009000, 0 },
	/* OCRAM */
	{ 0x00900000, 0x00900000, 0x00020000, 0 },
	/* OCRAM */
	{ 0x00920000, 0x00920000, 0x00020000, 0 },
	/* OCRAM */
	{ 0x00940000, 0x00940000, 0x00050000, 0 },
	/* QSPI Code - alias */
	{ 0x08000000, 0x08000000, 0x08000000, 0 },
	/* DDR (Code) - alias */
	{ 0x10000000, 0x40000000, 0x0FFE0000, 0 },
	/* DTCM */
	{ 0x20000000, 0x00800000, 0x00020000, ATT_OWN | ATT_IOMEM },
	/* OCRAM_S - alias */
	{ 0x20180000, 0x00180000, 0x00008000, ATT_OWN },
	/* OCRAM */
	{ 0x20200000, 0x00900000, 0x00020000, ATT_OWN },
	/* OCRAM */
	{ 0x20220000, 0x00920000, 0x00020000, ATT_OWN },
	/* OCRAM */
	{ 0x20240000, 0x00940000, 0x00040000, ATT_OWN },
	/* DDR (Data) */
	{ 0x40000000, 0x40000000, 0x80000000, ATT_IOMEM },
};

static const struct imx_rproc_att imx_rproc_att_imx8mq[] = {
	/* dev addr , sys addr  , size	    , flags */
	/* TCML - alias */
	{ 0x00000000, 0x007e0000, 0x00020000, ATT_IOMEM},
	/* OCRAM_S */
	{ 0x00180000, 0x00180000, 0x00008000, 0 },
	/* OCRAM */
	{ 0x00900000, 0x00900000, 0x00020000, 0 },
	/* OCRAM */
	{ 0x00920000, 0x00920000, 0x00020000, 0 },
	/* QSPI Code - alias */
	{ 0x08000000, 0x08000000, 0x08000000, 0 },
	/* DDR (Code) - alias */
	{ 0x10000000, 0x40000000, 0x0FFE0000, 0 },
	/* TCML */
	{ 0x1FFE0000, 0x007E0000, 0x00020000, ATT_OWN  | ATT_IOMEM},
	/* TCML/U */
	{ 0x1FFE0000, 0x007E0000, 0x00040000, ATT_OWN  | ATT_IOMEM},
	/* OCRAM_S */
	{ 0x20180000, 0x00180000, 0x00008000, ATT_OWN },
	/* OCRAM */
	{ 0x20200000, 0x00900000, 0x00020000, ATT_OWN },
	/* OCRAM */
	{ 0x20220000, 0x00920000, 0x00020000, ATT_OWN },
	/* DDR (Data) */
	{ 0x40000000, 0x40000000, 0x80000000, ATT_IOMEM },
};

static const struct imx_rproc_att imx_rproc_att_imx8ulp[] = {
	{0x1FFC0000, 0x1FFC0000, 0xC0000, ATT_OWN},
	{0x21000000, 0x21000000, 0x10000, ATT_OWN},
	{0x80000000, 0x80000000, 0x60000000, 0}
};

static const struct imx_rproc_att imx_rproc_att_imx7ulp[] = {
	{0x1FFD0000, 0x1FFD0000, 0x30000, ATT_OWN},
	{0x20000000, 0x20000000, 0x10000, ATT_OWN},
	{0x2F000000, 0x2F000000, 0x20000, ATT_OWN},
	{0x2F020000, 0x2F020000, 0x20000, ATT_OWN},
	{0x60000000, 0x60000000, 0x40000000, 0}
};

static const struct imx_rproc_att imx_rproc_att_imx7d[] = {
	/* dev addr , sys addr  , size	    , flags */
	/* OCRAM_S (M4 Boot code) - alias */
	{ 0x00000000, 0x00180000, 0x00008000, 0 },
	/* OCRAM_S (Code) */
	{ 0x00180000, 0x00180000, 0x00008000, ATT_OWN },
	/* OCRAM (Code) - alias */
	{ 0x00900000, 0x00900000, 0x00020000, 0 },
	/* OCRAM_EPDC (Code) - alias */
	{ 0x00920000, 0x00920000, 0x00020000, 0 },
	/* OCRAM_PXP (Code) - alias */
	{ 0x00940000, 0x00940000, 0x00008000, 0 },
	/* TCML (Code) */
	{ 0x1FFF8000, 0x007F8000, 0x00008000, ATT_OWN | ATT_IOMEM },
	/* DDR (Code) - alias, first part of DDR (Data) */
	{ 0x10000000, 0x80000000, 0x0FFF0000, 0 },

	/* TCMU (Data) */
	{ 0x20000000, 0x00800000, 0x00008000, ATT_OWN | ATT_IOMEM },
	/* OCRAM (Data) */
	{ 0x20200000, 0x00900000, 0x00020000, 0 },
	/* OCRAM_EPDC (Data) */
	{ 0x20220000, 0x00920000, 0x00020000, 0 },
	/* OCRAM_PXP (Data) */
	{ 0x20240000, 0x00940000, 0x00008000, 0 },
	/* DDR (Data) */
	{ 0x80000000, 0x80000000, 0x60000000, 0 },
};

static const struct imx_rproc_att imx_rproc_att_imx6sx[] = {
	/* dev addr , sys addr  , size	    , flags */
	/* TCML (M4 Boot Code) - alias */
	{ 0x00000000, 0x007F8000, 0x00008000, ATT_IOMEM },
	/* OCRAM_S (Code) */
	{ 0x00180000, 0x008F8000, 0x00004000, 0 },
	/* OCRAM_S (Code) - alias */
	{ 0x00180000, 0x008FC000, 0x00004000, 0 },
	/* TCML (Code) */
	{ 0x1FFF8000, 0x007F8000, 0x00008000, ATT_OWN | ATT_IOMEM },
	/* DDR (Code) - alias, first part of DDR (Data) */
	{ 0x10000000, 0x80000000, 0x0FFF8000, 0 },

	/* TCMU (Data) */
	{ 0x20000000, 0x00800000, 0x00008000, ATT_OWN | ATT_IOMEM },
	/* OCRAM_S (Data) - alias? */
	{ 0x208F8000, 0x008F8000, 0x00004000, 0 },
	/* DDR (Data) */
	{ 0x80000000, 0x80000000, 0x60000000, 0 },
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx8mn = {
	.att		= imx_rproc_att_imx8mn,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx8mn),
	.method		= IMX_RPROC_SMC,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx8mq = {
	.src_reg	= IMX7D_SRC_SCR,
	.src_mask	= IMX7D_M4_RST_MASK,
	.src_start	= IMX7D_M4_START,
	.src_stop	= IMX7D_M4_STOP,
	.att		= imx_rproc_att_imx8mq,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx8mq),
	.method		= IMX_RPROC_MMIO,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx8ulp = {
	.att		= imx_rproc_att_imx8ulp,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx8ulp),
	.method		= IMX_RPROC_NONE,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx7ulp = {
	.att		= imx_rproc_att_imx7ulp,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx7ulp),
	.method		= IMX_RPROC_NONE,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx7d = {
	.src_reg	= IMX7D_SRC_SCR,
	.src_mask	= IMX7D_M4_RST_MASK,
	.src_start	= IMX7D_M4_START,
	.src_stop	= IMX7D_M4_STOP,
	.att		= imx_rproc_att_imx7d,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx7d),
	.method		= IMX_RPROC_MMIO,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx6sx = {
	.src_reg	= IMX6SX_SRC_SCR,
	.src_mask	= IMX6SX_M4_RST_MASK,
	.src_start	= IMX6SX_M4_START,
	.src_stop	= IMX6SX_M4_STOP,
	.att		= imx_rproc_att_imx6sx,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx6sx),
	.method		= IMX_RPROC_MMIO,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx8qxp = {
	.att		= imx_rproc_att_imx8qxp,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx8qxp),
	.method		= IMX_SCU_API,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx8qm = {
	.att		= imx_rproc_att_imx8qm,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx8qm),
	.method		= IMX_SCU_API,
};

static const struct imx_rproc_dcfg imx_rproc_cfg_imx93 = {
	.att		= imx_rproc_att_imx93,
	.att_size	= ARRAY_SIZE(imx_rproc_att_imx93),
	.method		= IMX_RPROC_SMC,
};

static bool imx_rproc_is_imx93(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;

	if (dcfg == &imx_rproc_cfg_imx93)
		return true;
	else
		return false;
}

static int imx_rproc_ready(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	int i;

	if (!priv->rxdb_ch)
		return 0;

	for (i = 0; i < REMOTE_READY_WAIT_MAX_RETRIES; i++) {
		if (priv->flags & REMOTE_IS_READY)
			return 0;
		udelay(100);
	}

	/* Not return -ETIMEOUT, remote processor might not implement doorbell */
	return 0;
}

static int imx_rproc_rebuild_channels(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	struct mbox_client *cl = &priv->cl;
	struct device *dev = priv->dev;
	int ret = 0;

	if (!priv->tx_ch) {
		priv->tx_ch = mbox_request_channel_byname(cl, "tx");
		if (IS_ERR(priv->tx_ch)) {
			ret = PTR_ERR(priv->tx_ch);
			dev_err(dev, "failed to restart tx chan %d\n", ret);
			priv->tx_ch = NULL;

			goto err_exit;
		}
	}

	if (!priv->rx_ch) {
		priv->rx_ch = mbox_request_channel_byname(cl, "rx");
		if (IS_ERR(priv->rx_ch)) {
			ret = PTR_ERR(priv->rx_ch);
			dev_err(dev, "failed to restart rx chan %d\n", ret);
			priv->rx_ch = NULL;

			goto err_exit;
		}
	}

	if (!priv->rxdb_ch) {
		priv->rxdb_ch = mbox_request_channel_byname(cl, "rxdb");
		if (IS_ERR(priv->rxdb_ch)) {
			ret = PTR_ERR(priv->rxdb_ch);
			dev_err(dev, "failed to restart rxdb chan %d\n", ret);
			priv->rxdb_ch = NULL;

			goto err_exit;
		}
	}

	/* txdb is optional */
	if (!priv->txdb_ch) {
		priv->txdb_ch = mbox_request_channel_byname(cl, "txdb");
		if (IS_ERR(priv->txdb_ch))
			priv->txdb_ch = NULL;
	}

err_exit:
	return ret;
}

static void imx_rproc_free_channels(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	__u32 mmsg;

	if (priv->txdb_ch)
		mbox_send_message(priv->txdb_ch, (void *)&mmsg);

	mbox_free_channel(priv->tx_ch);
	mbox_free_channel(priv->rx_ch);
	mbox_free_channel(priv->rxdb_ch);
	mbox_free_channel(priv->txdb_ch);

	priv->tx_ch = NULL;
	priv->rx_ch = NULL;
	priv->rxdb_ch = NULL;
	priv->txdb_ch = NULL;
}

/*
 Per linkerfile: https://github.com/varigit/freertos-variscite/blob/mcuxpresso_sdk_2.8.x-var01/boards/som_mx8qm/demo_apps/hello_world/cm4_core0/armgcc/MIMX8QM6xxxFF_cm4_core0_ddr_ram.ld#L31
 "M4 always start up from TCM. The SCU will copy the first 32 bytes of the binary to TCM
 if the start address is not TCM. The TCM region [0x1FFE0000-0x1FFE001F] is reserved for this purpose."
 Therefore:
 For imx8q and imx8x, it is not necessary to copy the stack pointer and reset vector from ddr to tcm.
 Instead, determine if firmware was loaded into TCM or DDR and provide correct start address to SCU.
 Like 8M family, DDR4 address is defined in device tree node m4_reserved, m7_reserved, or mcore_reserved
*/
static void imx8_set_start_addr(struct rproc *rproc, u32 addr_tcm) {
	struct imx_rproc *priv = rproc->priv;

	if(priv->m_core_ddr_addr && priv->last_load_addr >= priv->m_core_ddr_addr) {
		priv->m4_start_addr = priv->m_core_ddr_addr;
		dev_info(priv->dev, "Setting Cortex M4 start address to DDR 0x%08x\n", priv->m4_start_addr);
	} else {
		priv->m4_start_addr = addr_tcm;
		dev_info(priv->dev, "Setting Cortex M4 start address to TCM 0x%08x\n", priv->m4_start_addr);
	}
}

/*
 Stack pointer and reset vector must be initialized
 See: https://www.nxp.com/docs/en/application-note/AN5317.pdf
 https://github.com/varigit/uboot-imx/blob/imx_v2020.04_5.4.24_2.1.0_var02/arch/arm/mach-imx/imx_bootaux.c#L115
 https://github.com/varigit/uboot-imx/blob/imx_v2020.04_5.4.24_2.1.0_var02/arch/arm/include/asm/arch-imx8m/imx-regs-imx8mm.h#L17
*/
static void imx_8m_setup_stack(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	void __iomem *io_tcml = ioremap(TCML_ADDR_IMX8M, 8);

	//initialize tcml stack pointer and reset vector
	if(priv->m_core_ddr_addr && priv->last_load_addr >= priv->m_core_ddr_addr) {
		void __iomem *io_ddr = ioremap(priv->m_core_ddr_addr, 8);
		dev_info(priv->dev, "Setting up stack pointer and reset vector from firmware in DDR\n");
		writel(readl(io_ddr), io_tcml);
		writel(readl(io_ddr + 4), io_tcml + 4);
		iounmap(io_ddr);
	} else {
		dev_info(priv->dev, "Setting up stack pointer and reset vector from firmware in TCML\n");
		writel(readl(io_tcml), io_tcml);
		writel(readl(io_tcml + 4), io_tcml + 4);
	}

	dev_info(priv->dev, "Stack: 0x%x\n", readl(io_tcml));
	dev_info(priv->dev, "Reset Vector: 0x%x\n", readl(io_tcml + 4));

	iounmap(io_tcml);
}


static int imx_rproc_start(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	struct arm_smccc_res res;
	int ret;

	switch (dcfg->method) {
	case IMX_RPROC_MMIO:
		imx_8m_setup_stack(rproc);
		ret = regmap_update_bits(priv->regmap, dcfg->src_reg, dcfg->src_mask,
					 dcfg->src_start);
		break;
	case IMX_RPROC_SMC:
		if (!imx_rproc_is_imx93(rproc))
			imx_8m_setup_stack(rproc);
		arm_smccc_smc(IMX_SIP_RPROC, IMX_SIP_RPROC_START, 0, 0, 0, 0, 0, 0, &res);
		ret = res.a0;
		break;
	case IMX_SCU_API:
		if (priv->ipc_only) {
			if (rproc->table_ptr == NULL)
				rproc->table_ptr = kmemdup(priv->rsc_table, SZ_1K, GFP_KERNEL);
			ret = imx_rproc_rebuild_channels(rproc);
			if (ret < 0)
				return -EINVAL;
			return imx_rproc_ready(rproc);
		}

		if (priv->id == 1) {
			imx8_set_start_addr(rproc, TCML_ADDR_IMX8_CM4_2);
			ret = imx_sc_pm_cpu_start(ipc_handle, priv->rsrc, true, priv->m4_start_addr);
		} else if (!priv->id) {
			imx8_set_start_addr(rproc, TCML_ADDR_IMX8_CM4_1);
			ret = imx_sc_pm_cpu_start(ipc_handle, priv->rsrc, true, priv->m4_start_addr);
		} else {
			ret = -EINVAL;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (ret)
		dev_err(dev, "Failed to enable remote core!\n");
	else
		ret = imx_rproc_ready(rproc);

	if (priv->startup_delay)
		msleep(priv->startup_delay);

	return ret;
}

static int imx_rproc_stop(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	struct arm_smccc_res res;
	int ret = 0;
	__u32 mmsg;


	if (rproc->state == RPROC_CRASHED && priv->ipc_only) {
		imx_rproc_free_channels(rproc);

		priv->flags &= ~REMOTE_IS_READY;
		return 0;
	}

	if (priv->txdb_ch) {
		ret = mbox_send_message(priv->txdb_ch, (void *)&mmsg);
		if (ret) {
			dev_err(dev, "txdb send fail: %d\n", ret);
			return ret;
		}
	}

	switch (dcfg->method) {
	case IMX_RPROC_MMIO:
		ret = regmap_update_bits(priv->regmap, dcfg->src_reg, dcfg->src_mask,
					 dcfg->src_stop);
		break;
	case IMX_RPROC_SMC:
		arm_smccc_smc(IMX_SIP_RPROC, IMX_SIP_RPROC_STOP, 0, 0, 0, 0, 0, 0, &res);
		ret = res.a0;
		if (res.a1)
			dev_info(dev, "Not in wfi, force stopped\n");
		break;
	case IMX_SCU_API:
		if (priv->id == 1)
			ret = imx_sc_pm_cpu_start(ipc_handle, priv->rsrc, false, priv->m4_start_addr);
		else if (!priv->id)
			ret = imx_sc_pm_cpu_start(ipc_handle, priv->rsrc, false, priv->m4_start_addr);
		else
			ret = -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (ret) {
		dev_err(dev, "Failed to stop remote core\n");
	} else {
		priv->early_boot = false;
		priv->flags &= ~REMOTE_IS_READY;
	}

	return ret;
}

static int imx_rproc_da_to_sys(struct imx_rproc *priv, u64 da,
			       size_t len, u64 *sys, bool *is_iomem)
{
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	int i;

	/* parse address translation table */
	for (i = 0; i < dcfg->att_size; i++) {
		const struct imx_rproc_att *att = &dcfg->att[i];

		if (att->flags & ATT_CORE_MASK) {
			if (!((1 << priv->id) & (att->flags & ATT_CORE_MASK)))
				continue;
		}

		if (da >= att->da && da + len < att->da + att->size) {
			unsigned int offset = da - att->da;

			*sys = att->sa + offset;
			if (is_iomem)
				*is_iomem = att->flags & ATT_IOMEM;
			return 0;
		}
	}

	dev_warn(priv->dev, "Translation failed: da = 0x%llx len = 0x%zx\n",
		 da, len);
	return -ENOENT;
}

static void *imx_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct imx_rproc *priv = rproc->priv;
	void *va = NULL;
	u64 sys;
	int i;

	if (len == 0)
		return NULL;

	/*
	 * On device side we have many aliases, so we need to convert device
	 * address (M4) to system bus address first.
	 */
	if (imx_rproc_da_to_sys(priv, da, len, &sys, is_iomem))
		return NULL;

	for (i = 0; i < IMX_RPROC_MEM_MAX; i++) {
		if (sys >= priv->mem[i].sys_addr && sys + len <
		    priv->mem[i].sys_addr +  priv->mem[i].size) {
			unsigned int offset = sys - priv->mem[i].sys_addr;
			/* __force to make sparse happy with type conversion */
			va = (__force void *)(priv->mem[i].cpu_addr + offset);
			break;
		}
	}

	dev_dbg(&rproc->dev, "da = 0x%llx len = 0x%zx va = 0x%p\n",
		da, len, va);

	return va;
}

static int imx_rproc_mem_alloc(struct rproc *rproc,
			       struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	dev_dbg(dev, "map memory: %p+%zx\n", &mem->dma, mem->len);
	va = ioremap_wc(mem->dma, mem->len);
	if (IS_ERR_OR_NULL(va)) {
		dev_err(dev, "Unable to map memory region: %p+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

static int imx_rproc_mem_release(struct rproc *rproc,
				 struct rproc_mem_entry *mem)
{
	dev_dbg(rproc->dev.parent, "unmap memory: %pa\n", &mem->dma);
	iounmap(mem->va);

	return 0;
}

static int imx_rproc_prepare(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	struct device_node *np = priv->dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	u32 da;

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		/*
		 * Ignore the first memory region which will be used vdev buffer.
		 * No need to do extra handlings, rproc_add_virtio_dev will handle it.
		 */
		if (!strcmp(it.node->name, "vdev0buffer"))
			continue;

		if (!strcmp(it.node->name, "rsc-table"))
			continue;

		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(priv->dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		/* No need to translate pa to da, i.MX use same map */
		da = rmem->base;

		if (!strncmp(it.node->name, "rsc_table", strlen("rsc_table"))) {
			continue;
		}

		/* Register memory region */
		mem = rproc_mem_entry_init(priv->dev, NULL, (dma_addr_t)rmem->base, rmem->size, da,
					   imx_rproc_mem_alloc, imx_rproc_mem_release,
					   it.node->name);

		if (mem)
			rproc_coredump_add_segment(rproc, da, rmem->size);
		else
			return -ENOMEM;

		//get m4/m7 ddr address from device tree
		if(0 == strcmp(it.node->name, "m4") || 0 == strcmp(it.node->name, "m7")
			|| 0 == strcmp(it.node->name, "m_core")) {
			priv->m_core_ddr_addr = rmem->base;
			dev_info(priv->dev, "%s ddr @ 0x%x\n", it.node->name, (u32) rmem->base);
		}

		rproc_add_carveout(rproc, mem);
	}

	return  0;
}

static int imx_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret)
		dev_info(&rproc->dev, "No resource table in elf\n");

	return 0;
}

static void imx_rproc_rxdb_callback(struct mbox_client *cl, void *msg)
{
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	struct imx_rproc *priv = rproc->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->mu_lock, flags);
	priv->flags |= REMOTE_IS_READY;
	spin_unlock_irqrestore(&priv->mu_lock, flags);
}

static u64 imx_rproc_elf_get_boot_addr(struct rproc *rproc,
				       const struct firmware *fw)
{
	struct imx_rproc *priv = rproc->priv;


	if (!priv->early_boot) {
		//save the location of the last firmware load for start function
		priv->last_load_addr = rproc_elf_get_boot_addr(rproc, fw);
		return (u64)priv->last_load_addr;
	}

	return 0;
}

static void imx_rproc_kick(struct rproc *rproc, int vqid)
{
	struct imx_rproc *priv = rproc->priv;
	int err;
	__u32 mmsg;

	if (!priv->tx_ch) {
		dev_err(priv->dev, "No initialized mbox tx channel\n");
		return;
	}

	/*
	 * Send the index of the triggered virtqueue as the mu payload.
	 * Let remote processor know which virtqueue is used.
	 */
	mmsg = vqid << 16;

	err = mbox_send_message(priv->tx_ch, (void *)&mmsg);
	if (err < 0)
		dev_err(priv->dev, "%s: failed (%d, err:%d)\n",
			__func__, vqid, err);
}

static int imx_rproc_attach(struct rproc *rproc)
{
	return 0;
}

static struct resource_table *imx_rproc_get_loaded_rsc_table(struct rproc *rproc, size_t *table_sz)
{
	struct imx_rproc *priv = rproc->priv;

	/* The resource table has already been mapped in imx_rproc_addr_init */
	if (!priv->rsc_table)
		return NULL;

	rproc->table_ptr = (struct resource_table *)priv->rsc_table;
	rproc->table_sz = SZ_1K;
	rproc->cached_table = NULL;

	return 0;
}

static int imx_rproc_elf_load_segments(struct rproc *rproc,
					const struct firmware *fw)
{
	struct imx_rproc *priv = rproc->priv;

	if (priv->ipc_only)
		return 0;

	if (!fw)
		return -EINVAL;

	return rproc_elf_load_segments(rproc, fw);
}

static struct resource_table *
imx_rproc_elf_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	struct imx_rproc *priv = rproc->priv;


	if (priv->ipc_only)
		return NULL;

	if (priv->rsc_table)
		return priv->rsc_table;

	return rproc_elf_find_loaded_rsc_table(rproc, fw);
}

static const struct rproc_ops imx_rproc_ops = {
	.prepare	= imx_rproc_prepare,
	.attach		= imx_rproc_attach,
	.start		= imx_rproc_start,
	.stop		= imx_rproc_stop,
	.kick		= imx_rproc_kick,
	.da_to_va       = imx_rproc_da_to_va,
	.load		= imx_rproc_elf_load_segments,
	.parse_fw	= imx_rproc_parse_fw,
	.find_loaded_rsc_table = imx_rproc_elf_find_loaded_rsc_table,
	.get_loaded_rsc_table = imx_rproc_get_loaded_rsc_table,
	.sanity_check	= rproc_elf_sanity_check,
	.get_boot_addr	= imx_rproc_elf_get_boot_addr,
};

static int imx_rproc_addr_init(struct imx_rproc *priv,
			       struct platform_device *pdev)
{
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int a, b = 0, err, nph;

	/* remap required addresses */
	for (a = 0; a < dcfg->att_size; a++) {
		const struct imx_rproc_att *att = &dcfg->att[a];

		if (!(att->flags & ATT_OWN))
			continue;

		if (att->flags & ATT_CORE_MASK) {
			if (!((1 << priv->id) & (att->flags & ATT_CORE_MASK)))
				continue;
		}

		if (b >= IMX_RPROC_MEM_MAX)
			break;

		if (att->flags & ATT_IOMEM)
			priv->mem[b].cpu_addr = devm_ioremap(&pdev->dev,
							     att->sa, att->size);
		else
			priv->mem[b].cpu_addr = devm_ioremap_wc(&pdev->dev,
								att->sa, att->size);
		if (!priv->mem[b].cpu_addr) {
			dev_err(dev, "failed to remap %#x bytes from %#x\n", att->size, att->sa);
			return -ENOMEM;
		}
		priv->mem[b].sys_addr = att->sa;
		priv->mem[b].size = att->size;
		b++;
	}

	/* memory-region is optional property */
	nph = of_count_phandle_with_args(np, "memory-region", NULL);
	if (nph <= 0)
		return 0;

	/* remap optional addresses */
	for (a = 0; a < nph; a++) {
		struct device_node *node;
		struct resource res;

		node = of_parse_phandle(np, "memory-region", a);
		/* Not map vdevbuffer, vdevring region */
		if (!strncmp(node->name, "vdev", strlen("vdev"))) {
			of_node_put(node);
			continue;
		}
		err = of_address_to_resource(node, 0, &res);
		of_node_put(node);
		if (err) {
			dev_err(dev, "unable to resolve memory region\n");
			return err;
		}

		if (b >= IMX_RPROC_MEM_MAX)
			break;

		/* Not use resource version, because we might share region */
		priv->mem[b].cpu_addr = devm_ioremap_wc(&pdev->dev, res.start, resource_size(&res));
		if (!priv->mem[b].cpu_addr) {
			dev_err(dev, "failed to remap %pr\n", &res);
			return -ENOMEM;
		}
		priv->mem[b].sys_addr = res.start;
		priv->mem[b].size = resource_size(&res);
		if (!strncmp(node->name, "rsc_table", strlen("rsc_table")))
			priv->rsc_table = priv->mem[b].cpu_addr;
		b++;
	}

	return 0;
}

static void imx_rproc_vq_work(struct work_struct *work)
{
	struct imx_rproc *priv = container_of(work, struct imx_rproc,
					      rproc_work);

	rproc_vq_interrupt(priv->rproc, 0);
	rproc_vq_interrupt(priv->rproc, 1);
	rproc_vq_interrupt(priv->rproc, 2);
	rproc_vq_interrupt(priv->rproc, 3);
}

static void imx_rproc_rx_callback(struct mbox_client *cl, void *msg)
{
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	struct imx_rproc *priv = rproc->priv;

	queue_work(priv->workqueue, &priv->rproc_work);
}

static int imx_rproc_xtr_mbox_init(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	struct mbox_client *cl;
	int ret;

	if (!of_get_property(dev->of_node, "mbox-names", NULL))
		return 0;

	spin_lock_init(&priv->mu_lock);

	cl = &priv->cl;
	cl->dev = dev;
	cl->tx_block = true;
	cl->tx_tout = 100;
	cl->knows_txdone = false;
	cl->rx_callback = imx_rproc_rx_callback;

	priv->tx_ch = mbox_request_channel_byname(cl, "tx");
	if (IS_ERR(priv->tx_ch)) {
		ret = PTR_ERR(priv->tx_ch);
		dev_dbg(cl->dev, "failed to request tx mailbox channel: %d\n",
			ret);
		goto err_out;
	}

	priv->rx_ch = mbox_request_channel_byname(cl, "rx");
	if (IS_ERR(priv->rx_ch)) {
		ret = PTR_ERR(priv->rx_ch);
		dev_dbg(cl->dev, "failed to request rx mailbox channel: %d\n",
			ret);
		goto err_out;
	}

	if (dcfg->method != IMX_SCU_API)
		return 0;

	cl = &priv->cl_rxdb;
	cl->dev = dev;
	cl->rx_callback = imx_rproc_rxdb_callback;

	/*
	 * RX door bell is used to receive the ready signal from remote
	 * after the partition reset of A core.
	 */
	priv->rxdb_ch = mbox_request_channel_byname(cl, "rxdb");
	if (IS_ERR(priv->rxdb_ch)) {
	        ret = PTR_ERR(priv->rxdb_ch);
		dev_dbg(cl->dev, "failed to request mbox chan rxdb, ret %d\n",
			ret);
		goto err_out;
	}

	cl = &priv->cl_txdb;
	cl->dev = dev;
	cl->tx_block = true;
	cl->tx_tout = 20;
	cl->knows_txdone = false;

	/* txdb is optional */
	priv->txdb_ch = mbox_request_channel_byname(cl, "txdb");
	if (IS_ERR(priv->txdb_ch)) {
	        ret = PTR_ERR(priv->txdb_ch);
		dev_info(cl->dev, "No txdb, ret %d\n", ret);
		priv->txdb_ch = NULL;
	}

	return 0;

err_out:
	if (!IS_ERR(priv->tx_ch))
		mbox_free_channel(priv->tx_ch);
	if (!IS_ERR(priv->rx_ch))
		mbox_free_channel(priv->rx_ch);
	if (!IS_ERR(priv->rxdb_ch))
	        mbox_free_channel(priv->rxdb_ch);

	return ret;
}

static void imx_rproc_free_mbox(struct rproc *rproc)
{
	struct imx_rproc *priv = rproc->priv;

	mbox_free_channel(priv->tx_ch);
	mbox_free_channel(priv->rx_ch);
}

static int imx_rproc_partition_notify(struct notifier_block *nb,
		                                     unsigned long event, void *group)
{
	struct imx_rproc *priv = container_of(nb, struct imx_rproc, proc_nb);

	/* Ignore other irqs */
	if (!((event & BIT(priv->mub_partition)) && (*(u8 *)group == 5)))
		return 0;

	rproc_report_crash(priv->rproc, RPROC_WATCHDOG);

	pr_info("Patition%d reset!\n", priv->mub_partition);

	return 0;
}

static int imx_rproc_parse_dt(struct device *dev, struct imx_rproc *priv)
{
	int ret = 0;

	if (priv->dcfg->method == IMX_SCU_API) {
		ret = of_property_read_u32(dev->of_node, "core-index", &priv->id);
		if (ret)
			dev_err(dev, "No reg <core index id>\n");
	}

	return ret;
}

static int imx_rproc_detect_mode(struct imx_rproc *priv)
{
	struct regmap_config config = { .name = "imx-rproc" };
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	struct regmap *regmap;
	struct arm_smccc_res res;
	int ret;
	u32 val;
	int i;

	switch (dcfg->method) {
	case IMX_RPROC_NONE:
		priv->rproc->state = RPROC_DETACHED;
		priv->early_boot = true;
		return 0;
	case IMX_RPROC_SMC:
		arm_smccc_smc(IMX_SIP_RPROC, IMX_SIP_RPROC_STARTED, 0, 0, 0, 0, 0, 0, &res);
		if (res.a0)
			priv->rproc->state = RPROC_DETACHED;
		priv->early_boot = !!res.a0;
		return 0;
	case IMX_SCU_API:
		ret = imx_scu_get_handle(&ipc_handle);
		if (ret)
			return ret;
		ret = of_property_read_u32(dev->of_node, "core-id", &priv->rsrc);
		if (ret) {
			dev_err(dev, "No reg <core resource id>\n");
			return ret;
		}
		priv->proc_nb.notifier_call = imx_rproc_partition_notify;


		priv->num_domains = of_count_phandle_with_args(dev->of_node, "power-domains",
							       "#power-domain-cells");
		if (priv->num_domains < 0)
			priv->num_domains = 0;

		if (priv->num_domains) {
			priv->pm_devices = devm_kcalloc(dev, priv->num_domains,
							sizeof(*priv->pm_devices), GFP_KERNEL);
			if (!priv->pm_devices)
				return -ENOMEM;
			priv->pm_devices_link = devm_kcalloc(dev, priv->num_domains,
							     sizeof(*priv->pm_devices_link),
							     GFP_KERNEL);
			if (!priv->pm_devices)
				return -ENOMEM;

			for (i = 0; i < priv->num_domains; i++) {
				priv->pm_devices[i] = genpd_dev_pm_attach_by_id(dev, i);
				if (IS_ERR(priv->pm_devices[i]))
					goto err_put_pd;
				priv->pm_devices_link[i] = device_link_add(dev, priv->pm_devices[i],
									   DL_FLAG_RPM_ACTIVE |
									   DL_FLAG_PM_RUNTIME |
									   DL_FLAG_STATELESS);
				if (IS_ERR(priv->pm_devices_link[i]))
					goto err_put_pd;
			}
		}
		if (!imx_sc_rm_is_resource_owned(ipc_handle, priv->rsrc)) {
			priv->ipc_only = true;
			priv->early_boot = true;
		#ifndef CONFIG_IMX_GKI_FIX
			priv->rproc->skip_fw_recovery = true;
		#endif
			priv->rproc->state = RPROC_DETACHED;
			/*
			 * Get muB partition id and enable irq in SCFW
			 * default partition 3
			 */
			if (of_property_read_u32(dev->of_node, "mub-partition",
						 &priv->mub_partition))
				priv->mub_partition = 3;

			ret = imx_scu_irq_group_enable(IMX_SC_IRQ_GROUP_REBOOTED,
					BIT(priv->mub_partition), true);
			if (ret) {
				dev_warn(dev, "Enable irq failed.\n");
				goto err_put_pd;
			}

			ret = imx_scu_irq_register_notifier(&priv->proc_nb);
			if (ret) {
				imx_scu_irq_group_enable(IMX_SC_IRQ_GROUP_REBOOTED,
					BIT(priv->mub_partition), false);
				dev_warn(dev, "reqister scu notifier failed.\n");
				goto err_put_pd;
			}
		}
		return 0;
	default:
		break;
	}

	regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "syscon");
	if (IS_ERR(regmap)) {
		dev_err(dev, "failed to find syscon\n");
		return PTR_ERR(regmap);
	}

	priv->regmap = regmap;
	regmap_attach_dev(dev, regmap, &config);

	ret = regmap_read(regmap, dcfg->src_reg, &val);
	if (ret) {
		dev_err(dev, "Failed to read src\n");
		return ret;
	}

	if ((val & dcfg->src_mask) != dcfg->src_stop) {
		priv->rproc->state = RPROC_DETACHED;
		priv->early_boot = true;
	}

	return 0;

err_put_pd:
	for (i = 0; i < priv->num_domains; i++) {
		if (priv->pm_devices_link[i])
			device_link_del(priv->pm_devices_link[i]);
		if (priv->pm_devices[i])
			dev_pm_domain_detach(priv->pm_devices[i], true);
	}
	return ret;
}

static int imx_rproc_clk_enable(struct imx_rproc *priv)
{
	const struct imx_rproc_dcfg *dcfg = priv->dcfg;
	struct device *dev = priv->dev;
	int ret;

	/* Remote core is not under control of Linux */
	if (dcfg->method == IMX_RPROC_NONE || dcfg->method == IMX_SCU_API)
		return 0;

	priv->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "Failed to get clock\n");
		return PTR_ERR(priv->clk);
	}

	/*
	 * clk for M4 block including memory. Should be
	 * enabled before .start for FW transfer.
	 */
	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		return ret;
	}

	return 0;
}

static int imx_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_rproc *priv;
	struct rproc *rproc;
	const struct imx_rproc_dcfg *dcfg;
	int ret;

	/* set some other name then imx */
	rproc = rproc_alloc(dev, "imx-rproc", &imx_rproc_ops,
			    NULL, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	dcfg = of_device_get_match_data(dev);
	if (!dcfg) {
		ret = -EINVAL;
		goto err_put_rproc;
	}

	priv = rproc->priv;
	priv->rproc = rproc;
	priv->dcfg = dcfg;
	priv->dev = dev;
	priv->m_core_ddr_addr = 0;
	priv->last_load_addr = 0;

	dev_set_drvdata(dev, rproc);
	priv->workqueue = create_workqueue(dev_name(dev));
	if (!priv->workqueue) {
		dev_err(dev, "cannot create workqueue\n");
		ret = -ENOMEM;
		goto err_put_rproc;
	}

	INIT_WORK(&priv->rproc_work, imx_rproc_vq_work);

	ret = imx_rproc_xtr_mbox_init(rproc);
	if (ret)
		goto err_put_wkq;

	ret = imx_rproc_parse_dt(dev, priv);
	if (ret)
		goto err_put_mbox;

	ret = imx_rproc_addr_init(priv, pdev);
	if (ret) {
		dev_err(dev, "failed on imx_rproc_addr_init\n");
		goto err_put_mbox;
	}

	ret = imx_rproc_detect_mode(priv);
	if (ret)
		goto err_put_mbox;

	ret = imx_rproc_clk_enable(priv);
	if (ret)
		goto err_put_mbox;

	rproc->auto_boot = false;
	if (priv->early_boot)
		rproc->auto_boot = true;

	ret = of_property_read_u32(dev->of_node, "fsl,startup-delay-ms", &priv->startup_delay);
	if (ret)
		priv->startup_delay = 0;

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed\n");
		goto err_put_clk;
	}

	return 0;

err_put_clk:
	if (!priv->early_boot)
		clk_disable_unprepare(priv->clk);
err_put_mbox:
	imx_rproc_free_mbox(rproc);
err_put_wkq:
	destroy_workqueue(priv->workqueue);
err_put_rproc:
	rproc_free(rproc);

	return ret;
}

static int imx_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct imx_rproc *priv = rproc->priv;
	if (!IS_ERR(priv->txdb_ch))
		mbox_free_channel(priv->txdb_ch);
	if (!IS_ERR(priv->rxdb_ch))
		mbox_free_channel(priv->rxdb_ch);
	if (!IS_ERR(priv->tx_ch))
		mbox_free_channel(priv->tx_ch);
	if (!IS_ERR(priv->rx_ch))
		mbox_free_channel(priv->rx_ch);

	if (!priv->early_boot)
		clk_disable_unprepare(priv->clk);
	rproc_del(rproc);
	imx_rproc_free_mbox(rproc);
	destroy_workqueue(priv->workqueue);
	rproc_free(rproc);

	return 0;
}

static const struct of_device_id imx_rproc_of_match[] = {
	{ .compatible = "fsl,imx7ulp-cm4", .data = &imx_rproc_cfg_imx7ulp },
	{ .compatible = "fsl,imx7d-cm4", .data = &imx_rproc_cfg_imx7d },
	{ .compatible = "fsl,imx6sx-cm4", .data = &imx_rproc_cfg_imx6sx },
	{ .compatible = "fsl,imx8mq-cm4", .data = &imx_rproc_cfg_imx8mq },
	{ .compatible = "fsl,imx8mm-cm4", .data = &imx_rproc_cfg_imx8mq },
	{ .compatible = "fsl,imx8mn-cm7", .data = &imx_rproc_cfg_imx8mn },
	{ .compatible = "fsl,imx8mp-cm7", .data = &imx_rproc_cfg_imx8mn },
	{ .compatible = "fsl,imx8qxp-cm4", .data = &imx_rproc_cfg_imx8qxp },
	{ .compatible = "fsl,imx8qm-cm4", .data = &imx_rproc_cfg_imx8qm },
	{ .compatible = "fsl,imx8ulp-cm33", .data = &imx_rproc_cfg_imx8ulp },
	{ .compatible = "fsl,imx93-cm33", .data = &imx_rproc_cfg_imx93 },
	{},
};
MODULE_DEVICE_TABLE(of, imx_rproc_of_match);

static struct platform_driver imx_rproc_driver = {
	.probe = imx_rproc_probe,
	.remove = imx_rproc_remove,
	.driver = {
		.name = "imx-rproc",
		.of_match_table = imx_rproc_of_match,
	},
};

module_platform_driver(imx_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("i.MX remote processor control driver");
MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
