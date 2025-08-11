// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#include <asm/set_memory.h>
#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/version.h>

#include "luxyd-ai-ioctl.h"

#define DRIVER_NAME	"luxyd-ai"
#define DRIVER_VERSION	"0.1"

MODULE_AUTHOR("Wan Ahmad Zainie <wanahmadzainie@gmail.com>");
MODULE_DESCRIPTION("LUXYD Technologies AI module driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

/* Hardware information */
#define VENDOR_ID			0x10ee	/* Xilinx Vendor ID */
#define DEVICE_ID			0x7021	/* Kintex-7 Device ID */

/* LUXYD AI Status Control Register */
#define LUXYD_AI_CMD_OFFSET		0xc0000
#define LUXYD_AI_CMD_START		BIT(8)

#define LUXYD_AI_INFO_OFFSET		0xc0004
#define LUXYD_AI_MATA_ROWCOUNT		GENMASK(7, 0)
#define LUXYD_AI_MATA_COLCOUNT		GENMASK(15, 8)
#define LUXYD_AI_MATB_ROWCOUNT		GENMASK(23, 16)
#define LUXYD_AI_MATB_COLCOUNT		GENMASK(31, 24)

#define LUXYD_AI_STATUS_OFFSET		0xc0008
#define LUXYD_AI_STATUS_READY		BIT(8)

#define CMD_SIGNATURE			(0xac)
#define STATUS_SIGNATURE		(0xfc)

#define DMA_BUFFER_SIZE			(4 * 1024 * 1024)

/* Borrowed from drivers/dma/xilinx/xdma-regs.h */
/* descriptor definitions */
#define XDMA_DESC_ADJACENT		32
#define XDMA_DESC_ADJACENT_MASK		(XDMA_DESC_ADJACENT - 1)
#define XDMA_DESC_ADJACENT_BITS		GENMASK(13, 8)
#define XDMA_DESC_MAGIC			0xad4bUL
#define XDMA_DESC_MAGIC_BITS		GENMASK(31, 16)
#define XDMA_DESC_FLAGS_BITS		GENMASK(7, 0)
#define XDMA_DESC_STOPPED		BIT(0)
#define XDMA_DESC_COMPLETED		BIT(1)
#define XDMA_DESC_BLEN_BITS		28
#define XDMA_DESC_BLEN_MAX		(BIT(XDMA_DESC_BLEN_BITS) - PAGE_SIZE)

/* macros to construct the descriptor control word */
#define XDMA_DESC_CONTROL(adjacent, flag)				\
	(FIELD_PREP(XDMA_DESC_MAGIC_BITS, XDMA_DESC_MAGIC) |		\
	 FIELD_PREP(XDMA_DESC_ADJACENT_BITS, (adjacent) - 1) |		\
	 FIELD_PREP(XDMA_DESC_FLAGS_BITS, (flag)))

/* Channel registers */
#define XDMA_CHAN_IDENTIFIER		0x0
#define XDMA_CHAN_CONTROL		0x4
#define XDMA_CHAN_CONTROL_W1S		0x8
#define XDMA_CHAN_CONTROL_W1C		0xc
#define XDMA_CHAN_STATUS		0x40
#define XDMA_CHAN_STATUS_RC		0x44
#define XDMA_CHAN_COMPLETED_DESC	0x48
#define XDMA_CHAN_ALIGNMENTS		0x4c
#define XDMA_CHAN_INTR_ENABLE		0x90
#define XDMA_CHAN_INTR_ENABLE_W1S	0x94
#define XDMA_CHAN_INTR_ENABLE_W1C	0x9c

#define XDMA_CHAN_STRIDE		0x100
#define XDMA_CHAN_H2C_OFFSET		0x0
#define XDMA_CHAN_C2H_OFFSET		0x1000
#define XDMA_CHAN_H2C_TARGET		0x0
#define XDMA_CHAN_C2H_TARGET		0x1

/* Channel SGDMA registers */
#define XDMA_SGDMA_IDENTIFIER		0x4000
#define XDMA_SGDMA_DESC_LO		0x4080
#define XDMA_SGDMA_DESC_HI		0x4084
#define XDMA_SGDMA_DESC_ADJ		0x4088
#define XDMA_SGDMA_DESC_CREDIT		0x408c

/* XDMA descriptor */
struct xdma_desc {
	__le32	control;
	__le32	bytes;		/* transfer length in bytes */
	__le64	src_addr;	/* source address */
	__le64	dst_addr;	/* destination address */
	__le64	next_desc;	/* next desc address */
};

#define XDMA_DESC_SIZE			sizeof(struct xdma_desc)
#define XDMA_DESC_BLOCK_SIZE		(XDMA_DESC_SIZE * XDMA_DESC_ADJACENT)
#define XDMA_DESC_BLOCK_ALIGN		32
#define XDMA_DESC_BLOCK_BOUNDARY	4096

/* Private data structure */
struct luxyd_ai_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	struct class *dev_class;
	struct device *dev;

	/* BARs addresses */
	void __iomem *bar0_virt_addr;
	void __iomem *bar1_virt_addr;

	/* XDMA descriptor pool */
	struct dma_pool *dma_desc_pool;
	void *dma_desc_virt;
	dma_addr_t dma_desc_phys;
	size_t dma_desc_size;

	/* DMA coherent memory for mmap */
	void *dma_buffer_virt;
	dma_addr_t dma_buffer_phys;
	size_t dma_buffer_size;
	size_t dma_allocated_size;

	/* Store matrices information */
	struct matrix_info matrix_info;

	struct mutex ioctl_lock;
};

static struct class *luxyd_ai_class;
static int luxyd_ai_major;

/* PCI device ID table */
static const struct pci_device_id luxyd_fpga_id_table[] = {
	//{ PCI_DEVICE(PCI_ANY_ID, PCI_ANY_ID) },
	{ PCI_DEVICE(0x80ee, 0xbeef) },		/* VirtualBox Graphics Adapter */
	{ PCI_DEVICE(0x10ee, 0x7011) },		/* Xilinx Kintex-7 - LUXYD AI */
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, luxyd_fpga_id_table);

static int luxyd_ai_open(struct inode *inode, struct file *filp)
{
	struct luxyd_ai_device *drvdata = container_of(inode->i_cdev,
						       struct luxyd_ai_device,
						       cdev);
	filp->private_data = drvdata;

	pr_info("%s: device opened\n", DRIVER_NAME);

	return 0;
}

static int luxyd_ai_release(struct inode *inode, struct file *filp)
{
	pr_info("%s: device released\n", DRIVER_NAME);

	return 0;
}

static int do_dma_transfer(struct luxyd_ai_device *drvdata, u32 len,
			   u64 src_addr, u64 dst_addr,
			   enum dma_transfer_direction dir)
{
	struct xdma_desc *dma_desc;
	int timeout;
	u32 val;


	val = ioread32(drvdata->bar1_virt_addr);
	pr_info("%s: H2C DMA Engine ID (0x%08x)\n", DRIVER_NAME, val);
	val = ioread32(drvdata->bar1_virt_addr + 0x1000);
	pr_info("%s: C2H DMA Engine ID (0x%08x)\n", DRIVER_NAME, val);

	/* Create XDMA descriptor */
	len = ALIGN(len, 0x1000);
	dma_desc = drvdata->dma_desc_virt;
	dma_desc->control = XDMA_DESC_CONTROL(1, 0x13); // 0xad4b0013
	dma_desc->bytes = cpu_to_le32(len);
	dma_desc->src_addr = cpu_to_le64(src_addr);
	dma_desc->dst_addr = cpu_to_le64(dst_addr);
	dma_desc->next_desc = cpu_to_le64(0);

	pr_info("%s: %s DMA descriptor information\n", DRIVER_NAME, __func__);
	pr_info("%s: location=%p control=0x%08x\n",
		DRIVER_NAME, dma_desc, dma_desc->control);
	pr_info("%s: bytes=0x%08x src_addr=0x%08llx dst_addr=0x%08llx\n",
		DRIVER_NAME, dma_desc->bytes, dma_desc->src_addr,
		dma_desc->dst_addr);

	if (dir == DMA_MEM_TO_DEV) {		/* H2C */
		/* write first XDMA descriptor */
		val = lower_32_bits(drvdata->dma_desc_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x4080);
		val = upper_32_bits(drvdata->dma_desc_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x4084);

		/* sync the buffer */
		dma_sync_single_for_device(&drvdata->pdev->dev,
					   drvdata->dma_buffer_phys,
					   drvdata->dma_buffer_size,
					   DMA_TO_DEVICE);

		/* read clear H2C status */
		val = ioread32(drvdata->bar1_virt_addr + 0x0044);
		pr_info("%s: H2C status (0x%08x)\n", DRIVER_NAME, val);

		/* kick off H2C DMA transfer */
		iowrite32(0x00fffe7f, drvdata->bar1_virt_addr + 0x0004);
		pr_info("%s: kick off C2H DMA transfer (0x%08x)\n", DRIVER_NAME,
			ioread32(drvdata->bar1_virt_addr + 0x1004));

		/* poll H2C status */
		for (timeout = 0; timeout < 10; timeout++) {
			val = ioread32(drvdata->bar1_virt_addr + 0x0040);
			pr_info("%s: poll H2C status (0x%08x)\n",
				DRIVER_NAME, val);
			if ((val & BIT(0)) == 0x0)
				break;

			udelay(10);
		}

		if (timeout == 0)
                        pr_err("%s: H2C DMA transfer timeout\n", DRIVER_NAME);

		/* read H2C descriptor count */
		val = ioread32(drvdata->bar1_virt_addr + 0x0048);
		if (!val)
			pr_err("%s: no H2C descriptor found\n", DRIVER_NAME);
		else
			pr_info("%s: H2C DMA descriptor found (%d)\n",
				DRIVER_NAME, val);

		/* read clear H2C status */
		val = ioread32(drvdata->bar1_virt_addr + 0x0044);
		pr_info("%s: H2C status (0x%08x)\n", DRIVER_NAME, val);
		if (val == 0x6)
			pr_info("%s: H2C DMA transfer completed\n", DRIVER_NAME);
		else
			pr_err("%s: H2C DMA transfer failed\n", DRIVER_NAME);

		/* clear run stop bit on H2C */
		iowrite32(val, drvdata->bar1_virt_addr + 0x000c);
		pr_info("%s: stop H2C DMA transfer\n", DRIVER_NAME);
	} else if (dir == DMA_DEV_TO_MEM) {	/* C2H */
		/* write first XDMA descriptor */
		val = lower_32_bits(drvdata->dma_desc_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x5080);
		val = upper_32_bits(drvdata->dma_desc_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x5084);

		/* read clear C2H status */
		val = ioread32(drvdata->bar1_virt_addr + 0x1044);
		pr_info("%s: C2H status (0x%08x)\n", DRIVER_NAME, val);

		/* kick off C2H DMA transfer */
		iowrite32(0x00fffe7f, drvdata->bar1_virt_addr + 0x1004);
		pr_info("%s: kick off C2H DMA transfer (0x%08x)\n", DRIVER_NAME,
			ioread32(drvdata->bar1_virt_addr + 0x1004));

		/* poll C2H status */
		for (timeout = 0; timeout < 10; timeout++) {
			val = ioread32(drvdata->bar1_virt_addr + 0x1040);
			pr_info("%s: poll C2H status (0x%08x)\n",
				DRIVER_NAME, val);
			if ((val & BIT(0)) == 0x0)
				break;

			udelay(10);
		}

		if (timeout == 0)
                        pr_err("%s: C2H DMA transfer timeout\n", DRIVER_NAME);

		/* read C2H descriptor count */
		val = ioread32(drvdata->bar1_virt_addr + 0x1048);
		if (!val)
			pr_err("%s: no C2H descriptor found\n", DRIVER_NAME);
		else
			pr_info("%s: C2H DMA descriptor found (%d)\n",
				DRIVER_NAME, val);

		/* read clear C2H status */
		val = ioread32(drvdata->bar1_virt_addr + 0x1044);
		pr_info("%s: C2H status (0x%08x)\n", DRIVER_NAME, val);
		if (val == 0x6)
			pr_info("%s: C2H DMA transfer completed\n", DRIVER_NAME);
		else
			pr_err("%s: C2H DMA transfer failed\n", DRIVER_NAME);

		/* clear run stop bit on C2H */
                iowrite32(val, drvdata->bar1_virt_addr + 0x100c);
		pr_info("%s: stop C2H DMA transfer\n", DRIVER_NAME);

		/* sync the buffer */
		dma_sync_single_for_cpu(&drvdata->pdev->dev,
					drvdata->dma_buffer_phys,
					drvdata->dma_buffer_size,
					DMA_FROM_DEVICE);
	} else {
		pr_err("%s: invalid direction\n", DRIVER_NAME);
		return -EINVAL;
	}

	udelay(10);

	return 0;
}

static long luxyd_ai_unlocked_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	struct luxyd_ai_device *drvdata = filp->private_data;
	int bytes_to_transfer;
	int timeout;
	int ret = 0;
	u32 val;

	//if (mutex_lock_interruptible(&drvdata->ioctl_lock))
	//	return -ERESTARTSYS;

	switch (cmd) {
	case LUXYD_AI_STATUS_GET:
		pr_info("%s: ioctl cmd LUXYD_AI_STATUS_GET\n", DRIVER_NAME);
		break;

	case LUXYD_AI_MODEL_LOAD:
		pr_info("%s: ioctl cmd LUXYD_AI_MODEL_LOAD\n", DRIVER_NAME);
		break;

	case LUXYD_AI_INFERENCE_START:
		pr_info("%s: ioctl cmd LUXYD_AI_INFERENCE_START\n",
			DRIVER_NAME);
		break;

	case LUXYD_AI_MATRIX_LOAD:
		pr_info("%s: ioctl cmd LUXYD_AI_MATRIX_LOAD\n", DRIVER_NAME);

		if (copy_from_user(&drvdata->matrix_info,
				   (struct matrix_info __user *)arg,
				   sizeof(struct matrix_info))) {
			ret = -EFAULT;
			break;
		}

		if (drvdata->matrix_info.m <= 0 ||
		    drvdata->matrix_info.n <= 0 ||
		    drvdata->matrix_info.p <= 0) {
			ret = -EINVAL;
			break;
		}

		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_INFO_OFFSET);
		pr_info("%s: INFO_REG value 0x%08x\n", DRIVER_NAME, val);

		/* Prepare and send matrix size information to the card */
		val = FIELD_PREP(LUXYD_AI_MATA_ROWCOUNT, drvdata->matrix_info.m) |
			FIELD_PREP(LUXYD_AI_MATA_COLCOUNT, drvdata->matrix_info.n) |
			FIELD_PREP(LUXYD_AI_MATB_ROWCOUNT, drvdata->matrix_info.n) |
			FIELD_PREP(LUXYD_AI_MATB_COLCOUNT, drvdata->matrix_info.p);
		iowrite32(val, drvdata->bar0_virt_addr + LUXYD_AI_INFO_OFFSET);

		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_INFO_OFFSET);
		pr_info("%s: new INFO_REG value 0x%08x\n", DRIVER_NAME, val);

#if 1
		/* H2C DMA transfer the whole memory chunk */
		bytes_to_transfer = drvdata->dma_allocated_size;
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      drvdata->dma_buffer_phys,
				      0,
				      DMA_MEM_TO_DEV);
#else
		/* Matrix A H2C DMA transfer */
		bytes_to_transfer =
			drvdata->matrix_info.m * drvdata->matrix_info.n * sizeof(u16);
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      drvdata->dma_buffer_phys + MAT_A_OFFSET,
				      MAT_A_OFFSET,
				      DMA_MEM_TO_DEV);

		/* Matrix B H2C DMA transfer */
		bytes_to_transfer =
			drvdata->matrix_info.n * drvdata->matrix_info.p * sizeof(u16);
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      drvdata->dma_buffer_phys + MAT_B_OFFSET,
				      MAT_B_OFFSET,
				      DMA_MEM_TO_DEV);
#endif

		break;

	case LUXYD_AI_MATRIX_MULTIPLY:
		pr_info("%s: ioctl cmd LUXYD_AI_MATRIX_MULTIPLY\n",
			DRIVER_NAME);

		/* Show COMMAND_REG and STATUS_REG values */
		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_CMD_OFFSET);
		pr_info("%s: COMMAND_REG value 0x%08x\n", DRIVER_NAME, val);
		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_STATUS_OFFSET);
		pr_info("%s: STATUS_REG value  0x%08x\n", DRIVER_NAME, val);

		/* Send command to start matrix multiplication */
		val = CMD_SIGNATURE | LUXYD_AI_CMD_START;
		pr_info("%s: send 0x%08x to COMMAND_REG\n", DRIVER_NAME, val);
		iowrite32(val, drvdata->bar0_virt_addr + LUXYD_AI_CMD_OFFSET);

		for (timeout = 100; timeout > 0; timeout--) {
			val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_STATUS_OFFSET);
			pr_info("%s: poll STATUS_REG (0x%08x) timeout (%d)\n",
				DRIVER_NAME, val, timeout);
                        if ((val & BIT(8)) == BIT(8))
                                break;

                        udelay(10);
                }

		if (timeout == 0) {
			pr_err("%s: timeout while mutiplying\n", DRIVER_NAME);
			ret = -EFAULT;
			break;
		}

#if 1
		/* C2H DMA transfer the whole memory chunk */
		bytes_to_transfer = drvdata->dma_allocated_size;
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      0,
				      drvdata->dma_buffer_phys,
				      DMA_DEV_TO_MEM);
#else
		/* Matrix P C2H DMA transfer */
		bytes_to_transfer =
			drvdata->matrix_info.m * drvdata->matrix_info.p * sizeof(u32);
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      MAT_P_OFFSET,
				      drvdata->dma_buffer_phys + MAT_P_OFFSET,
				      DMA_DEV_TO_MEM);

		/* Matrix A C2H DMA transfer */
		bytes_to_transfer =
			drvdata->matrix_info.m * drvdata->matrix_info.n * sizeof(u16);
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      MAT_A_OFFSET,
				      drvdata->dma_buffer_phys + MAT_A_OFFSET,
				      DMA_DEV_TO_MEM);

		/* Matrix B C2H DMA transfer */
		bytes_to_transfer =
			drvdata->matrix_info.n * drvdata->matrix_info.p * sizeof(u16);
		ret = do_dma_transfer(drvdata, bytes_to_transfer,
				      MAT_B_OFFSET,
				      drvdata->dma_buffer_phys + MAT_B_OFFSET,
				      DMA_DEV_TO_MEM);
#endif

		break;

	default:
		pr_info("%s: ioctl cmd not supported (%d)\n", DRIVER_NAME, cmd);
		ret = -ENOTTY;
		break;
	}

	//mutex_unlock(&drvdata->ioctl_lock);

	return ret;
}

static int luxyd_ai_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct luxyd_ai_device *drvdata = filp->private_data;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn;

	drvdata->dma_allocated_size = 0;

	/* Check availability */
	if (!drvdata->dma_buffer_virt || !drvdata->dma_buffer_phys) {
		pr_err("%s: no DMA coherent buffer found\n", DRIVER_NAME);
		return -EFAULT;
	}

	/* Check size */
	if (offset + size > drvdata->dma_buffer_size) {
		pr_err("%s: mmap request out of bound\n", DRIVER_NAME);
		return -EINVAL;
	}

	/* Get page frame number */
	pfn = (drvdata->dma_buffer_phys + offset) >> PAGE_SHIFT;

	/* Remap physical address to the userspace */
	set_memory_uc((unsigned long)drvdata->dma_buffer_virt,
		      drvdata->dma_buffer_size/PAGE_SIZE);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    pfn,
			    size,
			    vma->vm_page_prot)) {
		pr_err("%s: remap_pfn_range() failed\n", DRIVER_NAME);
		return -EAGAIN;
	}

	pr_info("%s: DMA buffer (phys 0x%llx, len 0x%lx) mapped to userspace at "
		"0x%lx, length 0x%lx\n",
		DRIVER_NAME, drvdata->dma_buffer_phys, drvdata->dma_buffer_size,
		vma->vm_start, size);

	drvdata->dma_allocated_size = size;

	return 0;
}

static const struct file_operations luxyd_ai_fops = {
	.owner          = THIS_MODULE,
	.open           = luxyd_ai_open,
	.release        = luxyd_ai_release,
	.unlocked_ioctl = luxyd_ai_unlocked_ioctl,
	.mmap		= luxyd_ai_mmap,
};

static int luxyd_fpga_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct luxyd_ai_device *drvdata;
	int bar_mask;
	int ret;

	pr_info("%s: probing device 0x%04x:0x%04x\n", DRIVER_NAME,
		pdev->vendor, pdev->device);

	/* Allocate private data structure */
	drvdata = devm_kzalloc(dev, sizeof(drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dma_buffer_size = 0;
	drvdata->pdev = pdev;
	pci_set_drvdata(pdev, drvdata);

	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret) {
		pr_err("%s: pcim_enable_device failed\n", DRIVER_NAME);
		return ret;
	}

	/* Enable as a PCI bus master, for DMA */
	pci_set_master(pdev);
	pr_info("%s: PCI device enabled as bus master\n", DRIVER_NAME);

	/* Request and map the BARs */
	bar_mask = pci_select_bars(pdev, IORESOURCE_MEM);
	ret = pcim_iomap_regions(pdev, bar_mask, DRIVER_NAME);
	if (ret) {
		pr_err("%s: pcim_iomap_regions failed\n", DRIVER_NAME);
		return -ENOMEM;
	}

	/* Get the virtual addresses for the BARs */
	drvdata->bar0_virt_addr = pcim_iomap_table(pdev)[0];
	drvdata->bar1_virt_addr = pcim_iomap_table(pdev)[1];
	if (!drvdata->bar0_virt_addr || !drvdata->bar1_virt_addr) {
		pr_err("%s: pcim_iomap_region failed\n", DRIVER_NAME);
		return -ENOMEM;
	}

	pr_info("%s: BAR0 mapped to %p, length 0x%llx\n", DRIVER_NAME,
		drvdata->bar0_virt_addr, pci_resource_len(pdev, 0));
	pr_info("%s: BAR1 mapped to %p, length 0x%llx\n", DRIVER_NAME,
		drvdata->bar1_virt_addr, pci_resource_len(pdev, 1));

	/* Enable DMA */
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_err("%s: no suitable DMA support available\n", DRIVER_NAME);
		return ret;
	}
	pr_info("%s: DMA coherent mask is set\n", DRIVER_NAME);

	/* Create DMA pool for a fixed-size allocation */
	drvdata->dma_desc_pool = dma_pool_create("desc_pool", dev,
						 XDMA_DESC_BLOCK_SIZE,
						 XDMA_DESC_BLOCK_ALIGN,
						 XDMA_DESC_BLOCK_BOUNDARY);
	if (!drvdata->dma_desc_pool) {
		pr_err("%s: unable to allocate desciptor pool\n", DRIVER_NAME);
		return -ENOMEM;
	}
	pr_info("%s: DMA pool created with size %zu\n",
		DRIVER_NAME, XDMA_DESC_BLOCK_SIZE);

	/* Allocate a buffer from the DMA pool */
	drvdata->dma_desc_size = XDMA_DESC_BLOCK_SIZE;
	drvdata->dma_desc_virt = dma_pool_alloc(drvdata->dma_desc_pool,
						GFP_KERNEL,
						&drvdata->dma_desc_phys);
	if (!drvdata->dma_desc_virt) {
		pr_err("%s: failed to allocate DMA coherent buffer from pool of size %zu.\n",
		       DRIVER_NAME, drvdata->dma_desc_size);
		dma_pool_destroy(drvdata->dma_desc_pool);
		return -ENOMEM;
	}
	pr_info("%s: DMA pool buffer allocated: virt=%p, phys=0x%llx, size=%zu\n",
		DRIVER_NAME, drvdata->dma_desc_virt,
		(unsigned long long)drvdata->dma_desc_phys,
		drvdata->dma_desc_size);

	/* Allocate DMA coherent buffer for mmap */
	drvdata->dma_buffer_size = DMA_BUFFER_SIZE;
	drvdata->dma_buffer_virt = dma_alloc_coherent(dev,
						      drvdata->dma_buffer_size,
						      &drvdata->dma_buffer_phys,
						      GFP_KERNEL);
	if (!drvdata->dma_buffer_virt) {
		pr_err("%s: failed to allocate DMA coherent buffer for mmap\n",
		       DRIVER_NAME);
		dma_pool_destroy(drvdata->dma_desc_pool);
	}
	memset(drvdata->dma_buffer_virt, 0, drvdata->dma_buffer_size);
	pr_info("%s: Coherent buffer allocated: virt=%p, phys=0x%llx, size=%zu\n",
		DRIVER_NAME, drvdata->dma_buffer_virt,
		(unsigned long long)drvdata->dma_buffer_phys,
		drvdata->dma_buffer_size);

	/* Request the device number */
	ret = alloc_chrdev_region(&luxyd_ai_major, 0, 1, DRIVER_NAME);
	if (ret) {
		dev_err(dev, "failed to allocate major number: %d\n", ret);
		return ret;
	}

	/* Initialize the character device and add it to userspace */
	cdev_init(&drvdata->cdev, &luxyd_ai_fops);
	drvdata->cdev.owner = THIS_MODULE;
	ret = cdev_add(&drvdata->cdev, luxyd_ai_major, 1);
	if (ret) {
		dev_err(dev, "failed to add char device: %d\n", ret);
		goto out_dealloc_region;
	}

	luxyd_ai_class = class_create(DRIVER_NAME);
	if (IS_ERR(luxyd_ai_class)) {
		dev_err(dev, "failed to allocate class\n");
		ret = PTR_ERR(luxyd_ai_class);
		goto out_delete_cdev;
	}

	/* Create device node */
	drvdata->dev = device_create(luxyd_ai_class, dev,
				     luxyd_ai_major, drvdata, DRIVER_NAME);
	if (IS_ERR(drvdata->dev)) {
		dev_err(dev, "failed to create device node\n");
		ret = PTR_ERR(drvdata->dev);
		goto out_destroy_class;
	}

	return 0;

out_destroy_class:
	class_destroy(luxyd_ai_class);

out_delete_cdev:
	cdev_del(&drvdata->cdev);

out_dealloc_region:
	unregister_chrdev_region(luxyd_ai_major, 1);

	dma_free_coherent(dev, drvdata->dma_buffer_size,
			  drvdata->dma_buffer_virt, drvdata->dma_buffer_phys);
	dma_pool_destroy(drvdata->dma_desc_pool);

	return ret;
}

static void luxyd_fpga_remove(struct pci_dev *pdev)
{
	struct luxyd_ai_device *drvdata = pci_get_drvdata(pdev);

	pr_info("%s: removing device 0x%04x:0x%04x\n", DRIVER_NAME,
		pdev->vendor, pdev->device);

	if (!drvdata) {
		pr_err("%s: no private data found\n", DRIVER_NAME);
		return;
	}

	/* Destroy device node */
	device_destroy(luxyd_ai_class, luxyd_ai_major);

	/* Destroy class */
	class_destroy(luxyd_ai_class);

	/* Remove character device */
	cdev_del(&drvdata->cdev);

	/* Unregister device number */
	unregister_chrdev_region(luxyd_ai_major, 1);

	/* DMA cleanup */
	if (drvdata->dma_buffer_size)
		dma_free_coherent(&drvdata->pdev->dev, drvdata->dma_buffer_size,
				  drvdata->dma_buffer_virt,
				  drvdata->dma_buffer_phys);

	dma_pool_free(drvdata->dma_desc_pool, drvdata->dma_desc_virt,
		      drvdata->dma_desc_phys);
	dma_pool_destroy(drvdata->dma_desc_pool);
}

/* PCI driver structure */
static struct pci_driver luxyd_ai_pci_driver = {
	.name		= DRIVER_NAME,
	.id_table	= luxyd_fpga_id_table,
	.probe		= luxyd_fpga_probe,
	.remove		= luxyd_fpga_remove,
};

static int __init luxyd_ai_init(void)
{
	int ret;

	pr_info("%s: version %s loading\n", DRIVER_NAME, DRIVER_VERSION);

	/* Register PCI driver */
	ret = pci_register_driver(&luxyd_ai_pci_driver);
	if (ret) {
		pr_err("%s: failed to register pci driver\n", DRIVER_NAME);
		return ret;
	}

	pr_info("%s: version %s loaded successfully\n",
		DRIVER_NAME, DRIVER_VERSION);

	return 0;
}

static void __exit luxyd_ai_cleanup(void)
{
	pr_info("%s: version %s unloading\n", DRIVER_NAME, DRIVER_VERSION);

	/* Unregister PCI driver */
	pci_unregister_driver(&luxyd_ai_pci_driver);

	pr_info("%s: version %s unloaded successfully\n",
		DRIVER_NAME, DRIVER_VERSION);
}

module_init(luxyd_ai_init);
module_exit(luxyd_ai_cleanup);
