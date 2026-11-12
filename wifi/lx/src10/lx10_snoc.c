/*
 * ath10k's SNOC bus on the OnePlus 6T (WCN3990 on the SDM845): what the layer gives the driver's own
 * snoc.c, qmi.c and core.c that LinuxKPI does not -- the device tree's node of the radio (as the 6T's
 * mainline dt has it, written out here), the platform device and its resources, the interrupts (GIC
 * SPIs 414..425 through the kernel's lx_os_irq_*), devres, DMA masks, the supplies and the clock, and
 * a few of FreeBSD's kernel functions LinuxKPI's headers reach. tools/mk-ath10k.sh builds it with
 * the driver; lx_base.c (shared with ath11k) has the rest of the layer.
 *
 * THE SUPPLIES ARE NOT VOTED FOR HERE. The 6T's boot leaves four of the radio's five LDOs on (run 29:
 * l5 800 mV, l7 1800, l17 1304, l25 3104 on; l23 off); regulator_bulk_enable() says what it would ask
 * and asks nothing, until the user decides on W4b's votes (l23 on, l25 at 3.3 V, RF_CLK2). The same for
 * the clock.
 *
 * The 802.11 side (mac80211, NAPI, the network device) is W4c's: until then tools/mk-ath10k.sh gives
 * each of those names a stub that says its name and answers 0.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/pwrseq/consumer.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/smem.h>
#include <linux/iommu.h>
#include <net/cfg80211.h>
#define LX_QMI_AS_IT_IS
#include <linux/soc/qcom/qmi.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/bus.h>

#include <lx_os.h>
#include "lx.h"

/* what the kernel's 6T glue gives (W4b): the hypervisor's MP_ASSIGN with lists, as qcom_scm_assign_mem() */
int lx_os_assign_memory(unsigned long long address, unsigned long size, unsigned long long from, const int *vmid, const int *perm,
    unsigned count);

extern struct platform_driver *const lx10_platform_driver;     /* snoc.c's, by module_platform_driver() (include10) */
extern unsigned int ath10k_debug_mask;

#define LX10_IRQS               12              /* dt: interrupts of wifi@18800000, GIC SPI 414..425, one per copy engine */
#define LX10_MEMBASE            0x18800000ull   /* dt: reg, "membase" */
#define LX10_MEMBASE_SIZE       0x800000ull
#define LX10_MSA                0x8df00000ull   /* dt: memory-region = <&wlan_msa_mem> */
#define LX10_MSA_SIZE           0x100000ull

/* ---- the node: sdm845.dtsi's wifi@18800000 with sdm845-oneplus-common.dtsi's &wifi --------------------- */

static const char compatible[] = "qcom,wcn3990-wifi";
static const struct property wifi_properties[] = {
	{ "compatible", sizeof(compatible), compatible },
	{ "qcom,snoc-host-cap-8bit-quirk", 0, "" },
};
static struct device_node wifi_node = {
	.name = "wifi",
	.full_name = "/soc@0/wifi@18800000",
	.properties = wifi_properties,
	.nproperties = sizeof(wifi_properties) / sizeof(wifi_properties[0]),
};
static struct platform_device wifi_device;
static struct resource wifi_membase = {
	.start = LX10_MEMBASE,
	.end = LX10_MEMBASE + LX10_MEMBASE_SIZE - 1,
	.name = "membase",
	.flags = IORESOURCE_MEM,
};

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	for (int i = 0; np != NULL && i < np->nproperties; i++) {
		if (strcmp(np->properties[i].name, name) == 0) {
			if (lenp != NULL) {
				*lenp = np->properties[i].length;
			}
			return (&np->properties[i]);
		}
	}
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	const struct property *p = of_find_property(np, name, lenp);

	return (p != NULL ? p->value : NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	const struct property *p = of_find_property(np, name, NULL);

	if (p == NULL) {
		return (-EINVAL);
	}
	if (p->length < 1 || ((const char *)p->value)[p->length - 1] != 0) {
		return (-EILSEQ);
	}
	*out = p->value;
	return (0);
}

int
of_property_read_string_index(const struct device_node *np, const char *propname, int index, const char **output)
{
	const struct property *p = of_find_property(np, propname, NULL);
	const char *s, *end;

	if (p == NULL) {
		return (-EINVAL);
	}
	for (s = p->value, end = s + p->length; s < end; s += strlen(s) + 1, index--) {
		if (index == 0) {
			*output = s;
			return (0);
		}
	}
	return (-ENODATA);
}

int
of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out)
{
	const struct property *p = of_find_property(np, name, NULL);
	const uint8_t *b;

	if (p == NULL) {
		return (-EINVAL);
	}
	if ((index + 1) * 4 > (u32)p->length) {
		return (-EOVERFLOW);
	}
	b = (const uint8_t *)p->value + 4 * index;
	*out = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
	return (0);
}

int
of_property_read_u8_array(const struct device_node *np, const char *propname, u8 *out_values, size_t sz)
{
	const struct property *p = of_find_property(np, propname, NULL);

	if (p == NULL) {
		return (-EINVAL);
	}
	if ((size_t)p->length < sz) {
		return (-EOVERFLOW);
	}
	memcpy(out_values, p->value, sz);
	return (0);
}

int
of_property_read_u8(const struct device_node *np, const char *propname, u8 *out_value)
{
	return (of_property_read_u8_array(np, propname, out_value, 1));
}

int
device_property_read_u32(const struct device *dev, const char *propname, u32 *val)
{
	return (of_property_read_u32_index(dev->of_node, propname, 0, val));
}

struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	for (struct device_node *child = np != NULL ? np->child : NULL; child != NULL; child = child->sibling) {
		if (strcmp(child->name, name) == 0) {
			return (child);
		}
	}
	return (NULL);
}

/* the one region of its "memory-region": wlan_msa_mem */
int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	if (np != &wifi_node || index != 0) {
		return (-ENODEV);
	}
	res->start = LX10_MSA;
	res->end = LX10_MSA + LX10_MSA_SIZE - 1;
	res->name = "wlan_msa_mem";
	res->flags = IORESOURCE_MEM;
	return (0);
}

const void *
device_get_match_data(const struct device *dev)
{
	const struct of_device_id *id = lx10_platform_driver->driver.of_match_table;

	for (; id != NULL && id->compatible[0] != 0; id++) {
		if (dev->of_node != NULL && of_get_property(dev->of_node, "compatible", NULL) != NULL &&
		    strcmp(id->compatible, of_get_property(dev->of_node, "compatible", NULL)) == 0) {
			return (id->data);
		}
	}
	return (NULL);
}

int
of_dma_configure(struct device *dev, struct device_node *np, bool force_dma)
{
	(void)dev;
	(void)np;
	(void)force_dma;
	return (0);
}

/* ---- the platform device ------------------------------------------------------------------------------- */

struct resource *
platform_get_resource_byname(struct platform_device *dev, unsigned int type, const char *name)
{
	if (dev != &wifi_device || type != IORESOURCE_MEM || strcmp(name, "membase") != 0) {
		return (NULL);
	}
	return (&wifi_membase);
}

/* the copy engines' interrupts by number: the layer's vectors, 0..11 (the kernel adds its first SPI) */
int
platform_get_irq(struct platform_device *dev, unsigned int num)
{
	return (dev == &wifi_device && num < LX10_IRQS ? (int)num : -ENXIO);
}

/* (for a "wifi-firmware" node, which the 6T's dt has none of: never called) */
struct platform_device *
platform_device_register_full(const struct platform_device_info *pdevinfo)
{
	(void)pdevinfo;
	return (ERR_PTR(-ENODEV));
}

void
platform_device_unregister(struct platform_device *pdev)
{
	(void)pdev;
}

void *
devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	void *mapped;

	(void)dev;
	(void)flags;
	/* the MSA's memory, which the hypervisor gives to the modem and the radio: mapped, read only by a crash dump */
	mapped = lx_os_map_registers(offset, size);
	return (mapped != NULL ? mapped : ERR_PTR(-ENOMEM));
}

/* ---- devres: what devm_* allocates, freed with the device (here: never; the driver is made once) ------- */

struct lx10_devres {
	void (*release)(struct device *, void *);
	size_t size;
	uint64_t data[];
};

void *
lkpi_devres_alloc(void (*release)(struct device *, void *), size_t size, gfp_t gfp)
{
	struct lx10_devres *dr;

	if (size == 0 || (dr = lkpi_kmalloc(sizeof(*dr) + size, gfp | __GFP_ZERO)) == NULL) {
		return (NULL);
	}
	dr->release = release;
	dr->size = size;
	return (dr->data);
}

void
lkpi_devres_add(struct device *dev, void *p)
{
	(void)dev;
	(void)p;
}

void
lkpi_devm_kmalloc_release(struct device *dev, void *p)
{
	(void)dev;
	(void)p;
}

/* ---- DMA: the radio's memory is the pool of lx_base.c, which the kernel maps for the radio -------------- */

int
linux_dma_tag_init(struct device *dev, u64 mask)
{
	(void)dev;
	(void)mask;
	return (0);
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 mask)
{
	(void)dev;
	(void)mask;
	return (0);
}

void *
linuxkpi_dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	return (linux_dma_alloc_coherent(dev, size, dma_handle, flag));
}

/* FreeBSD's CRC-32 table (the polynomial 0xedb88320), which LinuxKPI's crc32() reads */
const uint32_t crc32_tab[256] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

/* ---- the interrupts: vector n is GIC SPI first + n (the kernel's lx_os_irq_*; first is 414 on the 6T) ---- */

static struct {
	irq_handler_t handler, thread;
	void *argument;
	bool attached;
} lx10_irq[LX10_IRQS];
static bool lx10_irq_ready;

static void
lx10_irq_arrived(void *argument)
{
	unsigned vector = (unsigned)(uintptr_t)argument;

	if (vector < LX10_IRQS && lx10_irq[vector].handler != NULL &&
	    lx10_irq[vector].handler((int)vector, lx10_irq[vector].argument) == IRQ_WAKE_THREAD && lx10_irq[vector].thread != NULL) {
		lx10_irq[vector].thread((int)vector, lx10_irq[vector].argument);
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_handler, unsigned long flags,
    const char *name, void *arg)
{
	int error;

	(void)dev;
	if (irq >= LX10_IRQS || handler == NULL) {
		return (-EINVAL);
	}
	if (!lx10_irq_ready) {
		unsigned long long address = 0;
		unsigned data = 0;

		if (!lx_os_irq_msi(&address, &data)) {
			lx_tell("lx10: the kernel gives the radio no interrupts\n");
			return (-ENXIO);
		}
		lx10_irq_ready = true;
	}
	lx10_irq[irq].handler = handler;
	lx10_irq[irq].thread = thread_handler;
	lx10_irq[irq].argument = arg;
	if ((error = lx_os_irq_attach(irq, lx10_irq_arrived, (void *)(uintptr_t)irq)) != 0) {
		lx_tell("lx10: interrupt %u (%s): not attached, %d\n", irq, name, error);
		lx10_irq[irq].handler = NULL;
		return (-error);
	}
	lx10_irq[irq].attached = true;
	if (!(flags & IRQF_NO_AUTOEN)) {
		lx_os_irq_enable(irq);
	}
	return (0);
}

int
lkpi_enable_irq(unsigned int irq)
{
	if (irq < LX10_IRQS && lx10_irq[irq].attached) {
		lx_os_irq_enable(irq);
	}
	return (0);
}

void
lkpi_disable_irq(unsigned int irq)
{
	if (irq < LX10_IRQS && lx10_irq[irq].attached) {
		lx_os_irq_disable(irq);
	}
}

/* (the kernel detaches all at once: one is masked here and forgotten) */
void
lkpi_free_irq(unsigned int irq, void *arg)
{
	(void)arg;
	if (irq < LX10_IRQS && lx10_irq[irq].attached) {
		lx_os_irq_disable(irq);
		lx10_irq[irq].handler = NULL;
		lx10_irq[irq].attached = false;
	}
}

/* ---- supplies, clock, power sequencer: SAID, NOT ASKED FOR (see the top) --------------------------------- */

int
devm_regulator_bulk_get(struct device *dev, int num_consumers, struct regulator_bulk_data *consumers)
{
	(void)dev;
	for (int i = 0; i < num_consumers; i++) {
		consumers[i].consumer = (struct regulator *)(uintptr_t)(i + 1);
		consumers[i].ret = 0;
	}
	return (0);
}

int
regulator_bulk_enable(int num_consumers, struct regulator_bulk_data *consumers)
{
	for (int i = 0; i < num_consumers; i++) {
		lx_tell("lx10: supply %s: left as the boot leaves it (no vote: W4b's)\n", consumers[i].supply);
	}
	return (0);
}

int
regulator_bulk_disable(int num_consumers, struct regulator_bulk_data *consumers)
{
	(void)num_consumers;
	(void)consumers;
	return (0);
}

int
devm_clk_bulk_get_optional(struct device *dev, int num_clks, struct clk_bulk_data *clks)
{
	(void)dev;
	for (int i = 0; i < num_clks; i++) {
		clks[i].clk = NULL;             /* (a clock that is not there: what Linux gives for an optional one) */
	}
	return (0);
}

int
clk_bulk_prepare_enable(int num_clks, const struct clk_bulk_data *clks)
{
	for (int i = 0; i < num_clks; i++) {
		lx_tell("lx10: clock %s: left as it is (no vote for RF_CLK2: W4b's)\n", clks[i].id);
	}
	return (0);
}

void
clk_bulk_disable_unprepare(int num_clks, const struct clk_bulk_data *clks)
{
	(void)num_clks;
	(void)clks;
}

/* no power sequencer for the WCN3990 here: snoc.c then takes the supplies one by one */
struct pwrseq_desc *
devm_pwrseq_get(struct device *dev, const char *target)
{
	(void)dev;
	(void)target;
	return (ERR_PTR(-EPROBE_DEFER));
}

int
pwrseq_power_on(struct pwrseq_desc *desc)
{
	(void)desc;
	return (0);
}

int
pwrseq_power_off(struct pwrseq_desc *desc)
{
	(void)desc;
	return (0);
}

/* ---- the SoC's services: the hypervisor, the shared memory, the modem's restarts, nvmem ---------------------- */

int
qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src, const struct qcom_scm_vmperm *newvm, unsigned int dest_cnt)
{
	int vmid[4], perm[4];
	u64 next = 0;

	if (dest_cnt == 0 || dest_cnt > 4) {
		return (-EINVAL);
	}
	for (unsigned i = 0; i < dest_cnt; i++) {
		vmid[i] = newvm[i].vmid;
		perm[i] = newvm[i].perm;
		next |= 1ull << newvm[i].vmid;
	}
	if (lx_os_assign_memory(mem_addr, mem_sz, *src, vmid, perm, dest_cnt) != 0) {
		return (-EINVAL);
	}
	*src = next;
	return (0);
}

/* (the firmware's version into SMEM's image table: not kept here) */
void *
qcom_smem_get(unsigned host, unsigned item, size_t *size)
{
	(void)host;
	(void)item;
	(void)size;
	return (ERR_PTR(-ENOENT));
}

static struct notifier_block *lx10_ssr;

void *
qcom_register_ssr_notifier(const char *name, struct notifier_block *nb)
{
	(void)name;
	lx10_ssr = nb;
	return (&lx10_ssr);
}

int
qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb)
{
	(void)notify;
	(void)nb;
	lx10_ssr = NULL;
	return (0);
}

struct nvmem_cell *
devm_nvmem_cell_get(struct device *dev, const char *id)
{
	(void)dev;
	(void)id;
	return (ERR_PTR(-ENOENT));
}

void *
nvmem_cell_read(struct nvmem_cell *cell, size_t *len)
{
	(void)cell;
	(void)len;
	return (ERR_PTR(-ENOENT));
}

/* the firmware's own IOMMU domain is for a "wifi-firmware" node, which the 6T has none of: never called */
struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	(void)dev;
	return (ERR_PTR(-ENODEV));
}

int
iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	(void)domain;
	(void)dev;
	return (-ENODEV);
}

void
iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
	(void)domain;
	(void)dev;
}

int
iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	(void)domain;
	(void)iova;
	(void)paddr;
	(void)size;
	(void)prot;
	(void)gfp;
	return (-ENODEV);
}

size_t
iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	(void)domain;
	(void)iova;
	(void)size;
	return (0);
}

void
iommu_domain_free(struct iommu_domain *domain)
{
	(void)domain;
}

/* What the driver asks the firmware goes through here first: the mode is said (ath11k's lx_bus.c does more). */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == 0x22) {                   /* QMI_WLFW_WLAN_MODE_REQ_V01 */
		lx_tell("lx10: the driver asks the firmware for a mode (0x22): the radio is to come up\n");
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- FreeBSD's, as LinuxKPI's headers reach them ------------------------------------------------------- */

const uint8_t rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
int linuxkpi_debug_80211;

void
DELAY(int usec)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)(usec > 0 ? usec : 0) * 1000;

	while (lx_os_nanoseconds() < until) {
	}
}

int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	static int nobody;              /* (an address nobody wakes) */
	unsigned long long ns = (unsigned long long)(((__uint128_t)(sbt > 0 ? sbt : 1) * 1000000000ull) >> 32);

	(void)wmesg;
	(void)pr;
	(void)flags;
	lx_os_sleep(&nobody, ns != 0 ? ns : 1);
	return (0);
}

void
nanouptime(struct timespec *tsp)
{
	unsigned long long ns = lx_os_nanoseconds();

	tsp->tv_sec = (time_t)(ns / 1000000000ull);
	tsp->tv_nsec = (long)(ns % 1000000000ull);
}

int
device_printf(device_t dev, const char *format, ...)
{
	char line[256];
	va_list ap;

	(void)dev;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	lx_os_print(line);
	return (0);
}

int
eventratecheck(struct timeval *lasttime, int *cureps, int maxeps)
{
	(void)lasttime;
	(void)cureps;
	(void)maxeps;
	return (1);
}

int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii)
{
	(void)buf;
	(void)len;
	(void)rowsize;
	(void)groupsize;
	(void)ascii;
	if (linebuflen > 0) {
		linebuf[0] = 0;
	}
	return (0);
}

/* ---- the kernel's way in (sys/hakios.lx.h) ------------------------------------------------------------------ */

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

/*
 * snoc.c's probe, on the node above. From there the driver goes on by itself: its qmi.c finds the firmware's
 * service and makes the exchange step 11 of drivers/modem makes by hand; on "ready" the core registers and powers
 * the radio (the supplies said, not asked for), and the copy engines and the firmware's messages follow.
 */
int
lx_start(void)
{
	int ret;

	if (lx_prepare() != 0) {
		lx_tell("lx10: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	ath10k_debug_mask = lx_os_option("wifi_quiet") ? 0 : 0x00000004 | 0x00000020 | 0x00000400 | 0x00100000 | 0x00200000;  /* HTC BOOT BMI SNOC QMI */
	wifi_device.name = "wifi";
	wifi_device.id = -1;
	wifi_device.dev.of_node = &wifi_node;
	wifi_device.dev.kobj.name = "wifi";
	wifi_device.dev.dma_priv = &wifi_device;        /* (dma_set_mask() wants one; the pool is lx_base.c's) */
	wifi_device.num_resources = 1;
	wifi_device.resource = &wifi_membase;
	lx_tell("lx10: ath10k's snoc.c probes the WCN3990 (6T): %s, membase %#llx, %u interrupts, MSA %#llx\n", compatible,
	    LX10_MEMBASE, LX10_IRQS, LX10_MSA);
	ret = lx10_platform_driver->probe(&wifi_device);
	if (ret < 0) {
		lx_tell("lx10: the probe fails: %d\n", ret);
		return (-ret);
	}
	return (0);
}

void
lx_stop(void)
{
	if (lx10_platform_driver->remove != NULL && platform_get_drvdata(&wifi_device) != NULL) {
		lx10_platform_driver->remove(&wifi_device);
		platform_set_drvdata(&wifi_device, NULL);
		lx_tell("lx10: the driver is down again\n");
	}
}

/* (frames: W4c/W5) */
int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	(void)frame;
	(void)length;
	return (ENETDOWN);
}
