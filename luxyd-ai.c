// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>

#define DRIVER_NAME	"luxyd-ai"
#define DRIVER_VERSION	"0.1"

MODULE_AUTHOR("Wan Ahmad Zainie <wanahmadzainie@gmail.com>");
MODULE_DESCRIPTION("LUXYD Technologies AI module driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

/* Hardware information */
#define VENDOR_ID			0x10ee	/* Xilinx Vendor ID */
#define DEVICE_ID			0x7021	/* Kintex-7 Device ID */

/* IOCTL commands */
#define LUXYD_AI_IOCTL_MAGIC		'L'
#define LUXYD_AI_LOAD_MODEL		_IOW (LUXYD_AI_IOCTL_MAGIC, 1, u32)
#define LUXYD_AI_START_INFERENCE	_IOWR(LUXYD_AI_IOCTL_MAGIC, 2, u32)
#define LUXYD_AI_GET_STATUS		_IOR (LUXYD_AI_IOCTL_MAGIC, 3, u32)
#define LUXYD_AI_LOAD_MATRIX		_IOW (LUXYD_AI_IOCTL_MAGIC, 4, struct matrix_size)
#define LUXYD_AI_MULTIPLY_MATRIX	_IO  (LUXYD_AI_IOCTL_MAGIC, 5)

struct matrix_size {
	int m;	/* Matrix A row size, matrix AB row size */
	int n;	/* Matrix A column size, matrix B row size */
	int p;	/* Matrix B column size, matrix AB column size */
};

/* Private data structure */
struct luxyd_ai_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	struct class *dev_class;
	struct device *dev;

	struct mutex ioctl_lock;
};

static struct class *luxyd_ai_class;
static int luxyd_ai_major;

/* Kernel buffer for matrix multiplication */
#define MAX_MATRIX_ROW			32
#define MAX_MATRIX_COL			32
#define MAX_MATRIX_SIZE			(MAX_MATRIX_ROW * MAX_MATRIX_COL)
#define MAX_BUFFER_SIZE			(3 * MAX_MATRIX_SIZE * sizeof(u32))

static void *mbuffer;
static size_t mbuffer_size = PAGE_ALIGN(MAX_BUFFER_SIZE);

/* PCI device ID table */
static const struct pci_device_id luxyd_fpga_id_table[] = {
	//{ PCI_DEVICE(PCI_ANY_ID, PCI_ANY_ID) },
	{ PCI_DEVICE(0x8086, 0x1237) },		/* Testing */
	{ PCI_DEVICE(0x8086, 0xa121) },		/* Testing */
	{ PCI_DEVICE(0x10ee, 0x7021) },		/* Xilinx Kintex-7 */
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
	static struct matrix_size matrix_size;
	static size_t a_size, b_size, p_size, total_size;
	static loff_t a_off, b_off, p_off;
	u16 *a_matrix, *b_matrix;
	u32 *p_matrix;
	int i, j, k, sum;
	int ret = 0;

	if (mutex_lock_interruptible(&drvdata->ioctl_lock))
		return -ERESTARTSYS;

	switch (cmd) {
	case LUXYD_AI_LOAD_MODEL:
		pr_info("%s: ioctl cmd: LUXYD_AI_LOAD_MODEL\n", DRIVER_NAME);
		break;

	case LUXYD_AI_START_INFERENCE:
		pr_info("%s: ioctl cmd: LUXYD_AI_START_INFERENCE\n",
			DRIVER_NAME);
		break;

	case LUXYD_AI_GET_STATUS:
		pr_info("%s: ioctl cmd: LUXYD_AI_GET_STATUS\n", DRIVER_NAME);
		break;

	case LUXYD_AI_LOAD_MATRIX:
		pr_info("%s: ioctl cmd: LUXYD_AI_LOAD_MATRIX\n", DRIVER_NAME);

		if (copy_from_user(&matrix_size,
				   (struct matrix_size __user *)arg,
				   sizeof(struct matrix_size))) {
			ret = -EFAULT;
			break;
		}

		if (matrix_size.m <= 0 || matrix_size.n <= 0
		    || matrix_size.p <= 0) {
			ret = -EINVAL;
			break;
		}

		/* Calculate the required size */
		a_size = (size_t)matrix_size.m * matrix_size.n * sizeof(u16);
		b_size = (size_t)matrix_size.n * matrix_size.p * sizeof(u16);
		p_size = (size_t)matrix_size.m * matrix_size.p * sizeof(u32);
		total_size = a_size + b_size + p_size;

		if (total_size > MAX_BUFFER_SIZE) {
			ret = -ENOMEM;
			break;
		}

		/* Calculate the offset in the allocated buffer */
		a_off = 0;
		b_off = a_off + a_size;
		p_off = b_off + b_size;

		pr_info("%s: LUXYD_AI_LOAD_MATRIX completed\n", DRIVER_NAME);
		break;

	case LUXYD_AI_MULTIPLY_MATRIX:
		pr_info("%s: ioctl cmd: LUXYD_AI_MULTIPLY_MATRIX\n", DRIVER_NAME);

		a_matrix = (u16 *)(mbuffer + a_off);
		b_matrix = (u16 *)(mbuffer + b_off);
		p_matrix = (u32 *)(mbuffer + p_off);

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

		pr_info("%s: LUXYD_AI_MULTIPLY_MATRIX completed\n", DRIVER_NAME);
		break;

	default:
		pr_info("%s: ioctl cmd: not supported\n", DRIVER_NAME);
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&drvdata->ioctl_lock);

	return ret;
}

static int luxyd_ai_mmap(struct file *filp, struct vm_area_struct *vma)
{
	u32 offset = vma->vm_pgoff << PAGE_SHIFT;
	u32 size = vma->vm_end - vma->vm_start;
	u32 pfn;

	pr_info("%s: mmap operation\n", DRIVER_NAME);

	if (offset + size > mbuffer_size) {
		pr_err("%s: mmap request out of bound\n", DRIVER_NAME);
		return -EINVAL;
	}

	pfn = vmalloc_to_pfn(mbuffer + offset);
	if (!pfn) {
		pr_err("%s: vmalloc_to_pfn failed for address %p\n",
		       DRIVER_NAME, mbuffer + offset);
		return -ENOMEM;
	}

	/* Remap to userspace */
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
		pr_err("%s: remap_pfn_range failed\n", DRIVER_NAME);
		return -EIO;
	}

	pr_info("%s: mapped %u bytes from kernel buffer at %p (pfn %u) "
		"to userspace at 0x%lx\n", DRIVER_NAME,
		size, mbuffer + offset, pfn, vma->vm_start);

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
	int ret;

	pr_info("%s: probing device 0x%04x:0x%04x\n", DRIVER_NAME,
		pdev->vendor, pdev->device);

	/* Allocate private data structure */
	drvdata = devm_kzalloc(dev, sizeof(drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->pdev = pdev;
	pci_set_drvdata(pdev, drvdata);

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

	//luxyd_ai_class = class_create(THIS_MODULE, DRIVER_NAME);
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

	/* Initialize buffer for matrix multiplication */
	mbuffer = vmalloc(mbuffer_size);
	if (!mbuffer) {
		dev_err(dev, "failed to allocate memory buffer\n");
		ret = -ENOMEM;
		goto out_destroy_class;
	}

	memset(mbuffer, 0, mbuffer_size);
	dev_info(dev,
		 "%zu bytes allocated for matrix multiplication at %p\n",
		 mbuffer_size, mbuffer);

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

	if (mbuffer) {
		vfree(mbuffer);
		mbuffer = NULL;
	}

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
