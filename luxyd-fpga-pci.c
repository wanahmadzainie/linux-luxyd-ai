// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>

#define DEVICE_NAME	"luxyd_fpga"
#define DRIVER_NAME	"luxyd-fpga-pci"
#define DRIVER_VERSION	"0.1"

#define DMA_SIZE_MAX	(4 * 1024 * 1024)

struct fpga_device {
	struct pci_dev *pdev;

	struct cdev cdev;
	dev_t dev_node;
	struct class *class;
	struct device *device;

	void *dma_buf_virt;
	dma_addr_t dma_buf_phys;
	size_t dma_buf_size;
};

static int
fpga_open(struct inode *inode, struct file *file)
{
	struct fpga_device *priv;

	priv = container_of(inode->i_cdev, struct fpga_device, cdev);
	file->private_data = priv;

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
fpga_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	pr_info("device write\n");
	return 0;
}

static ssize_t
fpga_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	pr_info("device read\n");
	return 0;
}

static long
fpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	pr_info("device ioctl\n");
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

	priv->dma_buf_size = DMA_SIZE_MAX;
	priv->dma_buf_virt = dmam_alloc_coherent(&pdev->dev, priv->dma_buf_size,
						 &priv->dma_buf_phys,
						 GFP_KERNEL);
	if (!priv->dma_buf_virt) {
		pr_err("failed to allocate DMA coherent buffer\n");
		return -ENOMEM;
	}

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
