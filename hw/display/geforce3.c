/*
 * NVIDIA GeForce3 Graphics Card Emulation for QEMU
 * 
 * Copyright (c) 2025 QEMU Project
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/display/vga.h"
#include "hw/display/vga_int.h"
#include "hw/display/edid.h"
#include "hw/i2c/i2c.h"
#include "qapi/error.h"
#include "ui/console.h"

#define TYPE_GEFORCE3 "geforce3"
OBJECT_DECLARE_SIMPLE_TYPE(NVGFState, GEFORCE3)

/* GeForce3 PCI IDs */
#define NVIDIA_VENDOR_ID        0x10de
#define GEFORCE3_DEVICE_ID      0x0200

/* MMIO ranges */
#define NV_PRMVIO_SIZE          0x1000
#define NV_LFB_SIZE             0x1000000  /* 16MB frame buffer */
#define NV_CRTC_SIZE            0x1000

/* DDC/I2C constants */
#define DDC_SDA_PIN             0x01
#define DDC_SCL_PIN             0x02

/* NVIDIA register offsets */
#define NV_PMC_BOOT_0           0x000000
#define NV_PMC_INTR_0           0x000100
#define NV_PMC_INTR_EN_0        0x000140
#define NV_PBUS_PCI_NV_1        0x001804

/* NV20 (GeForce3) architecture constants */
#define NV_ARCH_20              0x20
#define NV_IMPL_GEFORCE3        0x00
#define NV_IMPL_GEFORCE3_TI200  0x01
#define NV_IMPL_GEFORCE3_TI500  0x02

/* VBE I/O Ports */
#define VBE_DISPI_IOPORT_INDEX          0x01CE
#define VBE_DISPI_IOPORT_DATA           0x01CF

/* VBE Registers */
#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9
#define VBE_DISPI_INDEX_NB              0xa

/* VBE IDs */
#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

/* VBE Enable bits */
#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

typedef struct NVGFState {
    PCIDevice parent_obj;
    
    /* VGA compatibility */
    VGACommonState vga;
    
    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion lfb;
    MemoryRegion crtc;
    MemoryRegion vbe_region;
    
    /* DDC/I2C support */
    I2CBus *i2c_bus;
    I2CSlave *i2c_ddc;
    uint8_t ddc_state;
    
    /* EDID support */
    qemu_edid_info edid_info;
    uint8_t edid_blob[256];
    bool edid_enabled;
    
    /* Device registers */
    uint32_t prmvio[NV_PRMVIO_SIZE / 4];
    
    /* VBE support */
    uint16_t vbe_index;
    uint16_t vbe_regs[16]; /* VBE register array */
    bool vbe_enabled;      /* VBE mode enabled flag */
    bool vbe_fallback_active; /* Whether VBE fallback to standard VGA is active */
    
    /* NVIDIA-specific registers */
    uint32_t pmc_boot_0;
    uint32_t pmc_intr_0;
    uint32_t pmc_intr_en_0;
    uint32_t architecture;
    uint32_t implementation;
    
} NVGFState;

/* Forward declarations */
static void geforce_ddc_init(NVGFState *s);
static void geforce_vbe_init(NVGFState *s);
static uint64_t geforce_ddc_read(void *opaque, hwaddr addr, unsigned size);
static void geforce_ddc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
static void geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info);
static uint32_t nv_compute_boot0(NVGFState *s);
static void nv_apply_model_ids(NVGFState *s);
static uint64_t nv_bar0_readl(void *opaque, hwaddr addr, unsigned size);

/* VGA I/O operations */
static uint64_t geforce_vga_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    return vga_ioport_read(&s->vga, addr);
}

static void geforce_vga_ioport_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    vga_ioport_write(&s->vga, addr, val);
}

/* VBE fallback functions */
static uint64_t geforce_vbe_read_fallback(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle VBE I/O port reads */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        return s->vbe_index;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        /* Try GeForce-specific VBE first */
        if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
            /* Handle GeForce-specific VBE registers */
            switch (s->vbe_index) {
            case VBE_DISPI_INDEX_ID:
                return VBE_DISPI_ID5; /* Report latest VBE version */
            case VBE_DISPI_INDEX_ENABLE:
                return s->vbe_enabled ? VBE_DISPI_ENABLED : VBE_DISPI_DISABLED;
            default:
                if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
                    return s->vbe_regs[s->vbe_index];
                }
                break;
            }
        }
        
        /* Fall back to standard VGA VBE if GeForce VBE doesn't handle it */
        s->vbe_fallback_active = true;
        return vga_ioport_read(&s->vga, addr);
    }
    
    /* For non-VBE ports, use standard VGA handling */
    return vga_ioport_read(&s->vga, addr);
}

static void geforce_vbe_write_fallback(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle VBE I/O port writes */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        s->vbe_index = val;
        s->vbe_fallback_active = false; /* Reset fallback state on index change */
        return;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        /* Try GeForce-specific VBE first */
        if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
            /* Handle GeForce-specific VBE registers */
            switch (s->vbe_index) {
            case VBE_DISPI_INDEX_ENABLE:
                s->vbe_enabled = (val & VBE_DISPI_ENABLED) != 0;
                s->vbe_regs[s->vbe_index] = val;
                return;
            case VBE_DISPI_INDEX_XRES:
            case VBE_DISPI_INDEX_YRES:
            case VBE_DISPI_INDEX_BPP:
            case VBE_DISPI_INDEX_BANK:
            case VBE_DISPI_INDEX_VIRT_WIDTH:
            case VBE_DISPI_INDEX_VIRT_HEIGHT:
            case VBE_DISPI_INDEX_X_OFFSET:
            case VBE_DISPI_INDEX_Y_OFFSET:
                s->vbe_regs[s->vbe_index] = val;
                return;
            default:
                break;
            }
        }
        
        /* Fall back to standard VGA VBE if GeForce VBE doesn't handle it */
        s->vbe_fallback_active = true;
        vga_ioport_write(&s->vga, addr, val);
        return;
    }
    
    /* For non-VBE ports, use standard VGA handling */
    vga_ioport_write(&s->vga, addr, val);
}

static const MemoryRegionOps geforce_vga_ops = {
    .read = geforce_vbe_read_fallback,
    .write = geforce_vbe_write_fallback,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Compute PMC_BOOT_0 register value for nouveau driver compatibility */
static uint32_t nv_compute_boot0(NVGFState *s)
{
    /* PMC_BOOT_0 format for nouveau driver:
     * Bits 31-20: Architecture (0x20 for NV20/GeForce3)
     * Bits 19-16: Implementation (0x00 for standard GeForce3)
     * Bits 15-0:  Device/revision identification
     * 
     * For GeForce3 (NV20), nouveau expects: 0x20200XXX
     * Where XXX contains device-specific revision information
     */
    uint32_t boot0 = (s->architecture << 20) | (s->architecture << 16) | (s->implementation << 4) | 0x00;
    return boot0;
}

/* Apply device ID mappings for nouveau compatibility */
static void nv_apply_model_ids(NVGFState *s)
{
    /* Set GeForce3 NV20 architecture */
    s->architecture = NV_ARCH_20;
    s->implementation = NV_IMPL_GEFORCE3;
    
    /* Compute PMC_BOOT_0 register */
    s->pmc_boot_0 = nv_compute_boot0(s);
    
    /* Initialize other PMC registers */
    s->pmc_intr_0 = 0x00000000;    /* No interrupts pending */
    s->pmc_intr_en_0 = 0x00000000; /* Interrupts disabled initially */
}

/* BAR0 register read handler for nouveau compatibility */
static uint64_t nv_bar0_readl(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* Critical register for nouveau chipset detection */
        return s->pmc_boot_0;
        
    case NV_PMC_INTR_0:
        /* Interrupt status register */
        return s->pmc_intr_0;
        
    case NV_PMC_INTR_EN_0:
        /* Interrupt enable register */
        return s->pmc_intr_en_0;
        
    case NV_PBUS_PCI_NV_1:
        /* PCI configuration mirror */
        return (NVIDIA_VENDOR_ID << 16) | GEFORCE3_DEVICE_ID;
        
    default:
        /* For unhandled registers, check if it's in PRMVIO range */
        if (addr < NV_PRMVIO_SIZE) {
            uint32_t reg = addr / 4;
            if (reg < ARRAY_SIZE(s->prmvio)) {
                return s->prmvio[reg];
            }
        }
        return 0;
    }
}

/* PRMVIO (VGA mirrors) operations */
static uint64_t geforce_prmvio_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Use the comprehensive BAR0 register handler */
    return nv_bar0_readl(opaque, addr, size);
}

static void geforce_prmvio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    switch (addr) {
    case NV_PMC_INTR_0:
        /* Interrupt status register - write to clear */
        s->pmc_intr_0 &= ~val;
        break;
        
    case NV_PMC_INTR_EN_0:
        /* Interrupt enable register */
        s->pmc_intr_en_0 = val;
        break;
        
    default:
        /* Handle generic PRMVIO register writes */
        if (addr < NV_PRMVIO_SIZE) {
            uint32_t reg = addr / 4;
            if (reg < ARRAY_SIZE(s->prmvio)) {
                s->prmvio[reg] = val;
            }
        }
        break;
    }
}

static const MemoryRegionOps geforce_prmvio_ops = {
    .read = geforce_prmvio_read,
    .write = geforce_prmvio_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* CRTC operations */
static uint64_t geforce_crtc_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle DDC reads */
    if (addr >= 0x50 && addr < 0x60) {
        return geforce_ddc_read(s, addr - 0x50, size);
    }
    
    /* Basic CRTC register read */
    switch (addr) {
    case 0x00: /* CRTC status */
        return 0x01; /* Not in VBlank */
    default:
        return 0;
    }
}

static void geforce_crtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle DDC writes */
    if (addr >= 0x50 && addr < 0x60) {
        geforce_ddc_write(s, addr - 0x50, val, size);
        return;
    }
    
    /* Basic CRTC register write */
    switch (addr) {
    case 0x00: /* CRTC control */
        /* Handle CRTC control */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps geforce_crtc_ops = {
    .read = geforce_crtc_read,
    .write = geforce_crtc_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* DDC/I2C implementation */
static void geforce_ddc_init(NVGFState *s)
{
    /* Initialize I2C bus for DDC */
    s->i2c_bus = i2c_init_bus(DEVICE(s), "ddc");
    /* TODO: Enable when I2C DDC support is available */
    /* s->i2c_ddc = i2c_slave_create_simple(s->i2c_bus, TYPE_I2CDDC, 0x50); */
    s->i2c_ddc = NULL;
    
    /* FIX: Initialize EDID with default values - Fixed vendor assignment to avoid const char* to void* issue */
    uint8_t vendor_id[4] = {' ', 'N', 'V', 'D'};
    memcpy(s->edid_info.vendor, vendor_id, sizeof(vendor_id));
    s->edid_info.name = "GeForce3";
    s->edid_info.serial = "12345678";
    s->edid_info.prefx = 1024;
    s->edid_info.prefy = 768;
    s->edid_info.maxx = 1600;
    s->edid_info.maxy = 1200;
    
    /* Generate initial EDID blob */
    qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
    s->edid_enabled = true;
    
    /* Set EDID data in DDC device */
    if (s->i2c_ddc) {
        /* I2CDDCState *ddc = I2CDDC(s->i2c_ddc); */
        /* TODO: Enable when i2c_ddc_set_edid is available */
        /* i2c_ddc_set_edid(ddc, s->edid_blob, sizeof(s->edid_blob)); */
    }
}

static uint64_t geforce_ddc_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    if (!s->edid_enabled || !s->i2c_bus) {
        return 0xff;
    }
    
    switch (addr) {
    case 0x00: /* DDC data */
        return i2c_recv(s->i2c_bus);
    case 0x04: /* DDC control/status */
        return s->ddc_state;
    default:
        return 0xff;
    }
}

static void geforce_ddc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    if (!s->edid_enabled || !s->i2c_bus) {
        return;
    }
    
    switch (addr) {
    case 0x00: /* DDC data */
        i2c_send(s->i2c_bus, val);
        break;
    case 0x04: /* DDC control */
        s->ddc_state = val;
        if (val & DDC_SCL_PIN) {
            i2c_start_transfer(s->i2c_bus, (val & DDC_SDA_PIN) ? 0x51 : 0x50, 0);
        }
        break;
    default:
        break;
    }
}

/* VBE initialization */
static void geforce_vbe_init(NVGFState *s)
{
    /* Initialize VBE state */
    s->vbe_index = 0;
    s->vbe_enabled = false;
    s->vbe_fallback_active = false;
    
    /* Initialize VBE registers with default values */
    memset(s->vbe_regs, 0, sizeof(s->vbe_regs));
    
    /* Set up default VBE capabilities */
    s->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s->vbe_regs[VBE_DISPI_INDEX_XRES] = 1024;
    s->vbe_regs[VBE_DISPI_INDEX_YRES] = 768;
    s->vbe_regs[VBE_DISPI_INDEX_BPP] = 32;
    s->vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_DISABLED;
    s->vbe_regs[VBE_DISPI_INDEX_BANK] = 0;
    s->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = 1024;
    s->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = 768;
    s->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
    s->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
}

/* UI info callback for dynamic EDID */
static void geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    NVGFState *s = opaque;
    
    if (!s->edid_enabled) {
        return;
    }
    
    /* Update EDID info with new display information */
    if (info->width && info->height) {
        s->edid_info.prefx = info->width;
        s->edid_info.prefy = info->height;
        s->edid_info.maxx = MAX(info->width, s->edid_info.maxx);
        s->edid_info.maxy = MAX(info->height, s->edid_info.maxy);
        
        /* Regenerate EDID blob */
        qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
        
        /* Update DDC device with new EDID */
        if (s->i2c_ddc) {
            /* I2CDDCState *ddc = I2CDDC(s->i2c_ddc); */
            /* TODO: Enable when i2c_ddc_set_edid is available */
            /* i2c_ddc_set_edid(ddc, s->edid_blob, sizeof(s->edid_blob)); */
        }
    }
}

/* Device initialization */
static void nv_realize(PCIDevice *pci_dev, Error **errp)
{
    NVGFState *s = GEFORCE3(pci_dev);
    VGACommonState *vga = &s->vga;
    
    /* Initialize NVIDIA-specific registers first */
    nv_apply_model_ids(s);
    
    /* FIX: Initialize VGA - Add missing Error** parameter to vga_common_init call */
    vga_common_init(vga, OBJECT(s), errp);
    vga_init(vga, OBJECT(s), pci_address_space(pci_dev), 
              pci_address_space_io(pci_dev), true);
    
    /* Set up PCI configuration */
    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
    
    /* Initialize memory regions */
    memory_region_init_io(&s->mmio, OBJECT(s), &geforce_prmvio_ops, s,
                          "geforce3-prmvio", NV_PRMVIO_SIZE);
    memory_region_init_io(&s->crtc, OBJECT(s), &geforce_crtc_ops, s,
                          "geforce3-crtc", NV_CRTC_SIZE);
    
    /* Map memory regions */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->mmio);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_MEM_TYPE_32, &vga->vram);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->crtc);
    
    /* Initialize DDC and EDID */
    geforce_ddc_init(s);
    
    /* Initialize VBE after VGA init */
    geforce_vbe_init(s);
    
    /* Register VBE I/O ports for direct access */
    memory_region_init_io(&s->vbe_region, OBJECT(s), &geforce_vga_ops, s,
                          "geforce3-vbe", 2);
    memory_region_add_subregion(pci_address_space_io(pci_dev), 
                                VBE_DISPI_IOPORT_INDEX, &s->vbe_region);
    
    /* FIX: Register UI info callback for dynamic EDID - Remove & from hw_ops to fix incompatible pointer types */
    vga->con = graphic_console_init(DEVICE(pci_dev), 0, vga->hw_ops, vga);
    qemu_console_set_display_gl_ctx(vga->con, NULL);
    
    /* FIX: Set up UI info callback - Use dpy_set_ui_info instead of qemu_console_set_ui_info */
    if (vga->con) {
        dpy_set_ui_info(vga->con, geforce_ui_info, s);
    }
}

/* VMState for GeForce3 with VBE support */
static const VMStateDescription vmstate_geforce3 = {
    .name = "geforce3",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, NVGFState),
        VMSTATE_STRUCT(vga, NVGFState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_UINT32_ARRAY(prmvio, NVGFState, NV_PRMVIO_SIZE / 4),
        VMSTATE_UINT32(pmc_boot_0, NVGFState),
        VMSTATE_UINT32(pmc_intr_0, NVGFState),
        VMSTATE_UINT32(pmc_intr_en_0, NVGFState),
        VMSTATE_UINT32(architecture, NVGFState),
        VMSTATE_UINT32(implementation, NVGFState),
        VMSTATE_UINT16(vbe_index, NVGFState),
        VMSTATE_UINT16_ARRAY(vbe_regs, NVGFState, 16),
        VMSTATE_BOOL(vbe_enabled, NVGFState),
        VMSTATE_BOOL(vbe_fallback_active, NVGFState),
        VMSTATE_END_OF_LIST()
    }
};

/* FIX: Update function signature to match expected prototype for class_init */
static void nv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    
    k->realize = nv_realize;
    k->vendor_id = NVIDIA_VENDOR_ID;
    k->device_id = GEFORCE3_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->subsystem_vendor_id = NVIDIA_VENDOR_ID;
    k->subsystem_id = GEFORCE3_DEVICE_ID;
    
    dc->desc = "NVIDIA GeForce3 Graphics Card";
    /* FIX: Modern QEMU uses device_class_set_parent_reset instead of dc->reset */
    /* dc->reset = vga_common_reset; */ /* Removed to fix "no member named 'reset'" error */
    dc->vmsd = &vmstate_geforce3;
    dc->hotpluggable = false;
    
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo geforce3_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NVGFState),
    .class_init = nv_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void geforce3_register_types(void)
{
    type_register_static(&geforce3_info);
}

type_init(geforce3_register_types);