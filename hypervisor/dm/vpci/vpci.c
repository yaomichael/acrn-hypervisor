/*-
* Copyright (c) 2011 NetApp, Inc.
* Copyright (c) 2018 Intel Corporation
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*
* $FreeBSD$
*/

#include <errno.h>
#include <ptdev.h>
#include <vm.h>
#include <vtd.h>
#include <io.h>
#include <mmu.h>
#include <logmsg.h>
#include "vpci_priv.h"
#include "pci_dev.h"

static void vpci_init_vdevs(struct acrn_vm *vm);
static void deinit_prelaunched_vm_vpci(struct acrn_vm *vm);
static void deinit_postlaunched_vm_vpci(struct acrn_vm *vm);
static int32_t vpci_read_cfg(struct acrn_vpci *vpci, union pci_bdf bdf, uint32_t offset, uint32_t bytes, uint32_t *val);
static int32_t vpci_write_cfg(struct acrn_vpci *vpci, union pci_bdf bdf, uint32_t offset, uint32_t bytes, uint32_t val);
static struct pci_vdev *find_available_vdev(struct acrn_vpci *vpci, union pci_bdf bdf);

/**
 * @pre vcpu != NULL
 * @pre vcpu->vm != NULL
 */
static bool vpci_pio_cfgaddr_read(struct acrn_vcpu *vcpu, uint16_t addr, size_t bytes)
{
	uint32_t val = ~0U;
	struct acrn_vpci *vpci = &vcpu->vm->vpci;
	union pci_cfg_addr_reg *cfg_addr = &vpci->addr;
	struct pio_request *pio_req = &vcpu->req.reqs.pio;

	if ((addr == (uint16_t)PCI_CONFIG_ADDR) && (bytes == 4U)) {
		val = cfg_addr->value;
	}

	pio_req->value = val;

	return true;
}

/**
 * @pre vcpu != NULL
 * @pre vcpu->vm != NULL
 *
 * @retval true on success.
 * @retval false. (ACRN will deliver this IO request to DM to handle for post-launched VM)
 */
static bool vpci_pio_cfgaddr_write(struct acrn_vcpu *vcpu, uint16_t addr, size_t bytes, uint32_t val)
{
	bool ret = true;
	struct acrn_vpci *vpci = &vcpu->vm->vpci;
	union pci_cfg_addr_reg *cfg_addr = &vpci->addr;
	union pci_bdf vbdf;

	if ((addr == (uint16_t)PCI_CONFIG_ADDR) && (bytes == 4U)) {
		/* unmask reserved fields: BITs 24-30 and BITs 0-1 */
		cfg_addr->value = val & (~0x7f000003U);

		if (is_postlaunched_vm(vcpu->vm)) {
			const struct pci_vdev *vdev;

			vbdf.value = cfg_addr->bits.bdf;
			vdev = find_available_vdev(vpci, vbdf);
			/* For post-launched VM, ACRN HV will only handle PT device,
			 * all virtual PCI device and QUIRK PT device
			 * still need to deliver to ACRN DM to handle.
			 */
			if ((vdev == NULL) || is_quirk_ptdev(vdev)) {
				ret = false;
			}
		}
	}

	return ret;
}

static inline bool vpci_is_valid_access_offset(uint32_t offset, uint32_t bytes)
{
	return ((offset & (bytes - 1U)) == 0U);
}

static inline bool vpci_is_valid_access_byte(uint32_t bytes)
{
	return ((bytes == 1U) || (bytes == 2U) || (bytes == 4U));
}

static inline bool vpci_is_valid_access(uint32_t offset, uint32_t bytes)
{
	return (vpci_is_valid_access_byte(bytes) && vpci_is_valid_access_offset(offset, bytes));
}

/**
 * @pre vcpu != NULL
 * @pre vcpu->vm != NULL
 * @pre vcpu->vm->vm_id < CONFIG_MAX_VM_NUM
 * @pre (get_vm_config(vcpu->vm->vm_id)->load_order == PRE_LAUNCHED_VM)
 *	|| (get_vm_config(vcpu->vm->vm_id)->load_order == SOS_VM)
 *
 * @retval true on success.
 * @retval false. (ACRN will deliver this IO request to DM to handle for post-launched VM)
 */
static bool vpci_pio_cfgdata_read(struct acrn_vcpu *vcpu, uint16_t addr, size_t bytes)
{
	int32_t ret = 0;
	struct acrn_vm *vm = vcpu->vm;
	struct acrn_vpci *vpci = &vm->vpci;
	union pci_cfg_addr_reg cfg_addr;
	union pci_bdf bdf;
	uint16_t offset = addr - PCI_CONFIG_DATA;
	uint32_t val = ~0U;
	struct pio_request *pio_req = &vcpu->req.reqs.pio;

	cfg_addr.value = atomic_readandclear32(&vpci->addr.value);
	if (cfg_addr.bits.enable != 0U) {
		if (vpci_is_valid_access(cfg_addr.bits.reg_num + offset, bytes)) {
			bdf.value = cfg_addr.bits.bdf;
			ret = vpci_read_cfg(vpci, bdf, cfg_addr.bits.reg_num + offset, bytes, &val);
		}
	}

	pio_req->value = val;
	return (ret == 0);
}

/**
 * @pre vcpu != NULL
 * @pre vcpu->vm != NULL
 * @pre vcpu->vm->vm_id < CONFIG_MAX_VM_NUM
 * @pre (get_vm_config(vcpu->vm->vm_id)->load_order == PRE_LAUNCHED_VM)
 *	|| (get_vm_config(vcpu->vm->vm_id)->load_order == SOS_VM)
 *
 * @retval true on success.
 * @retval false. (ACRN will deliver this IO request to DM to handle for post-launched VM)
 */
static bool vpci_pio_cfgdata_write(struct acrn_vcpu *vcpu, uint16_t addr, size_t bytes, uint32_t val)
{
	int32_t ret = 0;
	struct acrn_vm *vm = vcpu->vm;
	struct acrn_vpci *vpci = &vm->vpci;
	union pci_cfg_addr_reg cfg_addr;
	union pci_bdf bdf;
	uint16_t offset = addr - PCI_CONFIG_DATA;

	cfg_addr.value = atomic_readandclear32(&vpci->addr.value);
	if (cfg_addr.bits.enable != 0U) {
		if (vpci_is_valid_access(cfg_addr.bits.reg_num + offset, bytes)) {
			bdf.value = cfg_addr.bits.bdf;
			ret = vpci_write_cfg(vpci, bdf, cfg_addr.bits.reg_num + offset, bytes, val);
		}
	}

	return (ret == 0);
}

/**
 * @pre io_req != NULL && private_data != NULL
 *
 * @retval 0 on success.
 * @retval other on false. (ACRN will deliver this MMIO request to DM to handle for post-launched VM)
 */
static int32_t vpci_mmio_cfg_access(struct io_request *io_req, void *private_data)
{
	int32_t ret = 0;
	struct mmio_request *mmio = &io_req->reqs.mmio;
	struct acrn_vpci *vpci = (struct acrn_vpci *)private_data;
	uint64_t pci_mmcofg_base = vpci->pci_mmcfg_base;
	uint64_t address = mmio->address;
	uint32_t reg_num = (uint32_t)(address & 0xfffUL);
	union pci_bdf bdf;

	/**
	 * Enhanced Configuration Address Mapping
	 * A[(20+n-1):20] Bus Number 1 ≤ n ≤ 8
	 * A[19:15] Device Number
	 * A[14:12] Function Number
	 * A[11:8] Extended Register Number
	 * A[7:2] Register Number
	 * A[1:0] Along with size of the access, used to generate Byte Enables
	 */
	bdf.value = (uint16_t)((address - pci_mmcofg_base) >> 12U);

	if (mmio->direction == REQUEST_READ) {
		if (!is_plat_hidden_pdev(bdf)) {
			ret = vpci_read_cfg(vpci, bdf, reg_num, (uint32_t)mmio->size, (uint32_t *)&mmio->value);
		} else {
			/* expose and pass through platform hidden devices to SOS */
			mmio->value = (uint64_t)pci_pdev_read_cfg(bdf, reg_num, (uint32_t)mmio->size);
		}
	} else {
		if (!is_plat_hidden_pdev(bdf)) {
			ret = vpci_write_cfg(vpci, bdf, reg_num, (uint32_t)mmio->size, (uint32_t)mmio->value);
		} else {
			/* expose and pass through platform hidden devices to SOS */
			pci_pdev_write_cfg(bdf, reg_num, (uint32_t)mmio->size, (uint32_t)mmio->value);
		}
	}

	return ret;
}

/**
 * @pre vm != NULL
 * @pre vm->vm_id < CONFIG_MAX_VM_NUM
 */
void init_vpci(struct acrn_vm *vm)
{
	struct vm_io_range pci_cfgaddr_range = {
		.base = PCI_CONFIG_ADDR,
		.len = 1U
	};

	struct vm_io_range pci_cfgdata_range = {
		.base = PCI_CONFIG_DATA,
		.len = 4U
	};

	struct acrn_vm_config *vm_config;
	uint64_t pci_mmcfg_base;

	vm->iommu = create_iommu_domain(vm->vm_id, hva2hpa(vm->arch_vm.nworld_eptp), 48U);
	/* Build up vdev list for vm */
	vpci_init_vdevs(vm);

	vm_config = get_vm_config(vm->vm_id);
	if (vm_config->load_order != PRE_LAUNCHED_VM) {
		/* PCI MMCONFIG for post-launched VM is fixed to 0xE0000000 */
		pci_mmcfg_base = (vm_config->load_order == SOS_VM) ? get_mmcfg_base() : 0xE0000000UL;
		vm->vpci.pci_mmcfg_base = pci_mmcfg_base;
		register_mmio_emulation_handler(vm, vpci_mmio_cfg_access,
			pci_mmcfg_base, pci_mmcfg_base + PCI_MMCONFIG_SIZE, &vm->vpci, false);
	}

	/* Intercept and handle I/O ports CF8h */
	register_pio_emulation_handler(vm, PCI_CFGADDR_PIO_IDX, &pci_cfgaddr_range,
		vpci_pio_cfgaddr_read, vpci_pio_cfgaddr_write);

	/* Intercept and handle I/O ports CFCh -- CFFh */
	register_pio_emulation_handler(vm, PCI_CFGDATA_PIO_IDX, &pci_cfgdata_range,
		vpci_pio_cfgdata_read, vpci_pio_cfgdata_write);

	spinlock_init(&vm->vpci.lock);
}

/**
 * @pre vm != NULL
 * @pre vm->vm_id < CONFIG_MAX_VM_NUM
 */
void deinit_vpci(struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config;

	vm_config = get_vm_config(vm->vm_id);
	switch (vm_config->load_order) {
	case PRE_LAUNCHED_VM:
	case SOS_VM:
		/* deinit function for both SOS and pre-launched VMs (consider sos also as pre-launched) */
		deinit_prelaunched_vm_vpci(vm);
		break;

	case POST_LAUNCHED_VM:
		deinit_postlaunched_vm_vpci(vm);
		break;

	default:
		/* Unsupported VM type - Do nothing */
		break;
	}

	ptdev_release_all_entries(vm);

	/* Free iommu */
	destroy_iommu_domain(vm->iommu);
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 * @pre vdev->vpci->vm->iommu != NULL
 */
static void assign_vdev_pt_iommu_domain(struct pci_vdev *vdev)
{
	int32_t ret;
	struct acrn_vm *vm = vpci2vm(vdev->vpci);

	ret = move_pt_device(NULL, vm->iommu, (uint8_t)vdev->pdev->bdf.bits.b,
		(uint8_t)(vdev->pdev->bdf.value & 0xFFU));
	if (ret != 0) {
		panic("failed to assign iommu device!");
	}
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 * @pre vdev->vpci->vm->iommu != NULL
 */
static void remove_vdev_pt_iommu_domain(const struct pci_vdev *vdev)
{
	int32_t ret;
	const struct acrn_vm *vm = vpci2vm(vdev->vpci);

	ret = move_pt_device(vm->iommu, NULL, (uint8_t)vdev->pdev->bdf.bits.b,
		(uint8_t)(vdev->pdev->bdf.value & 0xFFU));
	if (ret != 0) {
		/*
		 *TODO
		 * panic needs to be removed here
		 * Currently unassign_pt_device can fail for multiple reasons
		 * Once all the reasons and methods to avoid them can be made sure
		 * panic here is not necessary.
		 */
		panic("failed to unassign iommu device!");
	}
}

/**
 * @brief Find an available vdev structure with BDF from a specified vpci structure.
 *        If the vdev's vpci is the same as the specified vpci, the vdev is available.
 *        If the vdev's vpci is not the same as the specified vpci, the vdev has already
 *        been assigned and it is unavailable for SOS.
 *        If the vdev's vpci is NULL, the vdev is a orphan/zombie instance, it can't
 *        be accessed by any vpci.
 *
 * @param vpci Pointer to a specified vpci structure
 * @param bdf  Indicate the vdev's BDF
 *
 * @pre vpci != NULL
 * @pre vpci->vm != NULL
 *
 * @return Return a available vdev instance, otherwise return NULL
 */
static struct pci_vdev *find_available_vdev(struct acrn_vpci *vpci, union pci_bdf bdf)
{
	struct pci_vdev *vdev = pci_find_vdev(vpci, bdf);

	if ((vdev != NULL) && (vdev->vpci != vpci)) {
		/* In the case a device is assigned to a UOS and is not in a zombie state */
		if ((vdev->new_owner != NULL) && (vdev->new_owner->vpci != NULL)) {
			/* the SOS is able to access, if and only if the SOS has higher severity than the UOS. */
			if (get_vm_severity(vpci2vm(vpci)->vm_id) <
					get_vm_severity(vpci2vm(vdev->new_owner->vpci)->vm_id)) {
				vdev = NULL;
			}
		} else {
			vdev = NULL;
		}
	}

	return vdev;
}

static void vpci_init_pt_dev(struct pci_vdev *vdev)
{
	/*
	 * Here init_vdev_pt() needs to be called after init_vmsix() for the following reason:
	 * init_vdev_pt() will indirectly call has_msix_cap(), which
	 * requires init_vmsix() to be called first.
	 */
	init_vmsi(vdev);
	init_vmsix(vdev);
	init_vsriov(vdev);
	init_vdev_pt(vdev, false);

	assign_vdev_pt_iommu_domain(vdev);
}

static void vpci_deinit_pt_dev(struct pci_vdev *vdev)
{
	deinit_vdev_pt(vdev);
	remove_vdev_pt_iommu_domain(vdev);
	deinit_vmsix(vdev);
	deinit_vmsi(vdev);
}

struct cfg_header_perm {
	/* For each 4-byte register defined in PCI config space header,
	 * there is one bit dedicated for it in pt_mask and ro_mask.
	 * For example, bit 0 for CFG Vendor ID and Device ID register,
	 * Bit 1 for CFG register Command and Status register, and so on.
	 *
	 * For each mask, only low 16-bits takes effect.
	 *
	 * If bit x is set the pt_mask, it indicates that the corresponding 4 Bytes register
	 * for bit x is pass through to guest. Otherwise, it's virtualized.
	 *
	 * If bit x is set the ro_mask, it indicates that the corresponding 4 Bytes register
	 * for bit x is read-only. Otherwise, it's writable.
	 */
	uint32_t pt_mask;
	uint32_t ro_mask;
};

static const struct cfg_header_perm cfg_hdr_perm = {
	/* Only Command (0x04-0x05) and Status (0x06-0x07) Registers are pass through */
	.pt_mask = 0x0002U,
	/* Command (0x04-0x05) and Status (0x06-0x07) Registers and
	 * Base Address Registers (0x10-0x27) are writable */
	.ro_mask = (uint16_t)~0x03f2U
};


/*
 * @pre offset + bytes < PCI_CFG_HEADER_LENGTH
 */
static void read_cfg_header(const struct pci_vdev *vdev,
		uint32_t offset, uint32_t bytes, uint32_t *val)
{
	if (vbar_access(vdev, offset)) {
		/* bar access must be 4 bytes and offset must also be 4 bytes aligned */
		if ((bytes == 4U) && ((offset & 0x3U) == 0U)) {
			*val = pci_vdev_read_vbar(vdev, pci_bar_index(offset));
		} else {
			*val = ~0U;
		}
	} else {
		if (bitmap32_test(((uint16_t)offset) >> 2U, &cfg_hdr_perm.pt_mask)) {
			*val = pci_pdev_read_cfg(vdev->pdev->bdf, offset, bytes);

			/* MSE(Memory Space Enable) bit always be set for an assigned VF */
			if ((vdev->phyfun != NULL) && (offset == PCIR_COMMAND) &&
					(vdev->vpci != vdev->phyfun->vpci)) {
				*val |= PCIM_CMD_MEMEN;
			}
		} else {
			*val = pci_vdev_read_vcfg(vdev, offset, bytes);
		}
	}
}

/*
 * @pre offset + bytes < PCI_CFG_HEADER_LENGTH
 */
static void write_cfg_header(struct pci_vdev *vdev,
		uint32_t offset, uint32_t bytes, uint32_t val)
{
	if (vbar_access(vdev, offset)) {
		/* bar write access must be 4 bytes and offset must also be 4 bytes aligned */
		if ((bytes == 4U) && ((offset & 0x3U) == 0U)) {
			vdev_pt_write_vbar(vdev, pci_bar_index(offset), val);
		}
	} else {
		if (offset == PCIR_COMMAND) {
#define PCIM_SPACE_EN (PCIM_CMD_PORTEN | PCIM_CMD_MEMEN)
			uint16_t phys_cmd = (uint16_t)pci_pdev_read_cfg(vdev->pdev->bdf, PCIR_COMMAND, 2U);

			/* check whether need to restore BAR because some kind of reset */
			if (((phys_cmd & PCIM_SPACE_EN) == 0U) && ((val & PCIM_SPACE_EN) != 0U) &&
					pdev_need_bar_restore(vdev->pdev)) {
				pdev_restore_bar(vdev->pdev);
			}
		}

		if (!bitmap32_test(((uint16_t)offset) >> 2U, &cfg_hdr_perm.ro_mask)) {
			if (bitmap32_test(((uint16_t)offset) >> 2U, &cfg_hdr_perm.pt_mask)) {
				pci_pdev_write_cfg(vdev->pdev->bdf, offset, bytes, val);
			} else {
				pci_vdev_write_vcfg(vdev, offset, bytes, val);
			}
		}
	}
}

static int32_t write_pt_dev_cfg(struct pci_vdev *vdev, uint32_t offset,
		uint32_t bytes, uint32_t val)
{
	int32_t ret = 0;

	if (cfg_header_access(offset)) {
		write_cfg_header(vdev, offset, bytes, val);
	} else if (msicap_access(vdev, offset)) {
		write_vmsi_cfg(vdev, offset, bytes, val);
	} else if (msixcap_access(vdev, offset)) {
		write_vmsix_cfg(vdev, offset, bytes, val);
	} else if (sriovcap_access(vdev, offset)) {
		write_sriov_cap_reg(vdev, offset, bytes, val);
	} else {
		if (!is_quirk_ptdev(vdev)) {
			/* passthru to physical device */
			pci_pdev_write_cfg(vdev->pdev->bdf, offset, bytes, val);
		} else {
			ret = -ENODEV;
		}
	}

	return ret;
}

static int32_t read_pt_dev_cfg(const struct pci_vdev *vdev, uint32_t offset,
		uint32_t bytes, uint32_t *val)
{
	int32_t ret = 0;

	if (cfg_header_access(offset)) {
		read_cfg_header(vdev, offset, bytes, val);
	} else if (msicap_access(vdev, offset)) {
		read_vmsi_cfg(vdev, offset, bytes, val);
	} else if (msixcap_access(vdev, offset)) {
		read_vmsix_cfg(vdev, offset, bytes, val);
	} else if (sriovcap_access(vdev, offset)) {
		read_sriov_cap_reg(vdev, offset, bytes, val);
	} else {
		if (!is_quirk_ptdev(vdev)) {
			/* passthru to physical device */
			*val = pci_pdev_read_cfg(vdev->pdev->bdf, offset, bytes);
		} else {
			ret = -ENODEV;
		}
	}

	return ret;
}

static const struct pci_vdev_ops pci_pt_dev_ops = {
	.init_vdev	= vpci_init_pt_dev,
	.deinit_vdev	= vpci_deinit_pt_dev,
	.write_vdev_cfg	= write_pt_dev_cfg,
	.read_vdev_cfg	= read_pt_dev_cfg,
};

/**
 * @pre vpci != NULL
 */
static int32_t vpci_read_cfg(struct acrn_vpci *vpci, union pci_bdf bdf,
	uint32_t offset, uint32_t bytes, uint32_t *val)
{
	int32_t ret = 0;
	struct pci_vdev *vdev;

	spinlock_obtain(&vpci->lock);
	vdev = find_available_vdev(vpci, bdf);
	if (vdev != NULL) {
		ret = vdev->vdev_ops->read_vdev_cfg(vdev, offset, bytes, val);
	} else {
		if (is_postlaunched_vm(vpci2vm(vpci))) {
			ret = -ENODEV;
		}
	}
	spinlock_release(&vpci->lock);
	return ret;
}

/**
 * @pre vpci != NULL
 */
static int32_t vpci_write_cfg(struct acrn_vpci *vpci, union pci_bdf bdf,
	uint32_t offset, uint32_t bytes, uint32_t val)
{
	int32_t ret = 0;
	struct pci_vdev *vdev;

	spinlock_obtain(&vpci->lock);
	vdev = find_available_vdev(vpci, bdf);
	if (vdev != NULL) {
		ret = vdev->vdev_ops->write_vdev_cfg(vdev, offset, bytes, val);
	} else {
		if (!is_postlaunched_vm(vpci2vm(vpci))) {
			pr_acrnlog("%s %x:%x.%x not found! off: 0x%x, val: 0x%x\n", __func__,
				bdf.bits.b, bdf.bits.d, bdf.bits.f, offset, val);
		} else {
			ret = -ENODEV;
		}
	}
	spinlock_release(&vpci->lock);
	return ret;
}

/**
 * @brief Initialize a vdev structure.
 *
 * The function vpci_init_vdev is used to initialize a vdev structure with a PCI device configuration(dev_config)
 * on a specified vPCI bus(vpci). If the function vpci_init_vdev initializes a SRIOV Virtual Function(VF) vdev structure,
 * the parameter parent_pf_vdev is the VF associated Physical Function(PF) vdev structure, otherwise the parameter parent_pf_vdev is NULL.
 * The caller of the function vpci_init_vdev should guarantee execution atomically.
 *
 * @param vpci              Pointer to a vpci structure
 * @param dev_config        Pointer to a dev_config structure of the vdev
 * @param parent_pf_vdev    If the parameter def_config points to a SRIOV VF vdev, this parameter parent_pf_vdev indicates the parent PF vdev.
 *                          Otherwise, it is NULL.
 *
 * @pre vpci != NULL
 * @pre vpci.pci_vdev_cnt <= CONFIG_MAX_PCI_DEV_NUM
 *
 * @return If there's a successfully initialized vdev structure return it, otherwise return NULL;
 */
struct pci_vdev *vpci_init_vdev(struct acrn_vpci *vpci, struct acrn_vm_pci_dev_config *dev_config, struct pci_vdev *parent_pf_vdev)
{
	struct pci_vdev *vdev = &vpci->pci_vdevs[vpci->pci_vdev_cnt];

	vpci->pci_vdev_cnt++;
	vdev->vpci = vpci;
	vdev->bdf.value = dev_config->vbdf.value;
	vdev->pdev = dev_config->pdev;
	vdev->pci_dev_config = dev_config;
	vdev->phyfun = parent_pf_vdev;

	if (dev_config->vdev_ops != NULL) {
		vdev->vdev_ops = dev_config->vdev_ops;
	} else {
		if (vdev->pdev->hdr_type == PCIM_HDRTYPE_BRIDGE) {
			vdev->vdev_ops = &vpci_bridge_ops;
		} else {
			vdev->vdev_ops = &pci_pt_dev_ops;
		}

		ASSERT(dev_config->emu_type == PCI_DEV_TYPE_PTDEV,
			"Only PCI_DEV_TYPE_PTDEV could not configure vdev_ops");
		ASSERT(dev_config->pdev != NULL, "PCI PTDev is not present on platform!");
	}

	vdev->vdev_ops->init_vdev(vdev);

	return vdev;
}

/**
 * @pre vm != NULL
 */
static void vpci_init_vdevs(struct acrn_vm *vm)
{
	uint32_t idx;
	struct acrn_vpci *vpci = &(vm->vpci);
	const struct acrn_vm_config *vm_config = get_vm_config(vpci2vm(vpci)->vm_id);

	for (idx = 0U; idx < vm_config->pci_dev_num; idx++) {
		(void)vpci_init_vdev(vpci, &vm_config->pci_devs[idx], NULL);
	}
}

/**
 * @pre vm != NULL
 * @pre vm->vpci.pci_vdev_cnt <= CONFIG_MAX_PCI_DEV_NUM
 * @pre is_sos_vm(vm) || is_prelaunched_vm(vm)
 */
static void deinit_prelaunched_vm_vpci(struct acrn_vm *vm)
{
	struct pci_vdev *vdev;
	uint32_t i;

	for (i = 0U; i < vm->vpci.pci_vdev_cnt; i++) {
		vdev = (struct pci_vdev *) &(vm->vpci.pci_vdevs[i]);

		/* Only deinit the VM's own devices */
		if (is_own_device(vm, vdev)) {
			vdev->vdev_ops->deinit_vdev(vdev);
		}
	}
}

/**
 * @pre vm != NULL && Pointer vm shall point to SOS_VM
 * @pre vm->vpci.pci_vdev_cnt <= CONFIG_MAX_PCI_DEV_NUM
 * @pre is_postlaunched_vm(vm) == true
 */
static void deinit_postlaunched_vm_vpci(struct acrn_vm *vm)
{
	struct acrn_vm *sos_vm;
	uint32_t i;
	struct pci_vdev *vdev, *target_vdev;
	int32_t ret;
	/* PCI resources
	 * 1) IOMMU domain switch
	 * 2) Relese UOS MSI host IRQ/IRTE
	 * 3) Update vdev info in SOS vdev
	 * Cleanup mentioned above is  taken care when DM releases UOS resources
	 * during a UOS reboot or shutdown
	 * In the following cases, where DM does not get chance to cleanup
	 * 1) DM crash/segfault
	 * 2) SOS triple fault/hang
	 * 3) SOS reboot before shutting down POST_LAUNCHED_VMs
	 * ACRN must cleanup
	 */
	sos_vm = get_sos_vm();
	spinlock_obtain(&sos_vm->vpci.lock);
	for (i = 0U; i < sos_vm->vpci.pci_vdev_cnt; i++) {
		vdev = (struct pci_vdev *)&(sos_vm->vpci.pci_vdevs[i]);

		/* Only deinit the VM's own devices */
		if (is_own_device(vm, vdev)) {
			spinlock_obtain(&vm->vpci.lock);
			target_vdev = vdev->new_owner;
			ret = move_pt_device(vm->iommu, sos_vm->iommu, (uint8_t)target_vdev->pdev->bdf.bits.b,
					(uint8_t)(target_vdev->pdev->bdf.value & 0xFFU));
			if (ret != 0) {
				panic("failed to assign iommu device!");
			}

			deinit_vmsi(target_vdev);

			deinit_vmsix(target_vdev);

			/* Move vdev pointers back to SOS*/
			vdev->vpci = (struct acrn_vpci *) &sos_vm->vpci;
			vdev->new_owner = NULL;
			spinlock_release(&vm->vpci.lock);
		}
	}
	spinlock_release(&sos_vm->vpci.lock);
}

/**
 * @brief assign a PCI device from SOS to target post-launched VM.
 *
 * @pre tgt_vm != NULL
 * @pre pcidev != NULL
 */
int32_t vpci_assign_pcidev(struct acrn_vm *tgt_vm, struct acrn_assign_pcidev *pcidev)
{
	int32_t ret = 0;
	uint32_t idx;
	struct pci_vdev *vdev_in_sos, *vdev;
	struct acrn_vpci *vpci;
	union pci_bdf bdf;
	struct acrn_vm *sos_vm;

	bdf.value = pcidev->phys_bdf;
	sos_vm = get_sos_vm();
	spinlock_obtain(&sos_vm->vpci.lock);
	vdev_in_sos = pci_find_vdev(&sos_vm->vpci, bdf);
	/*
	 * TODO We didn't support SR-IOV capability of PF in UOS for now, we should
	 * hide the SR-IOV capability if we pass through the PF to a UOS.
	 *
	 * For now, we don't support assignment of PF to a UOS.
	 */
	if ((vdev_in_sos != NULL) && is_own_device(sos_vm, vdev_in_sos) && (vdev_in_sos->pdev != NULL) && (!has_sriov_cap(vdev_in_sos))) {
		/* ToDo: Each PT device must support one type reset */
		if (!vdev_in_sos->pdev->has_pm_reset && !vdev_in_sos->pdev->has_flr &&
				!vdev_in_sos->pdev->has_af_flr) {
			pr_fatal("%s %x:%x.%x not support FLR or not support PM reset\n",
				__func__, bdf.bits.b,  bdf.bits.d,  bdf.bits.f);
		} else {
			/* DM will reset this device before assigning it */
			pdev_restore_bar(vdev_in_sos->pdev);
		}

		remove_vdev_pt_iommu_domain(vdev_in_sos);
		if (ret == 0) {
			vpci = &(tgt_vm->vpci);
			vdev_in_sos->vpci = vpci;

			spinlock_obtain(&tgt_vm->vpci.lock);
			vdev = vpci_init_vdev(vpci, vdev_in_sos->pci_dev_config, vdev_in_sos->phyfun);
			pci_vdev_write_vcfg(vdev, PCIR_INTERRUPT_LINE, 1U, pcidev->intr_line);
			pci_vdev_write_vcfg(vdev, PCIR_INTERRUPT_PIN, 1U, pcidev->intr_pin);
			for (idx = 0U; idx < vdev->nr_bars; idx++) {
				/* VF is assigned to a UOS */
				if (vdev->phyfun != NULL) {
					vdev->vbars[idx] = vdev_in_sos->vbars[idx];
					if (has_msix_cap(vdev) && (idx == vdev->msix.table_bar)) {
						vdev->msix.mmio_hpa = vdev->vbars[idx].base_hpa;
						vdev->msix.mmio_size = vdev->vbars[idx].size;
					}
				}
				pci_vdev_write_vbar(vdev, idx, pcidev->bar[idx]);
			}

			vdev->flags |= pcidev->type;
			vdev->bdf.value = pcidev->virt_bdf;
			spinlock_release(&tgt_vm->vpci.lock);
			vdev_in_sos->new_owner = vdev;
		}
	} else {
		pr_fatal("%s, can't find PCI device %x:%x.%x for vm[%d] %x:%x.%x\n", __func__,
			pcidev->phys_bdf >> 8U, (pcidev->phys_bdf >> 3U) & 0x1fU, pcidev->phys_bdf & 0x7U,
			tgt_vm->vm_id,
			pcidev->virt_bdf >> 8U, (pcidev->virt_bdf >> 3U) & 0x1fU, pcidev->virt_bdf & 0x7U);
		ret = -ENODEV;
	}
	spinlock_release(&sos_vm->vpci.lock);

	return ret;
}

/**
 * @brief deassign a PCI device from target post-launched VM to SOS.
 *
 * @pre tgt_vm != NULL
 * @pre pcidev != NULL
 */
int32_t vpci_deassign_pcidev(struct acrn_vm *tgt_vm, struct acrn_assign_pcidev *pcidev)
{
	int32_t ret = 0;
	struct pci_vdev *vdev_in_sos, *vdev;
	union pci_bdf bdf;
	struct acrn_vm *sos_vm;

	bdf.value = pcidev->phys_bdf;
	sos_vm = get_sos_vm();
	spinlock_obtain(&sos_vm->vpci.lock);
	vdev_in_sos = pci_find_vdev(&sos_vm->vpci, bdf);
	if ((vdev_in_sos != NULL) && is_own_device(tgt_vm, vdev_in_sos) && (vdev_in_sos->pdev != NULL)) {
		vdev = vdev_in_sos->new_owner;

		spinlock_obtain(&tgt_vm->vpci.lock);

		if (vdev != NULL) {
			ret = move_pt_device(tgt_vm->iommu, sos_vm->iommu, (uint8_t)(pcidev->phys_bdf >> 8U),
					(uint8_t)(pcidev->phys_bdf & 0xffU));
			if (ret != 0) {
				panic("failed to assign iommu device!");
			}

			deinit_vmsi(vdev);

			deinit_vmsix(vdev);
		}
		spinlock_release(&tgt_vm->vpci.lock);

		vdev_in_sos->vpci = &sos_vm->vpci;
		vdev_in_sos->new_owner = NULL;
	} else {
		pr_fatal("%s, can't find PCI device %x:%x.%x for vm[%d] %x:%x.%x\n", __func__,
			pcidev->phys_bdf >> 8U, (pcidev->phys_bdf >> 3U) & 0x1fU, pcidev->phys_bdf & 0x7U,
			tgt_vm->vm_id,
			pcidev->virt_bdf >> 8U, (pcidev->virt_bdf >> 3U) & 0x1fU, pcidev->virt_bdf & 0x7U);
		ret = -ENODEV;
	}
	spinlock_release(&sos_vm->vpci.lock);

	return ret;
}
