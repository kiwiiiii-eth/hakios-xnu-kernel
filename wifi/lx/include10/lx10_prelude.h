/*
 * ath10k's (tools/mk-ath10k.sh): included after lx_prelude.h. What FreeBSD's LinuxKPI lacks that ath10k reaches for,
 * declared under Linux's names; the functions are the layer's to give (W4b), until then the build lists them.
 */
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/cleanup.h>
/* ce.h takes __typeof__ of a counted array, which this clang does not yet do: the attribute is dropped */
#undef __counted_by
#define __counted_by(_x)
/* wmi-tlv.c: guard(spinlock_bh), as spinlock.h has spinlock_irqsave */
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))
struct device_node;
int of_property_read_u8_array(const struct device_node *np, const char *propname, u8 *out_values, size_t sz);
struct nvmem_cell;
struct device;
struct nvmem_cell *devm_nvmem_cell_get(struct device *dev, const char *id);
void *nvmem_cell_read(struct nvmem_cell *cell, size_t *len);
/* snoc.c's platform: the device tree's node, its memory, interrupts, supplies and the firmware's IOMMU domain
 * (linux/platform_device.h of this directory has the platform device itself) */
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#ifndef IRQF_NO_AUTOEN
#define IRQF_NO_AUTOEN          0x00080000
#endif
struct net_device;
struct regulator;
#ifndef IOMMU_READ
#define IOMMU_READ              (1 << 0)
#define IOMMU_WRITE             (1 << 1)
#endif
struct regulator_bulk_data {
	const char *supply;
	struct regulator *consumer;
	int init_load_uA;
	int ret;
};
int device_property_read_u32(const struct device *dev, const char *propname, u32 *val);
const void *device_get_match_data(const struct device *dev);
int of_property_read_u8(const struct device_node *np, const char *propname, u8 *out_value);
int of_property_read_string_index(const struct device_node *np, const char *propname, int index, const char **output);
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);
int of_dma_configure(struct device *dev, struct device_node *np, bool force_dma);
struct iommu_domain *iommu_paging_domain_alloc(struct device *dev);
int iommu_attach_device(struct iommu_domain *domain, struct device *dev);
void iommu_detach_device(struct iommu_domain *domain, struct device *dev);
int iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size, int prot, gfp_t gfp);
size_t iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size);
void iommu_domain_free(struct iommu_domain *domain);
int devm_regulator_bulk_get(struct device *dev, int num_consumers, struct regulator_bulk_data *consumers);
int regulator_bulk_enable(int num_consumers, struct regulator_bulk_data *consumers);
int regulator_bulk_disable(int num_consumers, struct regulator_bulk_data *consumers);
void netif_threaded_enable(struct net_device *dev);
