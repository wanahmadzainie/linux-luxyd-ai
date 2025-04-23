// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Luxyd Technologies
 */

#include <linux/module.h>

#define DRIVER_NAME	"luxyd-ai"
#define DRIVER_VERSION	"0.1"

MODULE_AUTHOR("Wan Ahmad Zainie <wanahmadzainie@gmail.com>");
MODULE_DESCRIPTION("LUXYD Technologies AI module driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

static int __init luxyd_ai_init(void)
{
	pr_info("%s: version %s loading\n", DRIVER_NAME, DRIVER_VERSION);

	pr_info("%s: version %s loaded successfully\n",
		DRIVER_NAME, DRIVER_VERSION);

	return 0;
}

static void __exit luxyd_ai_cleanup(void)
{
	pr_info("%s: version %s unloading\n", DRIVER_NAME, DRIVER_VERSION);

	pr_info("%s: version %s unloaded successfully\n",
		DRIVER_NAME, DRIVER_VERSION);
}

module_init(luxyd_ai_init);
module_exit(luxyd_ai_cleanup);
