// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Luxyd Technologies
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/bitfield.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "luxyd-ioctl.h"

#define DEVICE_NAME	"luxyd_fpga"
#define DRIVER_NAME	"luxyd-fpga-pci"
#define DRIVER_VERSION	"0.2"

/* Luxyd FPGA Status Control Register */
#define PCIE_BAR0_BASE_ADDR		0x000c0000
#define PCIE_GGML_CTRL			(PCIE_BAR0_BASE_ADDR + 0x00)
#define PCIE_GGML_STATUS		(PCIE_BAR0_BASE_ADDR + 0x00)
#define PCIE_GGML_INIT_LUT_LOW		(PCIE_BAR0_BASE_ADDR + 0x08)
#define PCIE_GGML_INIT_LUT_HIGH		(PCIE_BAR0_BASE_ADDR + 0x0C)
#define PCIE_GGML_PROC_LUT_LOW		(PCIE_BAR0_BASE_ADDR + 0x10)
#define PCIE_GGML_PROC_LUT_HIGH		(PCIE_BAR0_BASE_ADDR + 0x14)
#define PCIE_GGML_PROC_S_LOW		(PCIE_BAR0_BASE_ADDR + 0x18)
#define PCIE_GGML_PROC_S_HIGH		(PCIE_BAR0_BASE_ADDR + 0x1C)
#define PCIE_GGML_PROC_VX_LOW		(PCIE_BAR0_BASE_ADDR + 0x20)
#define PCIE_GGML_PROC_VX_HIGH		(PCIE_BAR0_BASE_ADDR + 0x24)
#define PCIE_GGML_PROC_VY_LOW		(PCIE_BAR0_BASE_ADDR + 0x28)
#define PCIE_GGML_PROC_VY_HIGH		(PCIE_BAR0_BASE_ADDR + 0x2c)
#define PCIE_GGML_PROC_n		(PCIE_BAR0_BASE_ADDR + 0x30)
#define PCIE_GGML1_bs_LOW		(PCIE_BAR0_BASE_ADDR + 0x34)
#define PCIE_GGML1_bs_HIGH		(PCIE_BAR0_BASE_ADDR + 0x38)
#define PCIE_GGML1_nr			(PCIE_BAR0_BASE_ADDR + 0x3C)
#define PCIE_GGML1_nc			(PCIE_BAR0_BASE_ADDR + 0x40)

#define CMD_GGML_INIT			BIT(8)
#define CMD_GGML_PROC			BIT(9)

#define STS_DDR_INIT			BIT(8)
#define STS_FPGA_READY			BIT(9)
#define STS_FPGA_TIMEOUT		BIT(10)
#define STS_GGML_INIT			BIT(11)
#define STS_GGML_PROC			BIT(12)

#define LUXYD_FPGA_CMD_OFFSET		0xc0000
#define LUXYD_FPGA_CMD_START		BIT(8)

#define LUXYD_FPGA_INFO_OFFSET		0xc0004
#define LUXYD_FPGA_MATA_ROWCOUNT	GENMASK(7, 0)
#define LUXYD_FPGA_MATA_COLCOUNT	GENMASK(15, 8)
#define LUXYD_FPGA_MATB_ROWCOUNT	GENMASK(23, 16)
#define LUXYD_FPGA_MATB_COLCOUNT	GENMASK(31, 24)

#define LUXYD_FPGA_STATUS_OFFSET	0xc0008
#define LUXYD_FPGA_STATUS_READY		BIT(8)

#define CMD_SIGNATURE			(0xac)
#define STATUS_SIGNATURE		(0xfc)

#define DMA_SIZE_MAX			(512 * 1024 * 1024)

/* Luxyd FPGA matrix location offset */
#define MAT_A_OFFSET			0x0000
#define MAT_B_OFFSET			0x2000
#define MAT_P_OFFSET			0x4000

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

/* private data stucture */
struct fpga_device {
	struct pci_dev *pdev;

	struct cdev cdev;
	dev_t dev_node;
	struct class *class;
	struct device *device;

	/* BARs addresses */
	void __iomem *bar0_virt_addr;
	void __iomem *bar1_virt_addr;

	/* XDMA descriptor pool */
	struct dma_pool *dma_desc_pool;
	void *dma_desc_virt;
	dma_addr_t dma_desc_phys;
	size_t dma_desc_size;

	/* DMA coherent buffer */
	void *dma_buf_virt;
	dma_addr_t dma_buf_phys;
	size_t dma_buf_size;

	/* matrix configuration */
	matrix_config config;
	bool config_set;
	size_t write_pos;
};

static __maybe_unused void
do_matrix_multiplication(u8 *a, u8 *b, u32 *c, size_t m, size_t n, size_t p)
{
	size_t i, j, k;

	for (i = 0; i < m; i++) {
		for (j = 0; j < p; j++) {
			c[i * p + j] = 0;
			for (k = 0; k < n; k++) {
				c[i * p + j] += a[i * n + k] * b[k * p + j];
			}
		}
	}

	pr_info("completed\n");
}

static __maybe_unused void
fpga_dump_regs(struct fpga_device *priv)
{
	u32 val;

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_CTRL);
	pr_info("REG_READ(PCIE_GGML_CTRL)          0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_STATUS);
	pr_info("REG_READ(PCIE_GGML_STATUS)        0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_INIT_LUT_LOW);
	pr_info("REG_READ(PCIE_GGML_INIT_LUT_LOW)  0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_INIT_LUT_HIGH);
	pr_info("REG_READ(PCIE_GGML_INIT_LUT_HIGH) 0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_LUT_LOW);
	pr_info("REG_READ(PCIE_GGML_PROC_LUT_LOW)  0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_LUT_HIGH);
	pr_info("REG_READ(PCIE_GGML_PROC_LUT_HIGH) 0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_S_LOW);
	pr_info("REG_READ(PCIE_GGML_PROC_S_LOW)    0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_S_HIGH);
	pr_info("REG_READ(PCIE_GGML_PROC_S_HIGH)   0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_VX_LOW);
	pr_info("REG_READ(PCIE_GGML_PROC_VX_LOW)   0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_VX_HIGH);
	pr_info("REG_READ(PCIE_GGML_PROC_VX_HIGH)  0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_VY_LOW);
	pr_info("REG_READ(PCIE_GGML_PROC_VY_LOW)   0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_VY_HIGH);
	pr_info("REG_READ(PCIE_GGML_PROC_VY_HIGH)  0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML_PROC_n);
	pr_info("REG_READ(PCIE_GGML_PROC_n)        0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML1_bs_LOW);
	pr_info("REG_READ(PCIE_GGML1_bs_LOW)       0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML1_bs_HIGH);
	pr_info("REG_READ(PCIE_GGML1_bs_HIGH)      0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML1_nr);
	pr_info("REG_READ(PCIE_GGML1_nr)           0x%08x\n", val);

	val = ioread32(priv->bar0_virt_addr + PCIE_GGML1_nc);
	pr_info("REG_READ(PCIE_GGML1_nc)           0x%08x\n", val);
}

static __maybe_unused int
fpga_do_dma(struct fpga_device *priv, size_t len, u64 src_addr, u64 dst_addr,
	    int dir)
{
	struct xdma_desc *dma_desc;
	int timeout;
	u32 val;

	/* fill XDMA descriptor */
	len = ALIGN(len, 0x1000);
	dma_desc = priv->dma_desc_virt;
	dma_desc->control = XDMA_DESC_CONTROL(1, 0x13); // 0xad4b0013
	dma_desc->bytes = cpu_to_le32(len);
	dma_desc->src_addr = cpu_to_le64(src_addr);
	dma_desc->dst_addr = cpu_to_le64(dst_addr);
	dma_desc->next_desc = cpu_to_le64(0);

	if (dir == DMA_MEM_TO_DEV) {		/* H2C */
		val = ioread32(priv->bar1_virt_addr);
		pr_info("H2C DMA Engine ID (0x%08x)\n", val);

		pr_info("DMA descriptor address   0x%p\n", dma_desc);
		pr_info("DMA descriptor: control =0x%08x\n", dma_desc->control);
		pr_info("DMA descriptor: bytes   =0x%08x\n", dma_desc->bytes);
		pr_info("DMA descriptor: src_addr=0x%016llx\n", dma_desc->src_addr);
		pr_info("DMA descriptor: dst_addr=0x%016llx\n", dma_desc->dst_addr);

		/* write first XDMA descriptor */
		val = lower_32_bits(priv->dma_desc_phys);
		iowrite32(val, priv->bar1_virt_addr + 0x4080);
		pr_info("H2C: REG_WRITE(0x4080) 0x%08x\n", val);
		val = upper_32_bits(priv->dma_desc_phys);
		iowrite32(val, priv->bar1_virt_addr + 0x4084);
		pr_info("H2C: REG_WRITE(0x4084) 0x%08x\n", val);

		/* kick off H2C DMA transfer */
		iowrite32(0x00fffe7f, priv->bar1_virt_addr + 0x0004);
		val = ioread32(priv->bar1_virt_addr + 0x0004);
		pr_info("H2C: REG_WRITE(0x0004) 0x%08x\n", val);
		pr_info("H2C: kick off DMA transfer\n");

		/* poll H2C status */
		for (timeout = 100; timeout > 0; timeout--) {
			val = ioread32(priv->bar1_virt_addr + 0x0040);
			pr_info("H2C: REG_READ(0x0040) 0x%08x\n", val);
			if ((val & BIT(0)) == 0x0)
				break;

			udelay(10);
		}

		if (timeout == 0)
                        pr_err("H2C: DMA transfer timeout\n");

		/* read H2C descriptor count */
		val = ioread32(priv->bar1_virt_addr + 0x0048);
		pr_info("H2C: REG_READ(0x0048) 0x%08x\n", val);
		if (!val)
			pr_err("H2C: no XDMA descriptor found\n");
		else
			pr_info("H2C: XDMA descriptor found (%d)\n", val);

		/* read H2C status */
		val = ioread32(priv->bar1_virt_addr + 0x0040);
		pr_info("H2C: REG_READ(0x0040) 0x%08x\n", val);
		if (val == 0x6)
			pr_info("H2C: DMA transfer completed\n");
		else
			pr_err("H2C: DMA transfer failed\n");

		/* stop DMA transfer */
		iowrite32(0x0, priv->bar1_virt_addr + 0x0004);
		val = ioread32(priv->bar1_virt_addr + 0x0004);
		pr_info("H2C: REG_WRITE(0x0004) 0x%08x\n", val);
		pr_info("H2C: DMA transfer stopped\n");
	} else if (dir == DMA_DEV_TO_MEM) {	/* C2H */
		val = ioread32(priv->bar1_virt_addr + 0x1000);
                pr_info("C2H DMA Engine ID (0x%08x)\n", val);

		pr_info("DMA descriptor address   0x%p\n", dma_desc);
		pr_info("DMA descriptor: control =0x%08x\n", dma_desc->control);
		pr_info("DMA descriptor: bytes   =0x%08x\n", dma_desc->bytes);
		pr_info("DMA descriptor: src_addr=0x%016llx\n", dma_desc->src_addr);
		pr_info("DMA descriptor: dst_addr=0x%016llx\n", dma_desc->dst_addr);

		/* write first XDMA descriptor */
		val = lower_32_bits(priv->dma_desc_phys);
		iowrite32(val, priv->bar1_virt_addr + 0x5080);
		pr_info("C2H: REG_WRITE(0x5080) 0x%08x\n", val);
		val = upper_32_bits(priv->dma_desc_phys);
		iowrite32(val, priv->bar1_virt_addr + 0x5084);
		pr_info("C2H: REG_WRITE(0x5084) 0x%08x\n", val);

		/* kick off C2H DMA transfer */
		iowrite32(0x00fffe7f, priv->bar1_virt_addr + 0x1004);
		val = ioread32(priv->bar1_virt_addr + 0x1004);
		pr_info("C2H: REG_WRITE(0x1004) 0x%08x\n", val);
		pr_info("C2H: kick off DMA transfer\n");

		/* poll C2H status */
		for (timeout = 100; timeout > 0; timeout--) {
			val = ioread32(priv->bar1_virt_addr + 0x1040);
			pr_info("C2H: REG_READ(0x1040) 0x%08x\n", val);
			if ((val & BIT(0)) == 0x0)
				break;

			udelay(10);
		}

		if (timeout == 0)
                        pr_err("C2H: DMA transfer timeout\n");

		/* read C2H descriptor count */
		val = ioread32(priv->bar1_virt_addr + 0x1048);
		pr_info("C2H: REG_READ(0x1048) 0x%08x\n", val);
		if (!val)
			pr_err("C2H: no XDMA descriptor found\n");
		else
			pr_info("C2H: DMA descriptor found (%d)\n", val);

		/* read C2H status */
		val = ioread32(priv->bar1_virt_addr + 0x1040);
		pr_info("C2H: REG_READ(0x1040) 0x%08x\n", val);
		if (val == 0x6)
			pr_info("C2H: DMA transfer completed\n");
		else
			pr_err("C2H: DMA transfer failed\n");

		/* stop DMA transfer */
		iowrite32(0x0, priv->bar1_virt_addr + 0x1004);
		val = ioread32(priv->bar1_virt_addr + 0x1004);
		pr_info("C2H: REG_WRITE(0x1004) 0x%08x\n", val);
                pr_info("C2H: DMA transfer stopped\n");
	} else {
		pr_err("invalid direction\n");
		return -EINVAL;
	}

	udelay(10);

	return 0;
}

static __maybe_unused int
fpga_do_matrix_multiplication(struct fpga_device *priv)
{
	int timeout;
	u32 val;

	/* Read status control registers */
	val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_CMD_OFFSET);
	pr_info("REG_READ(CMD_REG)    0x%08x\n", val);
	val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_INFO_OFFSET);
	pr_info("REG_READ(INFO_REG)   0x%08x\n", val);
	val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_STATUS_OFFSET);
	pr_info("REG_READ(STATUS_REG) 0x%08x\n", val);

	/* Send command to start operation */
	val = LUXYD_FPGA_CMD_START | CMD_SIGNATURE;
	iowrite32(val, priv->bar0_virt_addr + LUXYD_FPGA_CMD_OFFSET);
	pr_info("REG_WRITE(CMD_REG)   0x%08x\n", val);

	for (timeout = 100; timeout > 0; timeout--) {
		val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_STATUS_OFFSET);
		pr_info("REG_READ(STATUS_REG) 0x%08x\n", val);

		if ((val & BIT(8)) == BIT(8))
			 break;

		udelay(10);
	}

	/* Send command to stop operation */
	val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_CMD_OFFSET);
	pr_info("REG_READ(CMD_REG)    0x%08x\n", val);
	val = CMD_SIGNATURE;
	iowrite32(val, priv->bar0_virt_addr + LUXYD_FPGA_CMD_OFFSET);
	pr_info("REG_WRITE(CMD_REG)   0x%08x\n", val);

	if (timeout == 0) {
		pr_err("matrix multiplication timeout\n");
		return -EFAULT;
	}

	return 0;
}

static int
fpga_open(struct inode *inode, struct file *file)
{
	struct fpga_device *priv;

	priv = container_of(inode->i_cdev, struct fpga_device, cdev);
	file->private_data = priv;

	priv->write_pos = 0;
	priv->config_set = false;

	pr_info("device opened\n");
	return 0;
}

static int
fpga_release(struct inode *inode, struct file *file)
{
	pr_info("device released\n");
	return 0;
}

static ssize_t
fpga_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct fpga_device *priv = file->private_data;
	size_t bytes_to_transfer;
	size_t size_a, size_b;

	if (!priv->config_set) {
		pr_err("attempt to read before config\n");
		return -EINVAL;
	}

	size_a = priv->config.m * priv->config.n * sizeof(u16);
	size_b = priv->config.n * priv->config.p * sizeof(u16);

	if (count > priv->dma_buf_size) {
		dev_err(&priv->pdev->dev, "read size %zu exceeds limit %ld\n",
			count, priv->dma_buf_size);
		return -EINVAL;
	}

	/* C2H DMA transfer matrix P */
	bytes_to_transfer = priv->config.m * priv->config.p * sizeof(u16);
	fpga_do_dma(priv, bytes_to_transfer,
		    MAT_P_OFFSET,
		    priv->dma_buf_phys + MAT_P_OFFSET,
		    DMA_DEV_TO_MEM);

	if (copy_to_user(buf, priv->dma_buf_virt + MAT_P_OFFSET, count)) {
		dev_err(&priv->pdev->dev, "failed to copy_to_user in read\n");
		return -EFAULT;
	}

	pr_info("device read completed\n");
	return count;
}

static ssize_t
fpga_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct fpga_device *priv = file->private_data;
	size_t bytes_to_transfer;
	size_t size_a, size_b;
	size_t size_expected;

	if (!priv->config_set) {
		pr_err("attempt to write before config\n");
		return -EINVAL;
	}

	size_a = priv->config.m * priv->config.n * sizeof(u16);
	size_b = priv->config.n * priv->config.p * sizeof(u16);
	size_expected = size_a + size_b;

	if (priv->write_pos + count > priv->dma_buf_size) {
		dev_err(&priv->pdev->dev, "write size %zu exceeds limit %ld\n",
			count, priv->dma_buf_size);
		return -EINVAL;
	}

	if (copy_from_user(priv->dma_buf_virt + priv->write_pos, buf, count)) {
		dev_err(&priv->pdev->dev, "failed to copy_from_user in write\n");
		return -EFAULT;
	}

	if (priv->write_pos >= size_expected) {
		/* H2C DMA transfer matrix A */
		bytes_to_transfer = priv->config.m * priv->config.n * sizeof(u16);
                fpga_do_dma(priv, bytes_to_transfer,
                                  priv->dma_buf_phys + MAT_A_OFFSET,
                                  MAT_A_OFFSET,
                                  DMA_MEM_TO_DEV);

		/* H2C DMA transfer matrix B */
		bytes_to_transfer = priv->config.n * priv->config.p * sizeof(u16);
                fpga_do_dma(priv, bytes_to_transfer,
                                  priv->dma_buf_phys + MAT_B_OFFSET,
                                  MAT_B_OFFSET,
                                  DMA_MEM_TO_DEV);

		fpga_do_matrix_multiplication(priv);
		//do_matrix_multiplication(priv->dma_buf_virt,
		//			 priv->dma_buf_virt + size_a,
		//			 priv->dma_buf_virt + size_a + size_b,
		//			 priv->config.m, priv->config.n,
		//			 priv->config.p);

		priv->write_pos = 0;
	}

	priv->write_pos += MAT_B_OFFSET;

	pr_info("device write completed\n");
	return count;
}

static long
fpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpga_device *priv = file->private_data;
	void __user *argp = (void __user *)arg;
	u32 val;

	switch (cmd) {
	case LUXYD_IOCTL_MATMUL:
		if (copy_from_user(&priv->config, argp, sizeof(matrix_config)))
			return -EFAULT;

		priv->write_pos = 0;
		priv->config_set = true;

		/* program Luxyd FPGA - send matrix information */
		val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_INFO_OFFSET);
		pr_info("REG_READ(INFO_REG) 0x%08x\n", val);

		val = FIELD_PREP(LUXYD_FPGA_MATA_ROWCOUNT, priv->config.m) |
			FIELD_PREP(LUXYD_FPGA_MATA_COLCOUNT, priv->config.n) |
			FIELD_PREP(LUXYD_FPGA_MATB_ROWCOUNT, priv->config.n) |
			FIELD_PREP(LUXYD_FPGA_MATB_COLCOUNT, priv->config.p);
		iowrite32(val, priv->bar0_virt_addr + LUXYD_FPGA_INFO_OFFSET);
		pr_info("REG_WRITE(INFO_REG) 0x%08x\n", val);

		val = ioread32(priv->bar0_virt_addr + LUXYD_FPGA_INFO_OFFSET);
		pr_info("REG_READ(INFO_REG) 0x%08x\n", val);
		break;

	case LUXYD_IOCTL_GEMV:
		fpga_dump_regs(priv);

		/* set ggml_init lut address 31-0 */
		val = 0x80000000;
		iowrite32(val, priv->bar0_virt_addr + PCIE_GGML_INIT_LUT_LOW);
		pr_info("REG_WRITE(PCIE_GGML_INIT_LUT_LOW)  0x%08x\n", val);

		/* set ggml_init lut address 63-0 */
		val = 0x0;
		iowrite32(val, priv->bar0_virt_addr + PCIE_GGML_INIT_LUT_HIGH);
		pr_info("REG_WRITE(PCIE_GGML_INIT_LUT_HIGH) 0x%08x\n", val);

		/* set ggml_init start */
		val = CMD_GGML_INIT | CMD_SIGNATURE;
		iowrite32(val, priv->bar0_virt_addr + PCIE_GGML_CTRL);
		pr_info("REG_WRITE(PCIE_GGML_CTRL)          0x%08x\n", val);

		udelay(100);

		/* get ggml_init ready */
		val = ioread32(priv->bar0_virt_addr + PCIE_GGML_STATUS);
		pr_info("REG_READ(PCIE_GGML_STATUS)         0x%08x\n", val);
		break;

	default:
		return -EINVAL;
	}

	pr_info("device ioctl completed\n");
	return 0;
}

static const struct file_operations fpga_fops = {
	.owner		= THIS_MODULE,
	.open		= fpga_open,
	.release	= fpga_release,
	.read		= fpga_read,
	.write		= fpga_write,
	.unlocked_ioctl	= fpga_ioctl,
};

static int
fpga_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct fpga_device *priv;
	int bar_mask;
	int ret;

	pr_info("probing device 0x%04x:0x%04x\n", pdev->vendor, pdev->device);

	/* Allocate private data structure */
	priv = devm_kzalloc(dev, sizeof(priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;
	pci_set_drvdata(pdev, priv);

	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret) {
		pr_err("pcim_enable_device failed\n");
		return ret;
	}

	/* Enable as a PCI bus master, for DMA */
	pci_set_master(pdev);
	pr_info("PCI device enabled as bus master\n");

	/* Request and map the BARs */
	bar_mask = pci_select_bars(pdev, IORESOURCE_MEM);
	ret = pcim_iomap_regions(pdev, bar_mask, DRIVER_NAME);
	if (ret) {
		pr_err("pcim_iomap_regions failed\n");
		return -ENOMEM;
	}

	/* Get the virtual addresses for the BARs */
	priv->bar0_virt_addr = pcim_iomap_table(pdev)[0];
	priv->bar1_virt_addr = pcim_iomap_table(pdev)[1];
	if (!priv->bar0_virt_addr || !priv->bar1_virt_addr) {
		pr_err("pcim_iomap_region failed\n");
		return -ENOMEM;
	}

	pr_info("BAR0 mapped to %p, length 0x%llx\n", priv->bar0_virt_addr,
		pci_resource_len(pdev, 0));
	pr_info("BAR1 mapped to %p, length 0x%llx\n", priv->bar1_virt_addr,
		pci_resource_len(pdev, 1));

	/* Enable DMA */
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_err("no suitable DMA support available\n");
		return ret;
	}

	pr_info("DMA coherent mask is set.\n");

	/* Create DMA pool */
	priv->dma_desc_pool = dma_pool_create("desc_pool", dev,
					      XDMA_DESC_BLOCK_SIZE,
					      XDMA_DESC_BLOCK_ALIGN,
					      XDMA_DESC_BLOCK_BOUNDARY);
	if (!priv->dma_desc_pool) {
		pr_err("unable to allocate DMA desciptor pool\n");
		return -ENOMEM;
	}
	pr_info("DMA pool created with size %zu\n", XDMA_DESC_BLOCK_SIZE);

	/* Allocate a buffer from the DMA pool */
	priv->dma_desc_size = XDMA_DESC_BLOCK_SIZE;
	priv->dma_desc_virt = dma_pool_alloc(priv->dma_desc_pool, GFP_KERNEL,
					     &priv->dma_desc_phys);
	if (!priv->dma_desc_virt) {
		pr_err("failed to allocate DMA coherent buffer from pool of size %zu.\n",
		       priv->dma_desc_size);
		ret = -ENOMEM;
		goto out_destroy_dma_pool;
	}
	pr_info("DMA pool buffer allocated: virt=%p, phys=0x%llx, size=%zu\n",
		priv->dma_desc_virt, (unsigned long long)priv->dma_desc_phys,
		priv->dma_desc_size);

	/* Allocate DMA coherent buffer */
	priv->dma_buf_size = DMA_SIZE_MAX;
	priv->dma_buf_virt = dmam_alloc_coherent(&pdev->dev, priv->dma_buf_size,
						 &priv->dma_buf_phys,
						 GFP_KERNEL);
	if (!priv->dma_buf_virt) {
		pr_err("failed to allocate DMA coherent buffer\n");
		ret = -ENOMEM;
		goto out_free_dma_pool;
	}

	memset(priv->dma_buf_virt, 0, DMA_SIZE_MAX);
	pr_info("DMA buffer allocated at virt=%p, phys=%pad, size=%zu\n",
		priv->dma_buf_virt, &priv->dma_buf_phys, priv->dma_buf_size);

	ret = alloc_chrdev_region(&priv->dev_node, 0, 1, DEVICE_NAME);
	if (ret) {
		pr_err("failed to allocate chrdev region\n");
		goto out_free_dma_pool;
	}

	cdev_init(&priv->cdev, &fpga_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->dev_node, 1);
	if (ret) {
		pr_err("failed to add cdev\n");
		goto out_unregister_chrdev;
	}

	priv->class = class_create(DEVICE_NAME);
	if (IS_ERR(priv->class)) {
		pr_err("failed to allocate class\n");
		ret = PTR_ERR(priv->class);
		goto out_delete_cdev;
        }

	priv->device = device_create(priv->class, NULL, priv->dev_node,  NULL,
				     DEVICE_NAME);
	if (IS_ERR(priv->device)) {
		pr_err("failed to create device\n");
		ret = PTR_ERR(priv->device);
		goto out_unroll_device;
	}

	dev_info(dev, "probed\n");
	return 0;

out_unroll_device:
	class_destroy(priv->class);

out_delete_cdev:
	cdev_del(&priv->cdev);

out_unregister_chrdev:
	unregister_chrdev_region(priv->dev_node, 1);

out_free_dma_pool:
	dma_pool_free(priv->dma_desc_pool, priv->dma_desc_virt,
		      priv->dma_desc_phys);

out_destroy_dma_pool:
	dma_pool_destroy(priv->dma_desc_pool);

	return ret;
}

static void
fpga_remove(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpga_device *priv;

	pr_info("removing device 0x%04x:0x%04x\n", pdev->vendor, pdev->device);

	priv = pci_get_drvdata(pdev);
	if (priv) {
		dma_pool_free(priv->dma_desc_pool, priv->dma_desc_virt,
			      priv->dma_desc_phys);
		dma_pool_destroy(priv->dma_desc_pool);

		if (priv->device)
			device_destroy(priv->class, priv->dev_node);

		if (priv->class && !IS_ERR(priv->class))
			class_destroy(priv->class);

		cdev_del(&priv->cdev);
		unregister_chrdev_region(priv->dev_node, 1);
	}

	dev_info(dev, "removed\n");
}

static const struct pci_device_id fpga_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_XILINX, 0x7011) },	/* Luxyd - Xilinx Kintex-7 */
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, fpga_ids);

static struct pci_driver luxyd_driver = {
	.name		= DRIVER_NAME,
	.id_table	= fpga_ids,
	.probe		= fpga_probe,
	.remove		= fpga_remove,
};

module_pci_driver(luxyd_driver);

MODULE_AUTHOR("Wan Ahmad Zainie <wanahmadzainie@gmail.com>");
MODULE_DESCRIPTION("PCIe driver for LUXYD FPGA card");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
