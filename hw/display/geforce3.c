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

/* Standard VBE constants */
#define VBE_DISPI_IOPORT_INDEX          0x01CE
#define VBE_DISPI_IOPORT_DATA           0x01CF

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

/* VBE 2.0 IDs */
#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

/* VBE Enable flags */
#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

/* GeForce VBE extensions */
#define GEFORCE_VBE_EXT_BASE            0x0200
#define GEFORCE_VBE_VRAM_SIZE           (GEFORCE_VBE_EXT_BASE + 0)
#define GEFORCE_VBE_CAPABILITY          (GEFORCE_VBE_EXT_BASE + 1)
#define GEFORCE_VBE_DDC_CONTROL         (GEFORCE_VBE_EXT_BASE + 2)

/* NV20 (GeForce3) architecture constants */
#define NV_ARCH_20              0x20
#define NV_IMPL_GEFORCE3        0x00
#define NV_IMPL_GEFORCE3_TI200  0x01
#define NV_IMPL_GEFORCE3_TI500  0x02

typedef struct NVGFState {
    PCIDevice parent_obj;
    
    /* VGA compatibility */
    VGACommonState vga;
    
    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion lfb;
    MemoryRegion crtc;
    
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
    MemoryRegion vbe_region; /* VBE I/O region */
    
    /* VBE state tracking */
    uint16_t vbe_xres;
    uint16_t vbe_yres;
    uint16_t vbe_bpp;
    uint16_t vbe_enable;
    
    /* NVIDIA-specific registers */
    uint32_t pmc_boot_0;
    uint32_t pmc_intr_0;
    uint32_t pmc_intr_en_0;
    uint32_t architecture;
    uint32_t implementation;
    
} NVGFState;

/* Forward declarations */
static void geforce_ddc_init(NVGFState *s);
static uint64_t geforce_ddc_read(void *opaque, hwaddr addr, unsigned size);
static void geforce_ddc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
static void geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info);
static uint32_t nv_compute_boot0(NVGFState *s);
static void nv_apply_model_ids(NVGFState *s);
static uint64_t nv_bar0_readl(void *opaque, hwaddr addr, unsigned size);

/* VBE function declarations */
static uint64_t geforce_vbe_read(void *opaque, hwaddr addr, unsigned size);
static void geforce_vbe_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
static void geforce_sync_vbe_state(NVGFState *s);
static void geforce_init_vbe(NVGFState *s);
static bool geforce_handles_vbe_register(uint16_t index);
static void geforce_set_vbe_mode(NVGFState *s, uint16_t mode);

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

static const MemoryRegionOps geforce_vga_ops = {
    .read = geforce_vga_ioport_read,
    .write = geforce_vga_ioport_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* VBE I/O operations - integrates GeForce with standard VGA VBE */
static bool geforce_handles_vbe_register(uint16_t index)
{
    /* Check if this is a GeForce-specific VBE register */
    return (index >= GEFORCE_VBE_EXT_BASE && index < GEFORCE_VBE_EXT_BASE + 16);
}

static uint64_t geforce_vbe_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    VGACommonState *vga = &s->vga;
    
    switch (addr) {
    case 0: /* VBE_DISPI_IOPORT_INDEX - 0x01CE */
        return s->vbe_index;
        
    case 1: /* VBE_DISPI_IOPORT_DATA - 0x01CF */
        if (geforce_handles_vbe_register(s->vbe_index)) {
            /* Handle GeForce-specific VBE registers */
            switch (s->vbe_index) {
            case GEFORCE_VBE_VRAM_SIZE:
                return vga->vram_size >> 16; /* Return VRAM size in 64KB units */
            case GEFORCE_VBE_CAPABILITY:
                return VBE_DISPI_ID5; /* GeForce3 supports VBE 2.0+ */
            case GEFORCE_VBE_DDC_CONTROL:
                return s->ddc_state;
            default:
                return 0xFFFF;
            }
        } else if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
            /* Handle standard VBE registers through our local state */
            switch (s->vbe_index) {
            case VBE_DISPI_INDEX_ID:
                return VBE_DISPI_ID5;
            case VBE_DISPI_INDEX_XRES:
                return s->vbe_xres;
            case VBE_DISPI_INDEX_YRES:
                return s->vbe_yres;
            case VBE_DISPI_INDEX_BPP:
                return s->vbe_bpp;
            case VBE_DISPI_INDEX_ENABLE:
                return s->vbe_enable;
            default:
                /* Fall back to VGA VBE for other standard registers */
                if (vga->vbe_regs && s->vbe_index < 16) {
                    return vga->vbe_regs[s->vbe_index];
                }
                return 0;
            }
        }
        return 0;
        
    default:
        return 0;
    }
}

static void geforce_vbe_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    VGACommonState *vga = &s->vga;
    
    switch (addr) {
    case 0: /* VBE_DISPI_IOPORT_INDEX - 0x01CE */
        s->vbe_index = val;
        break;
        
    case 1: /* VBE_DISPI_IOPORT_DATA - 0x01CF */
        if (geforce_handles_vbe_register(s->vbe_index)) {
            /* Handle GeForce-specific VBE registers */
            switch (s->vbe_index) {
            case GEFORCE_VBE_DDC_CONTROL:
                s->ddc_state = val;
                break;
            default:
                /* Read-only registers, ignore writes */
                break;
            }
        } else if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
            /* Handle standard VBE registers */
            switch (s->vbe_index) {
            case VBE_DISPI_INDEX_XRES:
                s->vbe_xres = val;
                break;
            case VBE_DISPI_INDEX_YRES:
                s->vbe_yres = val;
                break;
            case VBE_DISPI_INDEX_BPP:
                s->vbe_bpp = val;
                break;
            case VBE_DISPI_INDEX_ENABLE:
                s->vbe_enable = val;
                if (val & VBE_DISPI_ENABLED) {
                    /* Mode change requested */
                    geforce_set_vbe_mode(s, (s->vbe_yres << 16) | s->vbe_xres);
                }
                break;
            default:
                /* Forward other standard VBE writes to VGA */
                if (vga->vbe_regs && s->vbe_index < 16) {
                    vga->vbe_regs[s->vbe_index] = val;
                }
                break;
            }
            
            /* Store in our local register array */
            if (s->vbe_index < ARRAY_SIZE(s->vbe_regs)) {
                s->vbe_regs[s->vbe_index] = val;
            }
        }
        
        /* Always synchronize state after VBE writes */
        geforce_sync_vbe_state(s);
        break;
        
    default:
        break;
    }
}

static const MemoryRegionOps geforce_vbe_ops = {
    .read = geforce_vbe_read,
    .write = geforce_vbe_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Synchronize VBE state between GeForce and VGA */
static void geforce_sync_vbe_state(NVGFState *s)
{
    VGACommonState *vga = &s->vga;
    
    if (!vga->vbe_regs) {
        return;
    }
    
    /* Update standard VBE registers to match GeForce state */
    if (VBE_DISPI_INDEX_XRES < 16) {
        vga->vbe_regs[VBE_DISPI_INDEX_XRES] = s->vbe_xres;
    }
    if (VBE_DISPI_INDEX_YRES < 16) {
        vga->vbe_regs[VBE_DISPI_INDEX_YRES] = s->vbe_yres;
    }
    if (VBE_DISPI_INDEX_BPP < 16) {
        vga->vbe_regs[VBE_DISPI_INDEX_BPP] = s->vbe_bpp;
    }
    if (VBE_DISPI_INDEX_ENABLE < 16) {
        vga->vbe_regs[VBE_DISPI_INDEX_ENABLE] = s->vbe_enable;
    }
}

/* GeForce VBE mode setting */
static void geforce_set_vbe_mode(NVGFState *s, uint16_t mode)
{
    VGACommonState *vga = &s->vga;
    
    /* Basic validation */
    if (s->vbe_xres == 0 || s->vbe_yres == 0 || s->vbe_bpp == 0) {
        return;
    }
    
    /* TODO: Add GeForce-specific mode validation and configuration */
    
    /* Update VGA state */
    if (vga->vbe_regs) {
        /* Ensure VGA knows about the mode change */
        vga->vbe_start_addr = 0;
        vga->vbe_line_offset = (s->vbe_xres * s->vbe_bpp) / 8;
        vga->vbe_bank_mask = 0;
    }
    
    /* Synchronize states */
    geforce_sync_vbe_state(s);
    
    /* Trigger display update */
    if (vga->con) {
        dpy_gfx_update(vga->con, 0, 0, s->vbe_xres, s->vbe_yres);
    }
}

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

/* Initialize integrated VBE support */
static void geforce_init_vbe(NVGFState *s)
{
    VGACommonState *vga = &s->vga;
    
    /* Initialize VBE state with default values */
    s->vbe_index = 0;
    s->vbe_xres = 1024;
    s->vbe_yres = 768;
    s->vbe_bpp = 32;
    s->vbe_enable = VBE_DISPI_DISABLED;
    
    /* Initialize VBE register array */
    memset(s->vbe_regs, 0, sizeof(s->vbe_regs));
    s->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s->vbe_regs[VBE_DISPI_INDEX_XRES] = s->vbe_xres;
    s->vbe_regs[VBE_DISPI_INDEX_YRES] = s->vbe_yres;
    s->vbe_regs[VBE_DISPI_INDEX_BPP] = s->vbe_bpp;
    s->vbe_regs[VBE_DISPI_INDEX_ENABLE] = s->vbe_enable;
    
    /* Remove standard VGA VBE region if it exists */
    if (memory_region_is_mapped(&vga->vbe_region)) {
        memory_region_del_subregion(vga->vbe_region.container, &vga->vbe_region);
    }
    
    /* Add GeForce-integrated VBE region */
    memory_region_init_io(&s->vbe_region, OBJECT(s), &geforce_vbe_ops, s,
                         "geforce-vbe", 2);
    memory_region_add_subregion(pci_address_space_io(PCI_DEVICE(s)), VBE_DISPI_IOPORT_INDEX,
                               &s->vbe_region);
    
    /* Synchronize with VGA state */
    geforce_sync_vbe_state(s);
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
    
    /* Initialize integrated VBE support */
    geforce_init_vbe(s);
    
    /* FIX: Register UI info callback for dynamic EDID - Remove & from hw_ops to fix incompatible pointer types */
    vga->con = graphic_console_init(DEVICE(pci_dev), 0, vga->hw_ops, vga);
    qemu_console_set_display_gl_ctx(vga->con, NULL);
    
    /* FIX: Set up UI info callback - Use dpy_set_ui_info instead of qemu_console_set_ui_info */
    if (vga->con) {
        dpy_set_ui_info(vga->con, geforce_ui_info, s);
    }
}

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
    dc->vmsd = &vmstate_vga_common;
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