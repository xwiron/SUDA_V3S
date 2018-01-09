/*
 * PCIe host controller driver for HiSilicon SoCs
 *
 * Copyright (C) 2015 HiSilicon Co., Ltd. http://www.hisilicon.com
 *
 * Authors: Zhou Wang <wangzhou1@hisilicon.com>
 *          Dacai Zhu <zhudacai@hisilicon.com>
 *          Gabriele Paoloni <gabriele.paoloni@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci-ecam.h>
#include <linux/regmap.h>
#include "../pci.h"

#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)

static int hisi_pcie_acpi_rd_conf(struct pci_bus *bus, u32 devfn, int where,
				  int size, u32 *val)
{
	struct pci_config_window *cfg = bus->sysdata;
	int dev = PCI_SLOT(devfn);

	if (bus->number == cfg->busr.start) {
		/* access only one slot on each root port */
		if (dev > 0)
			return PCIBIOS_DEVICE_NOT_FOUND;
		else
			return pci_generic_config_read32(bus, devfn, where,
							 size, val);
	}

	return pci_generic_config_read(bus, devfn, where, size, val);
}

static int hisi_pcie_acpi_wr_conf(struct pci_bus *bus, u32 devfn,
				  int where, int size, u32 val)
{
	struct pci_config_window *cfg = bus->sysdata;
	int dev = PCI_SLOT(devfn);

	if (bus->number == cfg->busr.start) {
		/* access only one slot on each root port */
		if (dev > 0)
			return PCIBIOS_DEVICE_NOT_FOUND;
		else
			return pci_generic_config_write32(bus, devfn, where,
							  size, val);
	}

	return pci_generic_config_write(bus, devfn, where, size, val);
}

static void __iomem *hisi_pcie_map_bus(struct pci_bus *bus, unsigned int devfn,
				       int where)
{
	struct pci_config_window *cfg = bus->sysdata;
	void __iomem *reg_base = cfg->priv;

	if (bus->number == cfg->busr.start)
		return reg_base + where;
	else
		return pci_ecam_map_bus(bus, devfn, where);
}

static int hisi_pcie_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct acpi_device *adev = to_acpi_device(dev);
	struct acpi_pci_root *root = acpi_driver_data(adev);
	struct resource *res;
	void __iomem *reg_base;
	int ret;

	/*
	 * Retrieve RC base and size from a HISI0081 device with _UID
	 * matching our segment.
	 */
	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	ret = acpi_get_rc_resources(dev, "HISI0081", root->segment, res);
	if (ret) {
		dev_err(dev, "can't get rc base address\n");
		return -ENOMEM;
	}

	reg_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!reg_base)
		return -ENOMEM;

	cfg->priv = reg_base;
	return 0;
}

struct pci_ecam_ops hisi_pcie_ops = {
	.bus_shift    = 20,
	.init         =  hisi_pcie_init,
	.pci_ops      = {
		.map_bus    = hisi_pcie_map_bus,
		.read       = hisi_pcie_acpi_rd_conf,
		.write      = hisi_pcie_acpi_wr_conf,
	}
};

#endif

#ifdef CONFIG_PCI_HISI

#include "pcie-designware.h"

#define PCIE_SUBCTRL_SYS_STATE4_REG		0x6818
#define PCIE_HIP06_CTRL_OFF			0x1000
#define PCIE_SYS_STATE4				(PCIE_HIP06_CTRL_OFF + 0x31c)
#define PCIE_LTSSM_LINKUP_STATE			0x11
#define PCIE_LTSSM_STATE_MASK			0x3F

#define to_hisi_pcie(x)	container_of(x, struct hisi_pcie, pp)

struct hisi_pcie;

struct pcie_soc_ops {
	int (*hisi_pcie_link_up)(struct hisi_pcie *hisi_pcie);
};

struct hisi_pcie {
	struct pcie_port pp;		/* pp.dbi_base is DT rc_dbi */
	struct regmap *subctrl;
	u32 port_id;
	struct pcie_soc_ops *soc_ops;
};

/* HipXX PCIe host only supports 32-bit config access */
static int hisi_pcie_cfg_read(struct pcie_port *pp, int where, int size,
			      u32 *val)
{
	u32 reg;
	u32 reg_val;
	void *walker = &reg_val;

	walker += (where & 0x3);
	reg = where & ~0x3;
	reg_val = dw_pcie_readl_rc(pp, reg);

	if (size == 1)
		*val = *(u8 __force *) walker;
	else if (size == 2)
		*val = *(u16 __force *) walker;
	else if (size == 4)
		*val = reg_val;
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

/* HipXX PCIe host only supports 32-bit config access */
static int hisi_pcie_cfg_write(struct pcie_port *pp, int where, int  size,
				u32 val)
{
	u32 reg_val;
	u32 reg;
	void *walker = &reg_val;

	walker += (where & 0x3);
	reg = where & ~0x3;
	if (size == 4)
		dw_pcie_writel_rc(pp, reg, val);
	else if (size == 2) {
		reg_val = dw_pcie_readl_rc(pp, reg);
		*(u16 __force *) walker = val;
		dw_pcie_writel_rc(pp, reg, reg_val);
	} else if (size == 1) {
		reg_val = dw_pcie_readl_rc(pp, reg);
		*(u8 __force *) walker = val;
		dw_pcie_writel_rc(pp, reg, reg_val);
	} else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

static int hisi_pcie_link_up_hip05(struct hisi_pcie *hisi_pcie)
{
	u32 val;

	regmap_read(hisi_pcie->subctrl, PCIE_SUBCTRL_SYS_STATE4_REG +
		    0x100 * hisi_pcie->port_id, &val);

	return ((val & PCIE_LTSSM_STATE_MASK) == PCIE_LTSSM_LINKUP_STATE);
}

static int hisi_pcie_link_up_hip06(struct hisi_pcie *hisi_pcie)
{
	struct pcie_port *pp = &hisi_pcie->pp;
	u32 val;

	val = dw_pcie_readl_rc(pp, PCIE_SYS_STATE4);

	return ((val & PCIE_LTSSM_STATE_MASK) == PCIE_LTSSM_LINKUP_STATE);
}

static int hisi_pcie_link_up(struct pcie_port *pp)
{
	struct hisi_pcie *hisi_pcie = to_hisi_pcie(pp);

	return hisi_pcie->soc_ops->hisi_pcie_link_up(hisi_pcie);
}

static struct pcie_host_ops hisi_pcie_host_ops = {
	.rd_own_conf = hisi_pcie_cfg_read,
	.wr_own_conf = hisi_pcie_cfg_write,
	.link_up = hisi_pcie_link_up,
};

static int hisi_add_pcie_port(struct hisi_pcie *hisi_pcie,
			      struct platform_device *pdev)
{
	struct pcie_port *pp = &hisi_pcie->pp;
	struct device *dev = pp->dev;
	int ret;
	u32 port_id;

	if (of_property_read_u32(dev->of_node, "port-id", &port_id)) {
		dev_err(dev, "failed to read port-id\n");
		return -EINVAL;
	}
	if (port_id > 3) {
		dev_err(dev, "Invalid port-id: %d\n", port_id);
		return -EINVAL;
	}
	hisi_pcie->port_id = port_id;

	pp->ops = &hisi_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int hisi_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hisi_pcie *hisi_pcie;
	struct pcie_port *pp;
	const struct of_device_id *match;
	struct resource *reg;
	struct device_driver *driver;
	int ret;

	hisi_pcie = devm_kzalloc(dev, sizeof(*hisi_pcie), GFP_KERNEL);
	if (!hisi_pcie)
		return -ENOMEM;

	pp = &hisi_pcie->pp;
	pp->dev = dev;
	driver = dev->driver;

	match = of_match_device(driver->of_match_table, dev);
	hisi_pcie->soc_ops = (struct pcie_soc_ops *) match->data;

	hisi_pcie->subctrl =
	syscon_regmap_lookup_by_compatible("hisilicon,pcie-sas-subctrl");
	if (IS_ERR(hisi_pcie->subctrl)) {
		dev_err(dev, "cannot get subctrl base\n");
		return PTR_ERR(hisi_pcie->subctrl);
	}

	reg = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbi");
	pp->dbi_base = devm_ioremap_resource(dev, reg);
	if (IS_ERR(pp->dbi_base))
		return PTR_ERR(pp->dbi_base);

	ret = hisi_add_pcie_port(hisi_pcie, pdev);
	if (ret)
		return ret;

	return 0;
}

static struct pcie_soc_ops hip05_ops = {
		&hisi_pcie_link_up_hip05
};

static struct pcie_soc_ops hip06_ops = {
		&hisi_pcie_link_up_hip06
};

static const struct of_device_id hisi_pcie_of_match[] = {
	{
			.compatible = "hisilicon,hip05-pcie",
			.data	    = (void *) &hip05_ops,
	},
	{
			.compatible = "hisilicon,hip06-pcie",
			.data	    = (void *) &hip06_ops,
	},
	{},
};

static struct platform_driver hisi_pcie_driver = {
	.probe  = hisi_pcie_probe,
	.driver = {
		   .name = "hisi-pcie",
		   .of_match_table = hisi_pcie_of_match,
	},
};
builtin_platform_driver(hisi_pcie_driver);

#endif