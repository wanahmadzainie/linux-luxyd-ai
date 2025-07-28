// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#include <linux/cdev.h>
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
#define DEVICE_ID			0x7011	/* Kintex-7 Device ID */

/* TODO: Status Control Register */
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

#define DMA_ENGINE_ID_OFFSET		0x0

#define DMA_BUFFER_SIZE			(16 * 1024)

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

#define XDMA_CHAN_STRIDE	0x100
#define XDMA_CHAN_H2C_OFFSET	0x0
#define XDMA_CHAN_C2H_OFFSET	0x1000
#define XDMA_CHAN_H2C_TARGET	0x0
#define XDMA_CHAN_C2H_TARGET	0x1

/* Channel SGDMA registers */
#define XDMA_SGDMA_IDENTIFIER	0x4000
#define XDMA_SGDMA_DESC_LO	0x4080
#define XDMA_SGDMA_DESC_HI	0x4084
#define XDMA_SGDMA_DESC_ADJ	0x4088
#define XDMA_SGDMA_DESC_CREDIT	0x408c

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

	void __iomem *bar0_virt_addr;
	void __iomem *bar1_virt_addr;
	phys_addr_t bar1_phys_addr;
	unsigned long bar1_len;

	struct dma_pool *desc_pool;
	void *dma_buffer_virt;
	dma_addr_t dma_buffer_phys;
	size_t dma_buffer_size;

	struct mutex ioctl_lock;
};

static struct class *luxyd_ai_class;
static int luxyd_ai_major;

/* PCI device ID table */
static const struct pci_device_id luxyd_fpga_id_table[] = {
	//{ PCI_DEVICE(PCI_ANY_ID, PCI_ANY_ID) },
	{ PCI_DEVICE(0x80ee, 0xbeef) },		/* VirtualBox Graphics Adapter */
	{ PCI_DEVICE(0x10ee, 0x7011) },		/* Xilinx Kintex-7 */
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

static long luxyd_ai_unlocked_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	struct luxyd_ai_device *drvdata = filp->private_data;
	struct xdma_desc *mata_desc;
	size_t bytes_to_transfer;
	int ret = 0;
	static struct matrix_size matrix_size;
	u16 *a_matrix, *b_matrix;
	u32 *p_matrix, sum;
	int i, j, k;
	u32 buffer;
	u32 val;

#if 0
	if (mutex_lock_interruptible(&drvdata->ioctl_lock))
		return -ERESTARTSYS;
#endif

	switch (cmd) {
	case LUXYD_AI_STATUS_GET:
		pr_info("%s: ioctl cmd LUXYD_AI_STATUS_GET\n", DRIVER_NAME);
		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_STATUS_OFFSET);

		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			return -EFAULT;

		val = ioread32(drvdata->bar1_virt_addr + 0x0000);
                pr_info("%s: DMA engine ID Read at offset 0x0000 val=0x%08x\n",
			DRIVER_NAME, val);
		val = ioread32(drvdata->bar1_virt_addr + 0x2000);
                pr_info("%s: DMA engine ID Read at offset 0x2000 val=0x%08x\n",
                        DRIVER_NAME, val);
		val = ioread32(drvdata->bar1_virt_addr + 0x4000);
                pr_info("%s: DMA engine ID Read at offset 0x4000 val=0x%08x\n",
                        DRIVER_NAME, val);

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

		if (copy_from_user(&matrix_size,
				   (struct matrix_size __user *)arg,
				   sizeof(struct matrix_size))) {
			ret = -EFAULT;
			break;
		}

		if (matrix_size.m <= 0 ||
		    matrix_size.n <= 0 ||
		    matrix_size.p <= 0) {
			ret = -EINVAL;
			break;
		}

		val = FIELD_PREP(LUXYD_AI_MATA_ROWCOUNT, matrix_size.m) |
			FIELD_PREP(LUXYD_AI_MATA_COLCOUNT, matrix_size.n) |
			FIELD_PREP(LUXYD_AI_MATB_ROWCOUNT, matrix_size.n) |
			FIELD_PREP(LUXYD_AI_MATB_COLCOUNT, matrix_size.p);
		iowrite32(val, drvdata->bar0_virt_addr + LUXYD_AI_INFO_OFFSET);

		val = ioread32(drvdata->bar0_virt_addr + LUXYD_AI_INFO_OFFSET);
		pr_info("%s: readback val=0x%08x\n", DRIVER_NAME, val);

		/* Transfer matrix data */
		buffer = 0xdeadbeef;
		bytes_to_transfer = sizeof(val);

		/* Fill up the descriptor */
		mata_desc = drvdata->dma_buffer_virt;
		mata_desc->control = XDMA_DESC_CONTROL(1, 0x13); // 0xad4b0013
		mata_desc->bytes = cpu_to_le32(bytes_to_transfer);
		mata_desc->src_addr = cpu_to_le64(&buffer);
		mata_desc->dst_addr = cpu_to_le64(0);
		mata_desc->next_desc = cpu_to_le64(0);

		pr_info("%s: mata_desc->control=0x%08x\n", DRIVER_NAME, mata_desc->control);
		pr_info("%s: mata_desc->bytes=0x%08x\n", DRIVER_NAME, mata_desc->bytes);
		pr_info("%s: mata_desc->src_addr=0x%08llx\n", DRIVER_NAME, mata_desc->src_addr);
		pr_info("%s: mata_desc->dst_addr=0x%08llx\n", DRIVER_NAME, mata_desc->dst_addr);
		pr_info("%s: mata_desc->next_desc=0x%08llx\n", DRIVER_NAME, mata_desc->next_desc);

		/* Step 1: Read DMA Engine ID */
		val = ioread32(drvdata->bar1_virt_addr);
		pr_info("%s: Step 1 DMA Engine ID val=0x%08x\n",
			DRIVER_NAME, val);
		val = ioread32(drvdata->bar1_virt_addr + 0x0040);
		pr_info("%s: Step X H2C status=0x%08x\n", DRIVER_NAME, val);

		/* Step 2: Write DMA config register for the descriptor */
		pr_info("%s: Step 2 write DMA config for the descriptor\n",
			DRIVER_NAME);
		val = lower_32_bits(drvdata->dma_buffer_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x4080);
		val = upper_32_bits(drvdata->dma_buffer_phys);
		iowrite32(val, drvdata->bar1_virt_addr + 0x4084);

		/* Step 3: Write DMA config register to start H2C */
		pr_info("%s: Step 3 write DMA config to start H2C\n",
			DRIVER_NAME);
		iowrite32(0x00fffe7f, drvdata->bar1_virt_addr + 0x0004);

		/* Step 4: Read H2C status */
		val = ioread32(drvdata->bar1_virt_addr + 0x0040);
		pr_info("%s: Step 4 H2C status=0x%08x\n", DRIVER_NAME, val);

		/* Step 5: Read H2C descriptor count */
		val = ioread32(drvdata->bar1_virt_addr + 0x0048);
		pr_info("%s: Step 5 H2C descriptor count val=0x%08x\n",
			DRIVER_NAME, val);

		/* Step 6: Read H2C status */
		val = ioread32(drvdata->bar1_virt_addr + 0x0040);
		pr_info("%s: Step 6 H2C status=0x%08x\n", DRIVER_NAME, val);
		break;

		buffer = 0;

		/* Fill up the descriptor */
		mata_desc->control = XDMA_DESC_CONTROL(1, 0x13); // 0xad4b0013
		mata_desc->bytes = cpu_to_le32(bytes_to_transfer);
		mata_desc->src_addr = cpu_to_le64(0);
		mata_desc->dst_addr = cpu_to_le64(&buffer);
		mata_desc->next_desc = cpu_to_le64(0);

		/* Step 1: Write DMA config register for the descriptor */
		val = lower_32_bits(drvdata->dma_buffer_phys);
		pr_info("%s: Step 1 write DMA config for the descriptor\n",
			DRIVER_NAME);
		iowrite32(val, drvdata->bar1_virt_addr + 0x5080);

		/* Step 2: Write DMA config register to start H2C */
		pr_info("%s: Step 2 write DMA config to start C2H\n",
			DRIVER_NAME);
		//iowrite32(0x00fffe7f, drvdata->bar1_virt_addr + 0x1004);

		/* Step 3: Read C2H status */
                val = ioread32(drvdata->bar1_virt_addr + 0x1040);
                pr_info("%s: Step 3 C2H status=0x%08x\n", DRIVER_NAME, val);

		/* Step 4: Read C2H descriptor count */
		val = ioread32(drvdata->bar1_virt_addr + 0x1048);
		pr_info("%s: Step 4 C2H descriptor count val=0x%08x\n",
			DRIVER_NAME, val);

                /* Step 5: Read C2H status */
		val = ioread32(drvdata->bar1_virt_addr + 0x1040);
		pr_info("%s: Step 5 C2H status=0x%08x\n", DRIVER_NAME, val);

		pr_info("%s: buffer=0x%08x\n", DRIVER_NAME, buffer);

		break;

	case LUXYD_AI_MATRIX_MULTIPLY:
		pr_info("%s: ioctl cmd LUXYD_AI_MATRIX_MULTIPLY\n",
			DRIVER_NAME);

		if (!drvdata->bar1_virt_addr) {
			pr_err("%s: BAR1 unknown physical address\n",
			       DRIVER_NAME);
			ret = -EIO;
			break;
		}

		a_matrix = (u16 *)(drvdata->bar1_virt_addr + MATRIXA_OFFSET);
		b_matrix = (u16 *)(drvdata->bar1_virt_addr + MATRIXB_OFFSET);
		p_matrix = (u32 *)(drvdata->bar1_virt_addr + MATRIXP_OFFSET);

		/* Matrix multiplication */
		for (i = 0; i < matrix_size.m; i++) {
			for (j = 0; j < matrix_size.p; j++) {
				sum = 0;
				for (k = 0; k < matrix_size.n; k++) {
					sum += a_matrix[i * matrix_size.n + k] *
						b_matrix[k * matrix_size.p + j];
				}
				p_matrix[i * matrix_size.p + j] = sum;
			}
		}

		pr_info("%s: Matrix P ready\n", DRIVER_NAME);
		break;

	default:
		pr_info("%s: ioctl cmd not supported (%d)\n", DRIVER_NAME, cmd);
		ret = -ENOTTY;
		break;
	}

#if 0
	mutex_unlock(&drvdata->ioctl_lock);
#endif

	return ret;
}

static int luxyd_ai_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct luxyd_ai_device *drvdata = filp->private_data;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn;

	/* Check */
	if (!drvdata->bar1_virt_addr || !drvdata->bar1_phys_addr) {
		pr_err("%s: BAR1 not mapped or unknown physical address\n",
		       DRIVER_NAME);
		return -EFAULT;
	}

	if (offset + size > drvdata->bar1_len) {
		pr_err("%s: mmap request out of bound\n", DRIVER_NAME);
		return -EINVAL;
	}

	/* Get page frame number */
	pfn = (drvdata->bar1_phys_addr + offset) >> PAGE_SHIFT;

	/* Remap physical address to the userspace */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    pfn,
			    size,
			    vma->vm_page_prot)) {
		pr_err("%s: remap_pfn_range() failed\n", DRIVER_NAME);
		return -EAGAIN;
	}

	pr_info("%s: BAR1 (phys 0x%llx, len 0x%lx) mapped to userspace at "
		"0x%lx, len 0x%lx bytes\n",
		DRIVER_NAME, drvdata->bar1_phys_addr, drvdata->bar1_len,
		vma->vm_start, size);

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
	unsigned long bar1_flags;
	int ret;

	pr_info("%s: probing device 0x%04x:0x%04x\n", DRIVER_NAME,
		pdev->vendor, pdev->device);

	/* Allocate private data structure */
	drvdata = devm_kzalloc(dev, sizeof(drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->pdev = pdev;
	pci_set_drvdata(pdev, drvdata);

	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret) {
		pr_err("%s: pcim_enable_device() failed. Aborting.\n",
		       DRIVER_NAME);

		return ret;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	/* Request and map BAR0 - status/control register */
	drvdata->bar0_virt_addr = pcim_iomap_region(pdev, 0, "fpga_pci_bar0");
	if (IS_ERR(drvdata->bar0_virt_addr)) {
		pr_err("%s: pcim_iomap_region() BAR0 failed. Aborting.\n",
		       DRIVER_NAME);
		return PTR_ERR(drvdata->bar0_virt_addr);
	}
	pr_info("%s: BAR0 mapped to %p, length 0x%llx\n", DRIVER_NAME,
		drvdata->bar0_virt_addr, pci_resource_len(pdev, 0));

	/* Request and map BAR1 - on-board memory */
	drvdata->bar1_virt_addr = pcim_iomap_region(pdev, 1, "fpga_pci_bar1");
	if (IS_ERR(drvdata->bar1_virt_addr)) {
		pr_err("%s: pcim_iomap_region() BAR1 failed. Aborting.\n",
		       DRIVER_NAME);
		return PTR_ERR(drvdata->bar1_virt_addr);
	}

	drvdata->bar1_phys_addr = pci_resource_start(pdev, 1);
	drvdata->bar1_len = pci_resource_len(pdev, 1);
	pr_info("%s: BAR1 memory mapped to %p, phys 0x%llx, length 0x%lx\n",
		DRIVER_NAME, drvdata->bar1_virt_addr, drvdata->bar1_phys_addr,
		drvdata->bar1_len);
#else
	/*
	 * FIXME: Real scenario as follows,
	 * BAR0 for status/control register, and
	 * BAR1 for memory
	 *
	 * During testing with VM, only BAR0 available, for memory.
	 */
	ret = pcim_iomap_regions(pdev, BIT(0), DRIVER_NAME);
	if (ret) {
		pr_err("%s: pcim_iomap_regions() failed. Aborting. \n",
		       DRIVER_NAME);
		return -ENOMEM;
	}

	drvdata->bar1_virt_addr = pcim_iomap_table(pdev)[0];
	drvdata->bar1_phys_addr = pci_resource_start(pdev, 0);
	drvdata->bar1_len = pci_resource_len(pdev, 0);
	pr_info("%s: BAR1 memory mapped to 0x%p, phys 0x%llx, length 0x%lx\n",
		DRIVER_NAME, drvdata->bar1_virt_addr, drvdata->bar1_phys_addr,
		drvdata->bar1_len);
#endif

	bar1_flags = pci_resource_flags(pdev, 1);
	if (!(bar1_flags & IORESOURCE_MEM)) {
		pr_warn("%s: BAR1 is not a memory region.\n", DRIVER_NAME);
		drvdata->bar1_virt_addr = NULL;
	} else if (bar1_flags & IORESOURCE_PREFETCH) {
		pr_warn("%s: BAR1 is prefetchable memory.\n", DRIVER_NAME);
	} else {
		pr_info("%s: BAR1 is non-prefetchable memory.\n", DRIVER_NAME);
	}

	/* Enable DMA */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34));
	if (ret) {
		pr_err("%s: No suitable DMA support available.\n", DRIVER_NAME);
		//pci_release_regions(pdev);
		return ret;
	}
	pr_info("%s: DMA mask set.\n", DRIVER_NAME);

	/* Create DMA pool for a fixed-size allocation */
	drvdata->desc_pool = dma_pool_create("desc_pool", dev,
					     XDMA_DESC_BLOCK_SIZE,
					     XDMA_DESC_BLOCK_ALIGN,
					     XDMA_DESC_BLOCK_BOUNDARY);
	if (!drvdata->desc_pool) {
		pr_err("%s: unable to allocate descriptor pool\n", DRIVER_NAME);
		return -ENOMEM;
	}
	pr_info("%s: DMA pool created for buffer size %zu\n",
		DRIVER_NAME, XDMA_DESC_BLOCK_SIZE);

	/* Allocate a buffer from the DMA pool */
	drvdata->dma_buffer_size = XDMA_DESC_BLOCK_SIZE;
	drvdata->dma_buffer_virt = dma_pool_alloc(drvdata->desc_pool,
						  GFP_KERNEL,
						  &drvdata->dma_buffer_phys);
	if (!drvdata->dma_buffer_virt) {
		pr_err("%s: Failed to allocated DMA coherent buffer from pool of size %zu.\n",
		       DRIVER_NAME, drvdata->dma_buffer_size);
		dma_pool_destroy(drvdata->desc_pool);
		return -ENOMEM;
	}
	pr_info("%s: DMA pool buffer allocated: virt=%p, phys=0x%llx, size=%zu\n",
		DRIVER_NAME, drvdata->dma_buffer_virt,
		(unsigned long long)drvdata->dma_buffer_phys,
		drvdata->dma_buffer_size);

	/* Initialize DMA buffer */
	memset(drvdata->dma_buffer_virt, 0, drvdata->dma_buffer_size);

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

	/* Free DMA buffer */
	if (drvdata->dma_buffer_virt) {
		dma_free_coherent(&pdev->dev, drvdata->dma_buffer_size,
				  drvdata->dma_buffer_virt,
				  drvdata->dma_buffer_phys);
	}
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
