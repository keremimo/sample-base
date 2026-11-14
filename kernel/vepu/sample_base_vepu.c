// SPDX-License-Identifier: GPL-2.0
/*
 * The RK3566's hardware JPEG encoder (the RK3568 VEPU, video-codec@fdee0000)
 * for Screen Mirror (src/drivers/screen_mirror.h). The MPC's device tree
 * leaves the node disabled, and hantro-vpu refuses a node that is not
 * available (hantro_disable_multicore()). This module sets the node's
 * status to "okay" in the live tree, in memory only (the kernel has no
 * CONFIG_OF_DYNAMIC, so the property's value is replaced in place), and
 * creates its platform device; hantro-vpu then binds it as
 * "rockchip,rk3568-vepu-enc". Its IOMMU (fdee0800) is enabled already.
 *
 * It cannot be unloaded: removing the device rebooted the MPC (2026-11-14),
 * so there is no exit function. A reboot gives the stock state back.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

static int __init sample_base_vepu_init(void)
{
	struct device_node *node = of_find_compatible_node(NULL, NULL, "rockchip,rk3568-vepu");
	struct property *status = NULL;
	void *old_value = NULL;
	int old_length = 0;
	char *okay = NULL;
	struct platform_device *vepu;

	if (!node)
		return -ENODEV;
	if (!of_device_is_available(node)) {
		status = of_find_property(node, "status", NULL);
		okay = kstrdup("okay", GFP_KERNEL);
		if (!status || !okay) {
			kfree(okay);
			of_node_put(node);
			return -ENOENT;
		}
		old_value = status->value;
		old_length = status->length;
		status->value = okay;
		status->length = 5;
	}
	vepu = of_platform_device_create(node, NULL, NULL);
	if (!vepu) {
		if (status) {
			status->value = old_value;
			status->length = old_length;
		}
		kfree(okay);
		of_node_put(node);
		return -EIO;
	}
	pr_info("sample_base_vepu: %s created for %pOF\n", dev_name(&vepu->dev), node);
	return 0;
}

module_init(sample_base_vepu_init);
MODULE_DESCRIPTION("Enable the RK3568 VEPU JPEG encoder the MPC Sample's device tree leaves disabled");
MODULE_LICENSE("GPL");
