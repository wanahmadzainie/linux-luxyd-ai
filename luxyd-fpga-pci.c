// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Luxyd Technologies
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "luxyd-ioctl.h"

#define DEVICE_NAME	"luxyd_fpga"
#define DRIVER_NAME	"luxyd-fpga-pci"
#define DRIVER_VERSION	"0.1"

#define DMA_SIZE_MAX	(512 * 1024 * 1024)

struct fpga_device {
	struct pci_dev *pdev;

	struct cdev cdev;
	dev_t dev_node;
	struct class *class;
	struct device *device;

	void *dma_buf_virt;
	dma_addr_t dma_buf_phys;
	size_t dma_buf_size;

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

static __maybe_unused int
fpga_do_dma(struct fpga_device *priv, void *data, size_t len, int dir)
{
	if (len > priv->dma_buf_size)
		return -ENOMEM;

	/* Dummy write - copy data to DMA buffer */
	if (dir == DMA_TO_DEVICE)
		memcpy(priv->dma_buf_virt, data, len);

	/* Dummy read - copy data from DMA buffer */
	if (dir == DMA_FROM_DEVICE)
		memcpy(data, priv->dma_buf_virt, len);

	return len;
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
	size_t size_a, size_b;

	if (!priv->config_set) {
		pr_err("attempt to read before config\n");
		return -EINVAL;
	}

	size_a = priv->config.m * priv->config.n;
	size_b = priv->config.n * priv->config.p;

	if (count > priv->dma_buf_size) {
		dev_err(&priv->pdev->dev, "read size %zu exceeds limit %ld\n",
			count, priv->dma_buf_size);
		return -EINVAL;
	}

	if (copy_to_user(buf, priv->dma_buf_virt + size_a + size_b, count)) {
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
	size_t size_a, size_b;
	size_t size_expected;

	if (!priv->config_set) {
		pr_err("attempt to write before config\n");
		return -EINVAL;
	}

	size_a = priv->config.m * priv->config.n;
	size_b = priv->config.n * priv->config.p;
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

	priv->write_pos += count;

	if (priv->write_pos >= size_expected) {
		do_matrix_multiplication(priv->dma_buf_virt,
					 priv->dma_buf_virt + size_a,
					 priv->dma_buf_virt + size_a + size_b,
					 priv->config.m, priv->config.n,
					 priv->config.p);

		priv->write_pos = 0;
	}

	pr_info("device write completed\n");
	return count;
}

static long
fpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpga_device *priv = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case LUXYD_IOCTL_MATMUL:
		if (copy_from_user(&priv->config, argp, sizeof(matrix_config)))
			return -EFAULT;

		priv->write_pos = 0;
		priv->config_set = true;

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
	int ret;

	pr_info("probing device 0x%04x:0x%04x\n", pdev->vendor, pdev->device);

	/* Allocate private data structure */
	priv = devm_kzalloc(dev, sizeof(priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;
	pci_set_drvdata(pdev, priv);

	/* Enable DMA */
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		pr_err("no suitable DMA support available\n");
		return ret;
	}

	pr_info("DMA coherent mask is set.\n");

	priv->dma_buf_size = DMA_SIZE_MAX;
	priv->dma_buf_virt = dmam_alloc_coherent(&pdev->dev, priv->dma_buf_size,
						 &priv->dma_buf_phys,
						 GFP_KERNEL);
	if (!priv->dma_buf_virt) {
		pr_err("failed to allocate DMA coherent buffer\n");
		return -ENOMEM;
	}

	memset(priv->dma_buf_virt, 0, DMA_SIZE_MAX);

	pr_info("DMA buffer allocated at virt=%p, phys=%pad, size=%zu\n",
		priv->dma_buf_virt, &priv->dma_buf_phys, priv->dma_buf_size);

	ret = alloc_chrdev_region(&priv->dev_node, 0, 1, DEVICE_NAME);
	if (ret) {
		pr_err("failed to allocate chrdev region\n");
		return ret;
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
