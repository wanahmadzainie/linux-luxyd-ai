// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/module.h>
#include <linux/pci.h>

#define DRIVER_NAME	"luxyd-fpga-pci"
#define DRIVER_VERSION	"0.1"

static int fpga_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;

	pr_info("probing device 0x%04x:0x%04x\n", pdev->vendor, pdev->device);

	/* driver initialization */

	dev_info(dev, "probed\n");
	return 0;
}

static void fpga_remove(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;

	pr_info("removing device 0x%04x:0x%04x\n", pdev->vendor, pdev->device);

	/* driver clean up */

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
