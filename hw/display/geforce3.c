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
#include "qapi/visitor.h"  /* For property visitor support */

#define TYPE_GEFORCE3 "geforce3"
OBJECT_DECLARE_SIMPLE_TYPE(NVGFState, GEFORCE3)

/* GeForce3 PCI IDs and Model Support */
#define NVIDIA_VENDOR_ID        0x10de
#define GEFORCE3_DEVICE_ID      0x0200

/* Extended GeForce model support - all major GeForce variants */
#define GEFORCE_DDR_DEVICE_ID   0x0100
#define GEFORCE2_MX_DEVICE_ID   0x0110
#define GEFORCE2_GTS_DEVICE_ID  0x0150
#define GEFORCE3_TI200_DEVICE_ID 0x0201
#define GEFORCE3_TI500_DEVICE_ID 0x0202
#define GEFORCE4_MX_DEVICE_ID   0x0170
#define GEFORCE4_TI_DEVICE_ID   0x0250

/* MMIO ranges */
#define NV_PRMVIO_SIZE          0x1000
#define NV_LFB_SIZE             0x1000000  /* 16MB frame buffer */
#define NV_CRTC_SIZE            0x1000

/* VRAM Size Support - 64MB to 512MB as supported by hardware */
#define GEFORCE_MIN_VRAM_SIZE   (64 * 1024 * 1024)   /* 64MB */
#define GEFORCE_MAX_VRAM_SIZE   (512 * 1024 * 1024)  /* 512MB */
#define GEFORCE_DEFAULT_VRAM_SIZE (128 * 1024 * 1024) /* 128MB default */

/* DDC/I2C constants */
#define DDC_SDA_PIN             0x01
#define DDC_SCL_PIN             0x02

/* NVIDIA register offsets */
#define NV_PMC_BOOT_0           0x000000
#define NV_PMC_INTR_0           0x000100
#define NV_PMC_INTR_EN_0        0x000140
#define NV_PBUS_PCI_NV_1        0x001804

/* VBE Support - Standard VBE register definitions for fallback */
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

#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

/* NV20 (GeForce3) architecture constants */
#define NV_ARCH_20              0x20
#define NV_IMPL_GEFORCE3        0x00
#define NV_IMPL_GEFORCE3_TI200  0x01
#define NV_IMPL_GEFORCE3_TI500  0x02

/* GeForce Model Identifiers for -device geforce,model=xxx support */
typedef enum {
    GEFORCE_MODEL_GEFORCE_DDR = 0,
    GEFORCE_MODEL_GEFORCE2_MX,
    GEFORCE_MODEL_GEFORCE2_GTS, 
    GEFORCE_MODEL_GEFORCE3,
    GEFORCE_MODEL_GEFORCE3_TI200,
    GEFORCE_MODEL_GEFORCE3_TI500,
    GEFORCE_MODEL_GEFORCE4_MX,
    GEFORCE_MODEL_GEFORCE4_TI,
    GEFORCE_MODEL_MAX
} GeForceModel;

typedef struct NVGFState {
    PCIDevice parent_obj;
    
    /* VGA compatibility */
    VGACommonState vga;
    
    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion lfb;
    MemoryRegion crtc;
    MemoryRegion vbe_region;  /* VBE fallback support - additive to existing VGA */
    
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
    
    /* VBE support - additive VBE fallback from QEMU VGA STD */
    uint16_t vbe_index;
    uint16_t vbe_regs[VBE_DISPI_INDEX_NB]; /* Extended VBE register array */
    bool vbe_enabled;
    bool vbe_fallback_active;  /* Tracks when VBE fallback is in use */
    
    /* NVIDIA-specific registers */
    uint32_t pmc_boot_0;
    uint32_t pmc_intr_0;
    uint32_t pmc_intr_en_0;
    uint32_t architecture;
    uint32_t implementation;
    
    /* Device Configuration Properties - support for -device geforce,model=xxx,vramsize=xxM,romfile= */
    GeForceModel model;
    uint32_t vram_size_mb;  /* VRAM size in MB */
    char *model_name;       /* String model name */
    char *romfile;          /* Optional ROM file path */
    
    /* Logging throttle state - prevent log spam while maintaining comprehensive logging */
    uint32_t mmio_read_throttle;
    uint32_t mmio_write_throttle;
    uint32_t vbe_access_throttle;
    
} NVGFState;

/* Forward declarations */
static void geforce_ddc_init(NVGFState *s);
static uint64_t geforce_ddc_read(void *opaque, hwaddr addr, unsigned size);
static void geforce_ddc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);
static void geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info);
static uint32_t nv_compute_boot0(NVGFState *s);
static void nv_apply_model_ids(NVGFState *s);
static uint64_t nv_bar0_readl(void *opaque, hwaddr addr, unsigned size);

/* VGA I/O operations with comprehensive logging and VBE fallback */
static uint64_t geforce_vga_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint64_t ret;
    
    /* Comprehensive logging: all legacy port accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VGA legacy port read addr=0x%04lx size=%d ", addr, size);
    
    /* Primary VGA compatibility through existing VGA layer */
    ret = vga_ioport_read(&s->vga, addr);
    
    qemu_log_mask(LOG_GUEST_ERROR, "value=0x%08lx\n", ret);
    
    return ret;
}

static void geforce_vga_ioport_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Comprehensive logging: all legacy port accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VGA legacy port write addr=0x%04lx value=0x%08lx size=%d\n", 
                  addr, val, size);
    
    /* Primary VGA compatibility through existing VGA layer */
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

/* VBE Fallback Support - additive VBE interface from QEMU VGA STD */
static uint64_t geforce_vbe_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint64_t ret = 0;
    
    /* Comprehensive logging: all VBE register accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VBE fallback read addr=0x%04lx size=%d ", addr, size);
    
    switch (addr) {
    case 0: /* VBE index register */
        ret = s->vbe_index;
        break;
    case 2: /* VBE data register */
        if (s->vbe_index < VBE_DISPI_INDEX_NB) {
            ret = s->vbe_regs[s->vbe_index];
        }
        break;
    default:
        /* Fallback to standard VGA for undefined VBE registers */
        if (s->vga.vbe_regs) {
            ret = vga_mem_readb(&s->vga, addr);
        }
        break;
    }
    
    qemu_log_mask(LOG_GUEST_ERROR, "value=0x%08lx VBE_fallback=%s\n", ret, 
                  s->vbe_fallback_active ? "active" : "inactive");
    
    return ret;
}

static void geforce_vbe_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Comprehensive logging: all VBE register accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VBE fallback write addr=0x%04lx value=0x%08lx size=%d\n", 
                  addr, val, size);
    
    switch (addr) {
    case 0: /* VBE index register */
        s->vbe_index = val;
        break;
    case 2: /* VBE data register */
        if (s->vbe_index < VBE_DISPI_INDEX_NB) {
            s->vbe_regs[s->vbe_index] = val;
            
            /* Handle VBE mode set - comprehensive logging */
            if (s->vbe_index == VBE_DISPI_INDEX_ENABLE) {
                qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VBE mode set enable=0x%08lx ", val);
                if (val & VBE_DISPI_ENABLED) {
                    s->vbe_fallback_active = true;
                    qemu_log_mask(LOG_GUEST_ERROR, "activating VBE fallback mode\n");
                } else {
                    s->vbe_fallback_active = false;
                    qemu_log_mask(LOG_GUEST_ERROR, "disabling VBE fallback mode\n");
                }
            }
            
            /* Log framebuffer changes */
            if (s->vbe_index == VBE_DISPI_INDEX_XRES || 
                s->vbe_index == VBE_DISPI_INDEX_YRES ||
                s->vbe_index == VBE_DISPI_INDEX_BPP) {
                qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VBE framebuffer change %s=0x%08lx\n",
                              s->vbe_index == VBE_DISPI_INDEX_XRES ? "XRES" :
                              s->vbe_index == VBE_DISPI_INDEX_YRES ? "YRES" : "BPP", val);
            }
        }
        break;
    default:
        /* Fallback to standard VGA for undefined VBE registers */
        if (s->vga.vbe_regs) {
            vga_mem_writeb(&s->vga, addr, val);
        }
        break;
    }
}

static const MemoryRegionOps geforce_vbe_ops = {
    .read = geforce_vbe_read,
    .write = geforce_vbe_write,
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

/* Apply device ID mappings for nouveau compatibility - extended model support */
static void nv_apply_model_ids(NVGFState *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);
    
    /* Set architecture and implementation based on model - preserve existing logic */
    s->architecture = NV_ARCH_20;  /* Default to NV20 for GeForce3 series */
    
    /* Comprehensive model support for -device geforce,model=xxx */
    switch (s->model) {
    case GEFORCE_MODEL_GEFORCE_DDR:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE_DDR_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE_DDR_DEVICE_ID >> 8) & 0xff;
        s->implementation = 0x00;
        s->architecture = 0x10;  /* NV10 architecture */
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce DDR (NV10)\n");
        break;
    case GEFORCE_MODEL_GEFORCE2_MX:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE2_MX_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE2_MX_DEVICE_ID >> 8) & 0xff;
        s->implementation = 0x00;
        s->architecture = 0x11;  /* NV11 architecture */
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce2 MX (NV11)\n");
        break;
    case GEFORCE_MODEL_GEFORCE2_GTS:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE2_GTS_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE2_GTS_DEVICE_ID >> 8) & 0xff;
        s->implementation = 0x00;
        s->architecture = 0x15;  /* NV15 architecture */
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce2 GTS (NV15)\n");
        break;
    case GEFORCE_MODEL_GEFORCE3:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE3_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE3_DEVICE_ID >> 8) & 0xff;
        s->implementation = NV_IMPL_GEFORCE3;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce3 (NV20)\n");
        break;
    case GEFORCE_MODEL_GEFORCE3_TI200:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE3_TI200_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE3_TI200_DEVICE_ID >> 8) & 0xff;
        s->implementation = NV_IMPL_GEFORCE3_TI200;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce3 Ti 200 (NV20)\n");
        break;
    case GEFORCE_MODEL_GEFORCE3_TI500:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE3_TI500_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE3_TI500_DEVICE_ID >> 8) & 0xff;
        s->implementation = NV_IMPL_GEFORCE3_TI500;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce3 Ti 500 (NV20)\n");
        break;
    case GEFORCE_MODEL_GEFORCE4_MX:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE4_MX_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE4_MX_DEVICE_ID >> 8) & 0xff;
        s->implementation = 0x00;
        s->architecture = 0x17;  /* NV17 architecture */
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce4 MX (NV17)\n");
        break;
    case GEFORCE_MODEL_GEFORCE4_TI:
        pci_dev->config[PCI_DEVICE_ID] = GEFORCE4_TI_DEVICE_ID & 0xff;
        pci_dev->config[PCI_DEVICE_ID + 1] = (GEFORCE4_TI_DEVICE_ID >> 8) & 0xff;
        s->implementation = 0x00;
        s->architecture = 0x25;  /* NV25 architecture */
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Configured as GeForce4 Ti (NV25)\n");
        break;
    default:
        /* Default to standard GeForce3 - preserve existing behavior */
        s->implementation = NV_IMPL_GEFORCE3;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Using default GeForce3 configuration (NV20)\n");
        break;
    }
    
    /* Compute PMC_BOOT_0 register - preserve existing logic */
    s->pmc_boot_0 = nv_compute_boot0(s);
    
    /* Initialize other PMC registers - preserve existing logic */
    s->pmc_intr_0 = 0x00000000;    /* No interrupts pending */
    s->pmc_intr_en_0 = 0x00000000; /* Interrupts disabled initially */
    
    /* Log VRAM configuration */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VRAM configured to %dMB\n", s->vram_size_mb);
}

/* BAR0 register read handler for nouveau compatibility - enhanced with comprehensive logging */
static uint64_t nv_bar0_readl(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint64_t ret = 0;
    
    /* Comprehensive logging: all MMIO BAR0 reads with throttling to prevent log spam */
    if ((s->mmio_read_throttle++ % 100) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: BAR0 MMIO read addr=0x%08lx size=%d ", addr, size);
    }
    
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* Critical register for nouveau chipset detection */
        ret = s->pmc_boot_0;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PMC_BOOT_0 read: 0x%08lx (arch=0x%02x impl=0x%02x)\n", 
                      ret, s->architecture, s->implementation);
        break;
        
    case NV_PMC_INTR_0:
        /* Interrupt status register */
        ret = s->pmc_intr_0;
        if ((s->mmio_read_throttle % 100) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PMC_INTR_0 read: 0x%08lx\n", ret);
        }
        break;
        
    case NV_PMC_INTR_EN_0:
        /* Interrupt enable register */
        ret = s->pmc_intr_en_0;
        if ((s->mmio_read_throttle % 100) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PMC_INTR_EN_0 read: 0x%08lx\n", ret);
        }
        break;
        
    case NV_PBUS_PCI_NV_1:
        /* PCI configuration mirror */
        ret = (NVIDIA_VENDOR_ID << 16) | GEFORCE3_DEVICE_ID;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PBUS_PCI_NV_1 read: 0x%08lx\n", ret);
        break;
        
    default:
        /* For unhandled registers, check if it's in PRMVIO range */
        if (addr < NV_PRMVIO_SIZE) {
            uint32_t reg = addr / 4;
            if (reg < ARRAY_SIZE(s->prmvio)) {
                ret = s->prmvio[reg];
            }
        }
        
        /* Log access to unsupported/unknown registers - potential guest confusion point */
        if ((s->mmio_read_throttle % 100) == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Unhandled BAR0 read addr=0x%08lx value=0x%08lx - guest may be confused\n", addr, ret);
        }
        break;
    }
    
    if ((s->mmio_read_throttle % 100) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "value=0x%08lx\n", ret);
    }
    
    return ret;
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
    
    /* Comprehensive logging: all MMIO BAR0 writes with throttling */
    if ((s->mmio_write_throttle++ % 100) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: BAR0 MMIO write addr=0x%08lx value=0x%08lx size=%d\n", 
                      addr, val, size);
    }
    
    switch (addr) {
    case NV_PMC_INTR_0:
        /* Interrupt status register - write to clear */
        s->pmc_intr_0 &= ~val;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PMC_INTR_0 write clear: 0x%08lx -> 0x%08x\n", 
                      val, s->pmc_intr_0);
        break;
        
    case NV_PMC_INTR_EN_0:
        /* Interrupt enable register */
        s->pmc_intr_en_0 = val;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: PMC_INTR_EN_0 write: 0x%08lx\n", val);
        break;
        
    default:
        /* Handle generic PRMVIO register writes */
        if (addr < NV_PRMVIO_SIZE) {
            uint32_t reg = addr / 4;
            if (reg < ARRAY_SIZE(s->prmvio)) {
                s->prmvio[reg] = val;
            }
        } else {
            /* Log access to unsupported registers - potential guest confusion point */
            if ((s->mmio_write_throttle % 100) == 0) {
                qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Unhandled BAR0 write addr=0x%08lx value=0x%08lx - guest may be confused\n", 
                              addr, val);
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

/* CRTC operations with comprehensive logging */
static uint64_t geforce_crtc_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint64_t ret = 0;
    
    /* Comprehensive logging: all CRTC register accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: CRTC/BAR2 read addr=0x%04lx size=%d ", addr, size);
    
    /* Handle DDC reads */
    if (addr >= 0x50 && addr < 0x60) {
        ret = geforce_ddc_read(s, addr - 0x50, size);
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_read value=0x%08lx\n", ret);
        return ret;
    }
    
    /* Basic CRTC register read */
    switch (addr) {
    case 0x00: /* CRTC status */
        ret = 0x01; /* Not in VBlank */
        qemu_log_mask(LOG_GUEST_ERROR, "CRTC_status value=0x%08lx\n", ret);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "unsupported_CRTC_reg value=0x%08lx - guest may be confused\n", ret);
        break;
    }
    
    return ret;
}

static void geforce_crtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Comprehensive logging: all CRTC register accesses */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: CRTC/BAR2 write addr=0x%04lx value=0x%08lx size=%d ", 
                  addr, val, size);
    
    /* Handle DDC writes */
    if (addr >= 0x50 && addr < 0x60) {
        geforce_ddc_write(s, addr - 0x50, val, size);
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_write\n");
        return;
    }
    
    /* Basic CRTC register write */
    switch (addr) {
    case 0x00: /* CRTC control */
        /* Handle CRTC control - log mode set activity */
        qemu_log_mask(LOG_GUEST_ERROR, "CRTC_control - potential mode set\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "unsupported_CRTC_reg - guest may be confused\n");
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

/* DDC/I2C implementation with comprehensive logging */
static void geforce_ddc_init(NVGFState *s)
{
    /* Initialize I2C bus for DDC */
    s->i2c_bus = i2c_init_bus(DEVICE(s), "ddc");
    /* TODO: Enable when I2C DDC support is available */
    /* s->i2c_ddc = i2c_slave_create_simple(s->i2c_bus, TYPE_I2CDDC, 0x50); */
    s->i2c_ddc = NULL;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: DDC/I2C bus initialized\n");
    
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
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: EDID initialized %dx%d default resolution\n", 
                  s->edid_info.prefx, s->edid_info.prefy);
    
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
    uint64_t ret = 0xff;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: DDC read addr=0x%04lx size=%d ", addr, size);
    
    if (!s->edid_enabled || !s->i2c_bus) {
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_disabled value=0x%08lx\n", ret);
        return ret;
    }
    
    switch (addr) {
    case 0x00: /* DDC data */
        ret = i2c_recv(s->i2c_bus);
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_data value=0x%08lx\n", ret);
        break;
    case 0x04: /* DDC control/status */
        ret = s->ddc_state;
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_control value=0x%08lx\n", ret);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_unknown value=0x%08lx - guest may be confused\n", ret);
        break;
    }
    
    return ret;
}

static void geforce_ddc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: DDC write addr=0x%04lx value=0x%08lx size=%d ", 
                  addr, val, size);
    
    if (!s->edid_enabled || !s->i2c_bus) {
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_disabled\n");
        return;
    }
    
    switch (addr) {
    case 0x00: /* DDC data */
        i2c_send(s->i2c_bus, val);
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_data_sent\n");
        break;
    case 0x04: /* DDC control */
        s->ddc_state = val;
        if (val & DDC_SCL_PIN) {
            i2c_start_transfer(s->i2c_bus, (val & DDC_SDA_PIN) ? 0x51 : 0x50, 0);
            qemu_log_mask(LOG_GUEST_ERROR, "DDC_transfer_start addr=0x%02x\n", 
                          (val & DDC_SDA_PIN) ? 0x51 : 0x50);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "DDC_control_set\n");
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "DDC_unknown - guest may be confused\n");
        break;
    }
}

/* UI info callback for dynamic EDID with comprehensive logging */
static void geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    NVGFState *s = opaque;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: UI info callback idx=%d ", idx);
    
    if (!s->edid_enabled) {
        qemu_log_mask(LOG_GUEST_ERROR, "EDID_disabled\n");
        return;
    }
    
    /* Update EDID info with new display information */
    if (info->width && info->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "resolution=%dx%d ", info->width, info->height);
        
        s->edid_info.prefx = info->width;
        s->edid_info.prefy = info->height;
        s->edid_info.maxx = MAX(info->width, s->edid_info.maxx);
        s->edid_info.maxy = MAX(info->height, s->edid_info.maxy);
        
        qemu_log_mask(LOG_GUEST_ERROR, "max_res=%dx%d ", s->edid_info.maxx, s->edid_info.maxy);
        
        /* Regenerate EDID blob */
        qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
        
        qemu_log_mask(LOG_GUEST_ERROR, "EDID_regenerated\n");
        
        /* Update DDC device with new EDID */
        if (s->i2c_ddc) {
            /* I2CDDCState *ddc = I2CDDC(s->i2c_ddc); */
            /* TODO: Enable when i2c_ddc_set_edid is available */
            /* i2c_ddc_set_edid(ddc, s->edid_blob, sizeof(s->edid_blob)); */
            qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: DDC device would be updated with new EDID\n");
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "invalid_resolution\n");
    }
}

/* Device initialization with extended model and VBE fallback support */
static void nv_realize(PCIDevice *pci_dev, Error **errp)
{
    NVGFState *s = GEFORCE3(pci_dev);
    VGACommonState *vga = &s->vga;
    
    /* Log device initialization */
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Device initialization started\n");
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Model=%s VRAM=%dMB ROM=%s\n", 
                  s->model_name ? s->model_name : "default", 
                  s->vram_size_mb,
                  s->romfile ? s->romfile : "none");
    
    /* Initialize NVIDIA-specific registers first - preserve existing logic */
    nv_apply_model_ids(s);
    
    /* Initialize VBE fallback state - additive VBE support */
    s->vbe_enabled = true;
    s->vbe_fallback_active = false;
    s->vbe_index = 0;
    
    /* Initialize VBE registers with default VGA STD fallback values */
    s->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    s->vbe_regs[VBE_DISPI_INDEX_XRES] = 1024;
    s->vbe_regs[VBE_DISPI_INDEX_YRES] = 768;
    s->vbe_regs[VBE_DISPI_INDEX_BPP] = 32;
    s->vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_DISABLED;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VBE fallback initialized\n");
    
    /* Adjust VGA VRAM size based on configured VRAM */
    vga->vram_size = s->vram_size_mb * 1024 * 1024;
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VGA VRAM configured to %ld bytes\n", vga->vram_size);
    
    /* FIX: Initialize VGA - Add missing Error** parameter to vga_common_init call - preserve existing fix */
    vga_common_init(vga, OBJECT(s), errp);
    vga_init(vga, OBJECT(s), pci_address_space(pci_dev), 
              pci_address_space_io(pci_dev), true);
    
    /* Set up PCI configuration - preserve existing logic */
    pci_dev->config[PCI_INTERRUPT_PIN] = 1;
    
    /* Initialize memory regions - preserve existing and add VBE fallback */
    memory_region_init_io(&s->mmio, OBJECT(s), &geforce_prmvio_ops, s,
                          "geforce3-prmvio", NV_PRMVIO_SIZE);
    memory_region_init_io(&s->crtc, OBJECT(s), &geforce_crtc_ops, s,
                          "geforce3-crtc", NV_CRTC_SIZE);
    
    /* Add VBE fallback region - additive VBE support, does not interfere with existing VGA */
    memory_region_init_io(&s->vbe_region, OBJECT(s), &geforce_vbe_ops, s,
                          "geforce3-vbe-fallback", 0x10);
    
    /* Map memory regions - preserve existing BAR layout */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->mmio);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_MEM_TYPE_32, &vga->vram);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->crtc);
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Memory regions mapped - BAR0(MMIO) BAR1(VRAM) BAR2(CRTC)\n");
    
    /* Initialize DDC and EDID - preserve existing logic */
    geforce_ddc_init(s);
    
    /* FIX: Register UI info callback for dynamic EDID - Remove & from hw_ops to fix incompatible pointer types - preserve existing fix */
    vga->con = graphic_console_init(DEVICE(pci_dev), 0, vga->hw_ops, vga);
    qemu_console_set_display_gl_ctx(vga->con, NULL);
    
    /* FIX: Set up UI info callback - Use dpy_set_ui_info instead of qemu_console_set_ui_info - preserve existing fix */
    if (vga->con) {
        dpy_set_ui_info(vga->con, geforce_ui_info, s);
    }
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Device initialization completed successfully\n");
}

/* Device property initialization - support for -device geforce,model=xxx,vramsize=xxM,romfile= */
static void nv_instance_init(Object *obj)
{
    NVGFState *s = GEFORCE3(obj);
    
    /* Set default configuration */
    s->model = GEFORCE_MODEL_GEFORCE3;  /* Default to GeForce3 */
    s->vram_size_mb = GEFORCE_DEFAULT_VRAM_SIZE / (1024 * 1024);  /* Default 128MB */
    s->model_name = g_strdup("geforce3");
    s->romfile = NULL;
    
    /* Initialize logging throttle counters */
    s->mmio_read_throttle = 0;
    s->mmio_write_throttle = 0;
    s->vbe_access_throttle = 0;
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Instance initialized with defaults\n");
}

/* Property validation and string-to-model conversion */
static void nv_set_model(Object *obj, const char *value, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    
    g_free(s->model_name);
    s->model_name = g_strdup(value);
    
    /* Convert string model name to enum for device configuration */
    if (strcmp(value, "geforce-ddr") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE_DDR;
    } else if (strcmp(value, "geforce2-mx") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE2_MX;
    } else if (strcmp(value, "geforce2-gts") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE2_GTS;
    } else if (strcmp(value, "geforce3") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE3;
    } else if (strcmp(value, "geforce3-ti200") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE3_TI200;
    } else if (strcmp(value, "geforce3-ti500") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE3_TI500;
    } else if (strcmp(value, "geforce4-mx") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE4_MX;
    } else if (strcmp(value, "geforce4-ti") == 0) {
        s->model = GEFORCE_MODEL_GEFORCE4_TI;
    } else {
        /* Default to GeForce3 for unrecognized models */
        s->model = GEFORCE_MODEL_GEFORCE3;
        qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Unknown model '%s', defaulting to geforce3\n", value);
    }
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Model set to %s\n", value);
}

static char *nv_get_model(Object *obj, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    return g_strdup(s->model_name);
}

static void nv_set_vram_size(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    uint32_t value;
    
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    
    /* Validate VRAM size range: 64MB to 512MB */
    if (value < 64 || value > 512) {
        error_setg(errp, "GeForce VRAM size must be between 64MB and 512MB, got %dMB", value);
        return;
    }
    
    s->vram_size_mb = value;
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: VRAM size set to %dMB\n", value);
}

static void nv_get_vram_size(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    visit_type_uint32(v, name, &s->vram_size_mb, errp);
}

/* ROM file property support for -device geforce,romfile= */
static void nv_set_romfile(Object *obj, const char *value, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    
    g_free(s->romfile);
    s->romfile = g_strdup(value);
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: ROM file set to %s\n", value ? value : "none");
}

static char *nv_get_romfile(Object *obj, Error **errp)
{
    NVGFState *s = GEFORCE3(obj);
    return g_strdup(s->romfile);
}
/* FIX: Update function signature to match expected prototype for class_init */
static void nv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    
    k->realize = nv_realize;
    k->vendor_id = NVIDIA_VENDOR_ID;
    k->device_id = GEFORCE3_DEVICE_ID;  /* Default, will be overridden by model */
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->subsystem_vendor_id = NVIDIA_VENDOR_ID;
    k->subsystem_id = GEFORCE3_DEVICE_ID;
    
    dc->desc = "NVIDIA GeForce Graphics Card (Multi-Model Support)";
    /* FIX: Modern QEMU uses device_class_set_parent_reset instead of dc->reset - preserve existing fix */
    /* dc->reset = vga_common_reset; */ /* Removed to fix "no member named 'reset'" error */
    dc->vmsd = &vmstate_vga_common;
    dc->hotpluggable = false;
    
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    
    /* Add device properties for -device geforce,model=xxx,vramsize=xxM,romfile= support */
    object_class_property_add_str(klass, "model", nv_get_model, nv_set_model);
    object_class_property_set_description(klass, "model", 
        "GeForce model (geforce-ddr, geforce2-mx, geforce2-gts, geforce3, geforce3-ti200, geforce3-ti500, geforce4-mx, geforce4-ti)");
    
    object_class_property_add(klass, "vramsize", "uint32", nv_get_vram_size, nv_set_vram_size, NULL, NULL);
    object_class_property_set_description(klass, "vramsize", 
        "VRAM size in MB (64-512MB supported)");
    
    object_class_property_add_str(klass, "romfile", nv_get_romfile, nv_set_romfile);
    object_class_property_set_description(klass, "romfile", 
        "Optional ROM file path");
    
    qemu_log_mask(LOG_GUEST_ERROR, "GeForce3: Class initialized with property support\n");
}

static const TypeInfo geforce3_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NVGFState),
    .instance_init = nv_instance_init,  /* Initialize properties and defaults */
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