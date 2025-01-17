// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

/*
 * Hardware interface for audio DSP on Baytrail, Braswell and Cherrytrail.
 */


#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/device.h>
#include <sound/sof.h>
#include <uapi/sound/sof-fw.h>

#include "sof-priv.h"
#include "ops.h"
#include "intel.h"

/* DSP memories */
#define IRAM_OFFSET		0x0C0000
#define IRAM_SIZE		(80 * 1024)
#define DRAM_OFFSET		0x100000
#define DRAM_SIZE		(160 * 1024)
#define SHIM_OFFSET		0x140000
#define SHIM_SIZE		0x100
#define MBOX_OFFSET		0x144000
#define MBOX_SIZE		0x1000

/* DSP peripherals */
#define DMAC0_OFFSET		0x098000
#define DMAC1_OFFSET		0x09c000
#define DMAC2_OFFSET		0x094000
#define DMAC_SIZE		0x420
#define SSP0_OFFSET		0x0a0000
#define SSP1_OFFSET		0x0a1000
#define SSP2_OFFSET		0x0a2000
#define SSP3_OFFSET		0x0a4000
#define SSP4_OFFSET		0x0a5000
#define SSP5_OFFSET		0x0a6000
#define SSP_SIZE		0x100

#define BYT_PCI_BAR_SIZE	0x200000

/*
 * Debug
 */

#define MBOX_DUMP_SIZE	0x30

/* BARs */
#define BYT_DSP_BAR		0
#define BYT_PCI_BAR		1
#define BYT_IMR_BAR		2

static const struct snd_sof_debugfs_map byt_debugfs[] = {
	{"dmac0", BYT_DSP_BAR, DMAC0_OFFSET, DMAC_SIZE},
	{"dmac1", BYT_DSP_BAR,  DMAC1_OFFSET, DMAC_SIZE},
	{"ssp0",  BYT_DSP_BAR, SSP0_OFFSET, SSP_SIZE},
	{"ssp1", BYT_DSP_BAR, SSP1_OFFSET, SSP_SIZE},
	{"ssp2", BYT_DSP_BAR, SSP2_OFFSET, SSP_SIZE},
	{"iram", BYT_DSP_BAR, IRAM_OFFSET, IRAM_SIZE},
	{"dram", BYT_DSP_BAR, DRAM_OFFSET, DRAM_SIZE},
	{"shim", BYT_DSP_BAR, SHIM_OFFSET, SHIM_SIZE},
	{"mbox", BYT_DSP_BAR, MBOX_OFFSET, MBOX_SIZE},
};

static const struct snd_sof_debugfs_map cht_debugfs[] = {
	{"dmac0", BYT_DSP_BAR, DMAC0_OFFSET, DMAC_SIZE},
	{"dmac1", BYT_DSP_BAR,  DMAC1_OFFSET, DMAC_SIZE},
	{"dmac2", BYT_DSP_BAR,  DMAC2_OFFSET, DMAC_SIZE},
	{"ssp0",  BYT_DSP_BAR, SSP0_OFFSET, SSP_SIZE},
	{"ssp1", BYT_DSP_BAR, SSP1_OFFSET, SSP_SIZE},
	{"ssp2", BYT_DSP_BAR, SSP2_OFFSET, SSP_SIZE},
	{"ssp3", BYT_DSP_BAR, SSP3_OFFSET, SSP_SIZE},
	{"ssp4", BYT_DSP_BAR, SSP4_OFFSET, SSP_SIZE},
	{"ssp5", BYT_DSP_BAR, SSP5_OFFSET, SSP_SIZE},
	{"iram", BYT_DSP_BAR, IRAM_OFFSET, IRAM_SIZE},
	{"dram", BYT_DSP_BAR, DRAM_OFFSET, DRAM_SIZE},
	{"shim", BYT_DSP_BAR, SHIM_OFFSET, SHIM_SIZE},
	{"mbox", BYT_DSP_BAR, MBOX_OFFSET, MBOX_SIZE},
};

static void byt_dump(struct snd_sof_dev *sdev, u32 flags)
{
	u32 val;
	int i;

	if (flags & SOF_DBG_REGS) {
		for (i = SHIM_OFFSET; i < SHIM_OFFSET  + SHIM_SIZE; i += 8 ) {
			dev_dbg(sdev->dev, "shim 0x%2.2x value 0x%16.16llx\n",
				i - SHIM_OFFSET, 
				snd_sof_dsp_read64(sdev, BYT_DSP_BAR, i));
		}
	}

	if (flags & SOF_DBG_MBOX) {
		for (i = MBOX_OFFSET; i < MBOX_OFFSET + MBOX_DUMP_SIZE; i += 4) {
			dev_dbg(sdev->dev, "mbox: 0x%2.2x value 0x%8.8x\n",
				i - MBOX_OFFSET,
				readl(sdev->bar[BYT_DSP_BAR] + i));
		}
	}

	if (flags & SOF_DBG_TEXT) {
		for (i = IRAM_OFFSET; i < IRAM_OFFSET + MBOX_DUMP_SIZE; i += 4) {
			dev_dbg(sdev->dev, "iram: 0x%2.2x value 0x%8.8x\n",
				i - IRAM_OFFSET,
				readl(sdev->bar[BYT_DSP_BAR] + i));
		}
	}
#if 0
	if (flags & SOF_DBG_PCI && sdev->pci == NULL) {
		for (i = 0; i < 0xff; i += 4) {
			dev_dbg(sdev->dev, "pci: 0x%2.2x value 0x%8.8x\n",
				i, readl(sdev->bar[BYT_PCI_BAR] + i));
		}
	}
#endif
	if (flags & SOF_DBG_PCI && sdev->pci) {
		for (i = 0; i < 0xff; i += 4) {
			pci_read_config_dword(sdev->pci, i, &val);
			dev_dbg(sdev->dev, "pci: 0x%2.2x value 0x%8.8x\n",
				i, val);
		}
	}
}

/*
 * Register IO
 */

static void byt_write(struct snd_sof_dev *sdev, void __iomem *addr,
	u32 value)
{
	writel(value, addr);
}

static u32 byt_read(struct snd_sof_dev *sdev, void __iomem *addr)
{
	return readl(addr);
}

static void byt_write64(struct snd_sof_dev *sdev, void __iomem *addr,
	u64 value)
{
	memcpy_toio(addr, &value, sizeof(value));
}

static u64 byt_read64(struct snd_sof_dev *sdev, void __iomem *addr)
{
	u64 val;

	memcpy_fromio(&val, addr, sizeof(val));
	return val;
}

/*
 * Memory copy.
 */

static void byt_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
	size_t size)
{
	volatile void __iomem *dest = sdev->bar[sdev->mmio_bar] + offset;
	u32 tmp = 0;
	int i, m, n;
	const u8 *src_byte = src;

	m = size / 4;
	n = size % 4;

	/* __iowrite32_copy use 32bit size values so divide by 4 */
	__iowrite32_copy((void *)dest, src, m);

	if (n) {
		for (i = 0; i < n; i++)
			tmp |= (u32)*(src_byte + m * 4 + i) << (i * 8);
		__iowrite32_copy((void *)(dest + m * 4), &tmp, 1);
	}
}

static void byt_block_read(struct snd_sof_dev *sdev, u32 offset, void *dest,
	size_t size)
{
	volatile void __iomem *src = sdev->bar[sdev->mmio_bar] + offset;
	memcpy_fromio(dest, src, size);
}

/*
 * IPC Firmware ready.
 */
static int byt_fw_ready(struct snd_sof_dev *sdev, u32 msg_id)
{
	struct sof_ipc_fw_ready *fw_ready = &sdev->fw_ready;
	struct sof_ipc_fw_version *v = &fw_ready->version;
	u32 offset;

	/* mailbox must be on 4k boundary */
	offset = MBOX_OFFSET;

	dev_dbg(sdev->dev, "ipc: DSP is ready 0x%8.8x offset 0x%x\n",
		msg_id, offset);

	/* copy data from the DSP FW ready offset */
	byt_block_read(sdev, offset, fw_ready, sizeof(*fw_ready));

	snd_sof_dsp_mailbox_init(sdev, fw_ready->dspbox_offset,
		fw_ready->dspbox_size, fw_ready->hostbox_offset,
		fw_ready->hostbox_size);

	dev_dbg(sdev->dev, " mailbox DSP initiated 0x%x - size 0x%x\n",
		fw_ready->dspbox_offset, fw_ready->dspbox_size);
	dev_dbg(sdev->dev, " mailbox Host initiated 0x%x - size 0x%x\n",
		fw_ready->hostbox_offset, fw_ready->hostbox_size);
	
	dev_info(sdev->dev, " Firmware info: version %d:%d-%s build %d on %s:%s\n", 
		v->major, v->minor, v->tag, v->build, v->date, v->time);

	return 0;
}

/*
 * IPC Mailbox IO
 */

static void byt_mailbox_write(struct snd_sof_dev *sdev, u32 offset,
	void *message, size_t bytes)
{
	void __iomem *dest = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_toio(dest, message, bytes);
}

static void byt_mailbox_read(struct snd_sof_dev *sdev, u32 offset,
	void *message, size_t bytes)
{
	void __iomem *src = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_fromio(message, src, bytes);
}

/*
 * IPC Doorbell IRQ handler and thread.
 */

static irqreturn_t byt_irq_handler(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *) context;
	u64 isr;
	int ret = IRQ_NONE;

	/* Interrupt arrived, check src */
	isr = snd_sof_dsp_read64(sdev, BYT_DSP_BAR, SHIM_ISRX);
	if (isr & SHIM_ISRX_DONE) {

		/* Mask Done interrupt before return */
		snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IMRX,
			SHIM_IMRX_DONE, SHIM_IMRX_DONE);
		ret = IRQ_WAKE_THREAD;
	}

	if (isr & SHIM_ISRX_BUSY) {

		/* Mask Busy interrupt before return */
		snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IMRX,
			SHIM_IMRX_BUSY, SHIM_IMRX_BUSY);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t byt_irq_thread(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *) context;
	u64 ipcx, ipcd;

	ipcx = snd_sof_dsp_read64(sdev, BYT_DSP_BAR, SHIM_IPCX);

	/* reply message from DSP */
	if (ipcx & SHIM_BYT_IPCX_DONE) {

		/* Handle Immediate reply from DSP Core */
		snd_sof_ipc_reply(sdev, ipcx);

		/* clear DONE bit - tell DSP we have completed */
		snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IPCX,
			SHIM_BYT_IPCX_DONE, 0);

		/* unmask Done interrupt */
		snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IMRX,
			SHIM_IMRX_DONE, 0);
	}

	/* new message from DSP */
	ipcd = snd_sof_dsp_read64(sdev, BYT_DSP_BAR, SHIM_IPCD);
	if (ipcd & SHIM_BYT_IPCD_BUSY) {

		/* Handle messages from DSP Core */
		snd_sof_ipc_msgs_rx(sdev);
	}

	/* continue to send any remaining messages... */
	snd_sof_ipc_msgs_tx(sdev);

	return IRQ_HANDLED;
}

static int byt_is_ready(struct snd_sof_dev *sdev)
{
	uint64_t imrx;

	imrx = snd_sof_dsp_read64(sdev, BYT_DSP_BAR, SHIM_IMRX);
	if (imrx & SHIM_IMRX_DONE)
		return 0;

	return 1;
}

static int byt_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	u64 cmd = msg->header;

	/* send the message */
	byt_mailbox_write(sdev, sdev->host_box.offset, msg->msg_data, msg->msg_size);
	snd_sof_dsp_write64(sdev, BYT_DSP_BAR, SHIM_IPCX,
		cmd | SHIM_BYT_IPCX_BUSY);

	return 0;
}

static int byt_get_reply(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct sof_ipc_reply reply;
	int ret = 0;
	u32 size;

	/* get reply */
	byt_mailbox_read(sdev, sdev->host_box.offset, &reply, sizeof(reply));
	if (reply.error < 0) {
		size = sizeof(reply);
		ret = reply.error;
	} else {
		/* reply correct size ? */
		if (reply.hdr.size != msg->reply_size) {
			dev_err(sdev->dev, "error: reply expected 0x%lx got 0x%x bytes\n",
				msg->reply_size, reply.hdr.size);
			size = msg->reply_size;
			ret = -EINVAL;
		} else {
			size = reply.hdr.size;
		}
	}

	/* read the message */
	if (msg->msg_data && size > 0)
		byt_mailbox_read(sdev, sdev->host_box.offset, msg->reply_data, size);

	return ret;
}

static int byt_cmd_done(struct snd_sof_dev *sdev)
{
	/* clear BUSY bit and set DONE bit - accept new messages */
	snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IPCD,
		SHIM_BYT_IPCD_BUSY | SHIM_BYT_IPCD_DONE,
		SHIM_BYT_IPCD_DONE);

	/* unmask busy interrupt */
	snd_sof_dsp_update_bits64_unlocked(sdev, BYT_DSP_BAR, SHIM_IMRX,
		SHIM_IMRX_BUSY, 0);

	return 0;
}

/*
 * DSP control.
 */

static int byt_run(struct snd_sof_dev *sdev)
{
	int tries = 10;

	/* release stall and wait to unstall */
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_CSR,
		SHIM_BYT_CSR_STALL, 0x0);
	while (tries--) {
		if (!(snd_sof_dsp_read64(sdev, BYT_DSP_BAR, SHIM_CSR) &
		      SHIM_BYT_CSR_PWAITMODE))
			break;
		msleep(100);
	}
	if (tries < 0) {
		dev_err(sdev->dev, "error:  unable to run DSP firmware\n");
		byt_dump(sdev, SOF_DBG_REGS | SOF_DBG_MBOX);
		return -ENODEV;
	}

	return 0;
}

static int byt_reset(struct snd_sof_dev *sdev)
{
	/* put DSP into reset, set reset vector and stall */
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_CSR,
		SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL | SHIM_BYT_CSR_STALL,
		SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL | SHIM_BYT_CSR_STALL);

	udelay(10);

	/* take DSP out of reset and keep stalled for FW loading */
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_CSR,
		SHIM_BYT_CSR_RST, 0);

	return 0;
}

/*
 * Probe and remove.
 */

static int byt_acpi_probe(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *pdata = sdev->pdata;
	const struct sof_dev_desc *desc = pdata->desc;
	struct platform_device *pdev =
		container_of(sdev->parent, struct platform_device, dev);
	struct resource *mmio;
	u32 base, size;
	int ret = 0;

	/* DSP DMA can only access low 31 bits of host memory */
	ret = dma_coerce_mask_and_coherent(sdev->dev, DMA_BIT_MASK(31));
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to set DMA mask %d\n", ret);
		return ret;
	}

	/* LPE base */
	mmio = platform_get_resource(pdev, IORESOURCE_MEM,
		desc->resindex_lpe_base);
	if (mmio) {
		base = mmio->start;
		size = resource_size(mmio);
	} else {
		dev_err(sdev->dev, "error: failed to get LPE base at idx %d\n",
			desc->resindex_lpe_base);
		return -EINVAL;
	}

	dev_dbg(sdev->dev, "LPE PHY base at 0x%x size 0x%x", base, size);
	sdev->bar[BYT_DSP_BAR] = ioremap(base, size);
	if (sdev->bar[BYT_DSP_BAR] == NULL) {
		dev_err(sdev->dev, "error: failed to ioremap LPE base 0x%x size 0x%x\n",
			base, size);
		return -ENODEV;
	}
	dev_dbg(sdev->dev, "LPE VADDR %p\n", sdev->bar[BYT_DSP_BAR]);

	/* TODO: add offsets */
	sdev->mmio_bar = BYT_DSP_BAR;
	sdev->mailbox_bar = BYT_DSP_BAR;

	/* IMR base - optional */
	if (desc->resindex_imr_base == -1)
		goto irq;

	mmio = platform_get_resource(pdev, IORESOURCE_MEM,
		desc->resindex_imr_base);
	if (mmio) {
		base = mmio->start;
		size = resource_size(mmio);
	} else {
		dev_err(sdev->dev, "error: failed to get IMR base at idx %d\n",
			desc->resindex_imr_base);
		ret = -ENODEV;
		goto imr_err;
	}

	/* some BIOSes dont map IMR */
	if (base == 0x55aa55aa || base == 0x0) {
		dev_info(sdev->dev, "IMR not set by BIOS. Ignoring\n");
		goto irq;
	}

	dev_dbg(sdev->dev, "IMR base at 0x%x size 0x%x", base, size);
	sdev->bar[BYT_IMR_BAR] = ioremap(base, size);
	if (sdev->bar[BYT_IMR_BAR] == NULL) {
		dev_err(sdev->dev, "error: failed to ioremap IMR base 0x%x size 0x%x\n",
			base, size);
		ret = -ENODEV;
		goto imr_err;
	}
	dev_dbg(sdev->dev, "IMR VADDR %p\n", sdev->bar[BYT_IMR_BAR]);

irq:
	/* register our IRQ */
	sdev->ipc_irq = platform_get_irq(pdev, desc->irqindex_host_ipc);
	if (sdev->ipc_irq < 0) {
		dev_err(sdev->dev, "error: failed to get IRQ at index %d\n",
			desc->irqindex_host_ipc);
		ret = sdev->ipc_irq;
		goto irq_err;
	}

	dev_dbg(sdev->dev, "using IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, byt_irq_handler,
		byt_irq_thread, IRQF_SHARED, "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IRQ %d\n",
			sdev->ipc_irq);
		goto irq_err;		
	}

	/* enable Interrupt from both sides */
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_IMRX, 0x3, 0x0);
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_IMRD, 0x3, 0x0);

	/* set BARS */
	sdev->cl_bar = BYT_DSP_BAR;

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = MBOX_OFFSET;

	return ret;

irq_err:
	iounmap(sdev->bar[BYT_IMR_BAR]);
imr_err:
	iounmap(sdev->bar[BYT_DSP_BAR]);	
	return ret;
}

static int byt_pci_probe(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *pdata = sdev->pdata;
	const struct sof_dev_desc *desc = pdata->desc;
	struct pci_dev *pci = sdev->pci;
	u32 base, size;
	int ret = 0;

	/* DSP DMA can only access low 31 bits of host memory */
	ret = dma_coerce_mask_and_coherent(&pci->dev, DMA_BIT_MASK(31));
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to set DMA mask %d\n", ret);
		return ret;
	}

	/* LPE base */
	base = pci_resource_start(pci, desc->resindex_lpe_base) - IRAM_OFFSET;
	size = BYT_PCI_BAR_SIZE;

	dev_dbg(sdev->dev, "LPE PHY base at 0x%x size 0x%x", base, size);
	sdev->bar[BYT_DSP_BAR] = ioremap(base, size);
	if (sdev->bar[BYT_DSP_BAR] == NULL) {
		dev_err(sdev->dev, "error: failed to ioremap LPE base 0x%x size 0x%x\n",
			base, size);
		return -ENODEV;
	}
	dev_dbg(sdev->dev, "LPE VADDR %p\n", sdev->bar[BYT_DSP_BAR]);

	/* IMR base - optional */
	if (desc->resindex_imr_base == -1)
		goto irq;

	base = pci_resource_start(pci, desc->resindex_imr_base);
	size = pci_resource_len(pci, desc->resindex_imr_base);

	/* some BIOSes dont map IMR */
	if (base == 0x55aa55aa || base == 0x0) {
		dev_info(sdev->dev, "IMR not set by BIOS. Ignoring\n");
		goto irq;
	}

	dev_dbg(sdev->dev, "IMR base at 0x%x size 0x%x", base, size);
	sdev->bar[BYT_IMR_BAR] = ioremap(base, size);
	if (sdev->bar[BYT_IMR_BAR] == NULL) {
		dev_err(sdev->dev, "error: failed to ioremap IMR base 0x%x size 0x%x\n",
			base, size);
		ret = -ENODEV;
		goto imr_err;
	}
	dev_dbg(sdev->dev, "IMR VADDR %p\n", sdev->bar[BYT_IMR_BAR]);

irq:
	/* register our IRQ */
	sdev->ipc_irq = pci->irq;
	dev_dbg(sdev->dev, "using IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, byt_irq_handler,
		byt_irq_thread, 0, "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IRQ %d\n",
			sdev->ipc_irq);
		goto irq_err;		
	}

	/* enable Interrupt from both sides */
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_IMRX, 0x3, 0x0);
	snd_sof_dsp_update_bits64(sdev, BYT_DSP_BAR, SHIM_IMRD, 0x3, 0x0);

	/* set BARS */
	sdev->cl_bar = BYT_DSP_BAR;

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = MBOX_OFFSET;

	return ret;

irq_err:
	iounmap(sdev->bar[BYT_IMR_BAR]);
imr_err:
	iounmap(sdev->bar[BYT_DSP_BAR]);	
	return ret;
}

static int byt_probe(struct snd_sof_dev *sdev)
{
	if (sdev->pci)
		return byt_pci_probe(sdev);
	else
		return byt_acpi_probe(sdev);
}

static int byt_acpi_remove(struct snd_sof_dev *sdev)
{
	iounmap(sdev->bar[BYT_DSP_BAR]);
	iounmap(sdev->bar[BYT_PCI_BAR]);
	iounmap(sdev->bar[BYT_IMR_BAR]);
	free_irq(sdev->ipc_irq, sdev);
	return 0;
}

static int byt_pci_remove(struct snd_sof_dev *sdev)
{
	free_irq(sdev->ipc_irq, sdev);
	return 0;
}

static int byt_remove(struct snd_sof_dev *sdev)
{
	if (sdev->pci)
		return byt_pci_remove(sdev);
	else
		return byt_acpi_remove(sdev);
}

/* baytrail ops */
struct snd_sof_dsp_ops snd_sof_byt_ops = {

	/* device init */
	.probe		= byt_probe,
	.remove		= byt_remove,

	/* DSP core boot / reset */
	.run		= byt_run,
	.reset		= byt_reset,

	/* Register IO */
	.write		= byt_write,
	.read		= byt_read,
	.write64	= byt_write64,
	.read64		= byt_read64,

	/* Block IO */
	.block_read	= byt_block_read,
	.block_write	= byt_block_write,

	/* doorbell */
	.irq_handler	= byt_irq_handler,
	.irq_thread	= byt_irq_thread,

	/* mailbox */
	.mailbox_read	= byt_mailbox_read,
	.mailbox_write	= byt_mailbox_write,

	/* ipc */
	.send_msg	= byt_send_msg,
	.get_reply	= byt_get_reply,
	.fw_ready	= byt_fw_ready,
	.is_ready	= byt_is_ready,
	.cmd_done	= byt_cmd_done,

	/* debug */
	.debug_map	= byt_debugfs,
	.debug_map_count	= ARRAY_SIZE(byt_debugfs),
	.dbg_dump	= byt_dump,

	/* module loading */
	.load_module	= snd_sof_parse_module_memcpy,

	/*Firmware loading */
 	.load_firmware	= snd_sof_load_firmware_memcpy,
};
EXPORT_SYMBOL(snd_sof_byt_ops);

/* cherrytrail and braswell ops */
struct snd_sof_dsp_ops snd_sof_cht_ops = {

	/* device init */
	.probe		= byt_probe,
	.remove		= byt_remove,

	/* DSP core boot / reset */
	.run		= byt_run,
	.reset		= byt_reset,

	/* Register IO */
	.write		= byt_write,
	.read		= byt_read,
	.write64	= byt_write64,
	.read64		= byt_read64,

	/* Block IO */
	.block_read	= byt_block_read,
	.block_write	= byt_block_write,

	/* doorbell */
	.irq_handler	= byt_irq_handler,
	.irq_thread	= byt_irq_thread,

	/* mailbox */
	.mailbox_read	= byt_mailbox_read,
	.mailbox_write	= byt_mailbox_write,

	/* ipc */
	.send_msg	= byt_send_msg,
	.get_reply	= byt_get_reply,
	.fw_ready	= byt_fw_ready,
	.is_ready	= byt_is_ready,
	.cmd_done	= byt_cmd_done,

	/* debug */
	.debug_map	= cht_debugfs,
	.debug_map_count	= ARRAY_SIZE(cht_debugfs),
	.dbg_dump	= byt_dump,

	/* module loading */
	.load_module	= snd_sof_parse_module_memcpy,

	/*Firmware loading */
 	.load_firmware	= snd_sof_load_firmware_memcpy,
};
EXPORT_SYMBOL(snd_sof_cht_ops);

MODULE_LICENSE("Dual BSD/GPL");
