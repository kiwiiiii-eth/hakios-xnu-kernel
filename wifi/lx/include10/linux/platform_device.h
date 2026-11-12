/*
 * <linux/platform_device.h> for ath10k's snoc.c: Linux's platform device and driver as snoc.c uses them, in place of
 * LinuxKPI's (whose guard this takes, so that header is not read after it), which has no probe and no resources.
 * module_platform_driver() leaves the driver where the layer finds it (lx10_platform_driver): the kernel probes it.
 */
#ifndef _LINUXKPI_LINUX_PLATFORM_DEVICE_H
#define _LINUXKPI_LINUX_PLATFORM_DEVICE_H
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>

struct resource;
struct fwnode_handle;
struct property_entry;

struct platform_device {
	const char *name;
	int id;
	bool id_auto;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
};

struct platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};

struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
	const struct property_entry *properties;
};

#define to_platform_device(x)   container_of((x), struct platform_device, dev)
#define dev_is_platform(dev)    (true)

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

struct platform_device *platform_device_register_full(const struct platform_device_info *pdevinfo);
void platform_device_unregister(struct platform_device *pdev);
struct resource *platform_get_resource_byname(struct platform_device *dev, unsigned int type, const char *name);
int platform_get_irq(struct platform_device *dev, unsigned int num);

#define module_platform_driver(__d)     struct platform_driver *const lx10_platform_driver = &(__d)
#endif
