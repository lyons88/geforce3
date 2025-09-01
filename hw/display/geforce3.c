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

/* VBE I/O ports (standard VBE DISPI ports) */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

/* NV20 (GeForce3) architecture constants */
#define NV_ARCH_20              0x20
#define NV_IMPL_GEFORCE3        0x00
#define NV_IMPL_GEFORCE3_TI200  0x01
#define NV_IMPL_GEFORCE3_TI500  0x02

/* VBE DISPI (VGA BIOS Extensions Display Interface) constants */
#define VBE_DISPI_MAX_XRES      4096
#define VBE_DISPI_MAX_YRES      4096
#define VBE_DISPI_MAX_BPP       32

/* VBE DISPI Register indices */
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
#define VBE_DISPI_INDEX_NB              0xa /* Number of VBE registers */

/* VBE DISPI IDs */
#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

/* VBE DISPI Enable flags */
#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

/* VBE DISPI Bank granularity */
#define VBE_DISPI_BANK_SIZE_KB          64
#define VBE_DISPI_BANK_SIZE             (VBE_DISPI_BANK_SIZE_KB * 1024)

typedef struct VBEState {
    /* VBE DISPI registers */
    uint16_t vbe_regs[VBE_DISPI_INDEX_NB];
    
    /* Current state */
    uint16_t vbe_index;
    uint16_t vbe_start_addr;
    uint32_t vbe_line_offset;
    uint32_t vbe_bank_mask;
    
    /* Capabilities and limits */
    uint32_t vbe_size;           /* Available VRAM size */
    bool vbe_mapped;             /* LFB mapping state */
    bool vbe_enabled;            /* VBE mode enabled */
    
    /* Banking support */
    uint32_t bank_offset;        /* Current bank offset */
    uint8_t *bank_ptr;           /* Pointer to current bank */
} VBEState;

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
    
    /* VBE support - Enhanced VBE state structure */
    VBEState vbe;
    
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
static void vbe_init(NVGFState *s);
static uint16_t vbe_read_reg(NVGFState *s, uint16_t index);
static void vbe_write_reg(NVGFState *s, uint16_t index, uint16_t val);
static bool vbe_validate_mode(NVGFState *s, uint16_t xres, uint16_t yres, uint16_t bpp);
static void vbe_enable_mode(NVGFState *s);
static void vbe_disable_mode(NVGFState *s);
static void vbe_update_bank(NVGFState *s);
static void vbe_fallback_to_vga(NVGFState *s);
static void vbe_update_display(NVGFState *s, uint32_t addr, uint32_t size);
static bool vbe_validate_virtual_resolution(NVGFState *s);
static void vbe_update_display_start(NVGFState *s);

/* VGA I/O operations with VBE DISPI port support */
static uint64_t geforce_vga_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle VBE DISPI ports */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        return s->vbe.vbe_index;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        return vbe_read_reg(s, s->vbe.vbe_index);
    }
    
    /* Fallback to standard VGA I/O */
    return vga_ioport_read(&s->vga, addr);
}

static void geforce_vga_ioport_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle VBE DISPI ports */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        s->vbe.vbe_index = val & 0xFFFF;
        return;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        vbe_write_reg(s, s->vbe.vbe_index, val & 0xFFFF);
        return;
    }
    
    /* Fallback to standard VGA I/O */
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

/* VBE (VESA BIOS Extensions) implementation */

/* Initialize VBE state with defaults */
static void vbe_init(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    
    /* Initialize VBE registers to safe defaults */
    memset(vbe->vbe_regs, 0, sizeof(vbe->vbe_regs));
    
    /* Set VBE ID to latest supported version */
    vbe->vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID5;
    
    /* Set default resolution to VGA compatible mode */
    vbe->vbe_regs[VBE_DISPI_INDEX_XRES] = 640;
    vbe->vbe_regs[VBE_DISPI_INDEX_YRES] = 480;
    vbe->vbe_regs[VBE_DISPI_INDEX_BPP] = 8;
    vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_DISABLED;
    vbe->vbe_regs[VBE_DISPI_INDEX_BANK] = 0;
    
    /* Virtual resolution defaults to physical resolution */
    vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = 640;
    vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = 480;
    vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
    vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
    
    /* Initialize state variables */
    vbe->vbe_index = 0;
    vbe->vbe_start_addr = 0;
    vbe->vbe_line_offset = 0;
    vbe->vbe_bank_mask = 0;
    vbe->vbe_size = s->vga.vram_size;
    vbe->vbe_mapped = false;
    vbe->vbe_enabled = false;
    vbe->bank_offset = 0;
    vbe->bank_ptr = s->vga.vram_ptr;
}

/* Validate VBE mode parameters */
static bool vbe_validate_mode(NVGFState *s, uint16_t xres, uint16_t yres, uint16_t bpp)
{
    uint32_t required_mem;
    
    /* Check resolution limits */
    if (xres == 0 || xres > VBE_DISPI_MAX_XRES ||
        yres == 0 || yres > VBE_DISPI_MAX_YRES) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Invalid resolution %dx%d (max %dx%d)\n",
                     xres, yres, VBE_DISPI_MAX_XRES, VBE_DISPI_MAX_YRES);
        return false;
    }
    
    /* Check BPP support */
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Unsupported BPP %d (supported: 8,15,16,24,32)\n", bpp);
        return false;
    }
    
    /* Check memory requirements */
    required_mem = ((uint32_t)xres * yres * ((bpp + 7) / 8));
    if (required_mem > s->vbe.vbe_size) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Mode requires %u bytes, only %u available\n",
                     required_mem, s->vbe.vbe_size);
        return false;
    }
    
    return true;
}

/* Read VBE DISPI register */
static uint16_t vbe_read_reg(NVGFState *s, uint16_t index)
{
    VBEState *vbe = &s->vbe;
    
    if (index >= VBE_DISPI_INDEX_NB) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: VBE read from invalid register %d\n", index);
        return 0;
    }
    
    switch (index) {
    case VBE_DISPI_INDEX_ID:
        return VBE_DISPI_ID5;
    case VBE_DISPI_INDEX_XRES:
    case VBE_DISPI_INDEX_YRES:
    case VBE_DISPI_INDEX_BPP:
    case VBE_DISPI_INDEX_ENABLE:
    case VBE_DISPI_INDEX_BANK:
    case VBE_DISPI_INDEX_VIRT_WIDTH:
    case VBE_DISPI_INDEX_VIRT_HEIGHT:
    case VBE_DISPI_INDEX_X_OFFSET:
    case VBE_DISPI_INDEX_Y_OFFSET:
        return vbe->vbe_regs[index];
    default:
        return 0;
    }
}

/* Write VBE DISPI register */
static void vbe_write_reg(NVGFState *s, uint16_t index, uint16_t val)
{
    VBEState *vbe = &s->vbe;
    uint16_t old_val;
    
    if (index >= VBE_DISPI_INDEX_NB) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: VBE write to invalid register %d\n", index);
        return;
    }
    
    old_val = vbe->vbe_regs[index];
    
    switch (index) {
    case VBE_DISPI_INDEX_ID:
        /* ID register is read-only */
        break;
        
    case VBE_DISPI_INDEX_XRES:
    case VBE_DISPI_INDEX_YRES:
    case VBE_DISPI_INDEX_BPP:
        /* Store the value but don't activate until ENABLE is written */
        vbe->vbe_regs[index] = val;
        break;
        
    case VBE_DISPI_INDEX_ENABLE:
        vbe->vbe_regs[index] = val;
        if (val & VBE_DISPI_ENABLED) {
            vbe_enable_mode(s);
        } else {
            vbe_disable_mode(s);
        }
        break;
        
    case VBE_DISPI_INDEX_BANK:
        vbe->vbe_regs[index] = val;
        vbe_update_bank(s);
        break;
        
    case VBE_DISPI_INDEX_VIRT_WIDTH:
    case VBE_DISPI_INDEX_VIRT_HEIGHT:
        /* Validate virtual dimensions */
        if (val <= VBE_DISPI_MAX_XRES) {
            vbe->vbe_regs[index] = val;
            /* Recalculate line offset if virtual width changed */
            if (index == VBE_DISPI_INDEX_VIRT_WIDTH && vbe->vbe_enabled) {
                uint16_t bpp = vbe->vbe_regs[VBE_DISPI_INDEX_BPP];
                vbe->vbe_line_offset = val * ((bpp + 7) / 8);
            }
            /* Validate virtual resolution if VBE is enabled */
            if (vbe->vbe_enabled && !vbe_validate_virtual_resolution(s)) {
                qemu_log_mask(LOG_GUEST_ERROR, 
                             "geforce3: Invalid virtual resolution, falling back to VGA\n");
                vbe_fallback_to_vga(s);
            }
        }
        break;
        
    case VBE_DISPI_INDEX_X_OFFSET:
    case VBE_DISPI_INDEX_Y_OFFSET:
        /* Validate offsets don't exceed virtual dimensions */
        vbe->vbe_regs[index] = val;
        if (vbe->vbe_enabled) {
            if (!vbe_validate_virtual_resolution(s)) {
                /* Reset invalid offsets */
                vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
                vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
            } else {
                /* Update display start address for panning */
                vbe_update_display_start(s);
            }
        }
        break;
        
    default:
        vbe->vbe_regs[index] = val;
        break;
    }
}

/* Enable VBE mode */
static void vbe_enable_mode(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    uint16_t xres = vbe->vbe_regs[VBE_DISPI_INDEX_XRES];
    uint16_t yres = vbe->vbe_regs[VBE_DISPI_INDEX_YRES];
    uint16_t bpp = vbe->vbe_regs[VBE_DISPI_INDEX_BPP];
    uint16_t enable = vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE];
    
    /* Validate mode parameters */
    if (!vbe_validate_mode(s, xres, yres, bpp)) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: VBE mode validation failed, falling back to VGA\n");
        vbe_fallback_to_vga(s);
        return;
    }
    
    /* Set virtual dimensions to match physical if not set explicitly */
    if (vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] == 0) {
        vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = xres;
    }
    if (vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] == 0) {
        vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = yres;
    }
    
    /* Validate virtual resolution and offsets */
    if (!vbe_validate_virtual_resolution(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Virtual resolution validation failed, falling back to VGA\n");
        vbe_fallback_to_vga(s);
        return;
    }
    
    /* Calculate line offset */
    vbe->vbe_line_offset = vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] * ((bpp + 7) / 8);
    
    /* Update display start address for panning */
    vbe_update_display_start(s);
    
    /* Set up banking if not using linear frame buffer */
    if (!(enable & VBE_DISPI_LFB_ENABLED)) {
        vbe->vbe_bank_mask = (VBE_DISPI_BANK_SIZE / 1024) - 1;
        vbe_update_bank(s);
    } else {
        vbe->vbe_mapped = true;
        vbe->bank_ptr = s->vga.vram_ptr;
    }
    
    vbe->vbe_enabled = true;
    
    qemu_log_mask(LOG_TRACE, 
                 "geforce3: VBE mode enabled: %dx%d@%dbpp, LFB=%s\n",
                 xres, yres, bpp, (enable & VBE_DISPI_LFB_ENABLED) ? "yes" : "no");
}

/* Disable VBE mode and fallback to VGA */
static void vbe_disable_mode(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    
    vbe->vbe_enabled = false;
    vbe->vbe_mapped = false;
    vbe->bank_offset = 0;
    vbe->bank_ptr = s->vga.vram_ptr;
    
    /* Reset to VGA mode */
    vbe_fallback_to_vga(s);
    
    qemu_log_mask(LOG_TRACE, "geforce3: VBE mode disabled, using VGA\n");
}

/* Update banking window */
static void vbe_update_bank(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    uint32_t bank = vbe->vbe_regs[VBE_DISPI_INDEX_BANK];
    
    if (!vbe->vbe_enabled || (vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_LFB_ENABLED)) {
        return;
    }
    
    /* Calculate bank offset (64KB granularity) */
    vbe->bank_offset = bank * VBE_DISPI_BANK_SIZE;
    
    /* Ensure bank offset doesn't exceed VRAM size */
    if (vbe->bank_offset >= vbe->vbe_size) {
        vbe->bank_offset = 0;
        vbe->vbe_regs[VBE_DISPI_INDEX_BANK] = 0;
    }
    
    /* Update bank pointer */
    vbe->bank_ptr = s->vga.vram_ptr + vbe->bank_offset;
    
    qemu_log_mask(LOG_TRACE, 
                 "geforce3: VBE bank updated to %d (offset 0x%x)\n",
                 bank, vbe->bank_offset);
}

/* Fallback to VGA mode when VBE mode is invalid */
static void vbe_fallback_to_vga(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    
    /* Disable VBE mode */
    vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE] = VBE_DISPI_DISABLED;
    vbe->vbe_enabled = false;
    vbe->vbe_mapped = false;
    
    /* Reset to standard VGA mode (80x25 text or 640x480x4bpp graphics) */
    vbe->vbe_regs[VBE_DISPI_INDEX_XRES] = 640;
    vbe->vbe_regs[VBE_DISPI_INDEX_YRES] = 480;
    vbe->vbe_regs[VBE_DISPI_INDEX_BPP] = 8;
    vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = 640;
    vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = 480;
    vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
    vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
    vbe->vbe_regs[VBE_DISPI_INDEX_BANK] = 0;
    
    /* Reset banking */
    vbe->bank_offset = 0;
    vbe->bank_ptr = s->vga.vram_ptr;
    
    qemu_log_mask(LOG_TRACE, "geforce3: Fallback to VGA mode\n");
}

/* Notify display subsystem of VBE memory updates */
static void vbe_update_display(NVGFState *s, uint32_t addr, uint32_t size)
{
    VBEState *vbe = &s->vbe;
    
    if (!vbe->vbe_enabled) {
        return;
    }
    
    /* Calculate affected scanlines for efficient updates */
    uint16_t xres = vbe->vbe_regs[VBE_DISPI_INDEX_XRES];
    uint16_t yres = vbe->vbe_regs[VBE_DISPI_INDEX_YRES];
    uint16_t bpp = vbe->vbe_regs[VBE_DISPI_INDEX_BPP];
    uint32_t line_offset = vbe->vbe_line_offset;
    
    if (line_offset == 0) {
        return;
    }
    
    /* Calculate start and end lines affected by this memory update */
    uint32_t start_line = addr / line_offset;
    uint32_t end_line = (addr + size - 1) / line_offset;
    
    /* Clamp to display bounds */
    if (start_line >= yres) {
        return;
    }
    if (end_line >= yres) {
        end_line = yres - 1;
    }
    
    /* Mark the affected region as dirty for display refresh */
    uint32_t dirty_start = start_line * line_offset;
    uint32_t dirty_size = (end_line - start_line + 1) * line_offset;
    
    memory_region_set_dirty(&s->vga.vram, dirty_start, dirty_size);
    
    qemu_log_mask(LOG_TRACE, 
                 "geforce3: VBE display update: lines %u-%u, addr 0x%x, size %u\n",
                 start_line, end_line, addr, size);
}

/* Validate virtual resolution and offsets */
static bool vbe_validate_virtual_resolution(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    uint16_t phys_xres = vbe->vbe_regs[VBE_DISPI_INDEX_XRES];
    uint16_t phys_yres = vbe->vbe_regs[VBE_DISPI_INDEX_YRES];
    uint16_t virt_width = vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH];
    uint16_t virt_height = vbe->vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT];
    uint16_t x_offset = vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET];
    uint16_t y_offset = vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET];
    uint16_t bpp = vbe->vbe_regs[VBE_DISPI_INDEX_BPP];
    
    /* Virtual resolution must be at least as large as physical */
    if (virt_width < phys_xres || virt_height < phys_yres) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Virtual resolution %dx%d smaller than physical %dx%d\n",
                     virt_width, virt_height, phys_xres, phys_yres);
        return false;
    }
    
    /* Check if offsets are within virtual bounds */
    if (x_offset + phys_xres > virt_width || y_offset + phys_yres > virt_height) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Display offset (%d,%d) + physical size (%dx%d) exceeds virtual (%dx%d)\n",
                     x_offset, y_offset, phys_xres, phys_yres, virt_width, virt_height);
        return false;
    }
    
    /* Check memory requirements for virtual resolution */
    uint32_t required_mem = virt_width * virt_height * ((bpp + 7) / 8);
    if (required_mem > vbe->vbe_size) {
        qemu_log_mask(LOG_GUEST_ERROR, 
                     "geforce3: Virtual resolution requires %u bytes, only %u available\n",
                     required_mem, vbe->vbe_size);
        return false;
    }
    
    return true;
}

/* Update display start address for panning/scrolling */
static void vbe_update_display_start(NVGFState *s)
{
    VBEState *vbe = &s->vbe;
    uint16_t x_offset = vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET];
    uint16_t y_offset = vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET];
    uint16_t bpp = vbe->vbe_regs[VBE_DISPI_INDEX_BPP];
    uint32_t bytes_per_pixel = (bpp + 7) / 8;
    
    /* Calculate display start address based on offsets */
    vbe->vbe_start_addr = (y_offset * vbe->vbe_line_offset) + (x_offset * bytes_per_pixel);
    
    /* Ensure start address is within VRAM bounds */
    if (vbe->vbe_start_addr >= vbe->vbe_size) {
        vbe->vbe_start_addr = 0;
        vbe->vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
        vbe->vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;
    }
    
    qemu_log_mask(LOG_TRACE, 
                 "geforce3: VBE display start updated to 0x%x (offset %d,%d)\n",
                 vbe->vbe_start_addr, x_offset, y_offset);
}

/* Enhanced memory access functions for VBE banking and LFB */

/* Read from VBE memory with banking support */
static uint64_t vbe_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    VBEState *vbe = &s->vbe;
    uint64_t ret = 0;
    
    if (!vbe->vbe_enabled) {
        /* Fall back to standard VGA memory access */
        return 0;
    }
    
    /* Handle banked vs linear framebuffer mode */
    if (vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_LFB_ENABLED) {
        /* Linear framebuffer mode - direct VRAM access */
        if (addr < vbe->vbe_size) {
            switch (size) {
            case 1:
                ret = s->vga.vram_ptr[addr];
                break;
            case 2:
                ret = lduw_le_p(s->vga.vram_ptr + addr);
                break;
            case 4:
                ret = ldl_le_p(s->vga.vram_ptr + addr);
                break;
            case 8:
                ret = ldq_le_p(s->vga.vram_ptr + addr);
                break;
            }
        }
    } else {
        /* Banked mode - 64KB window */
        uint32_t bank_addr = (addr % VBE_DISPI_BANK_SIZE) + vbe->bank_offset;
        if (bank_addr < vbe->vbe_size) {
            switch (size) {
            case 1:
                ret = s->vga.vram_ptr[bank_addr];
                break;
            case 2:
                ret = lduw_le_p(s->vga.vram_ptr + bank_addr);
                break;
            case 4:
                ret = ldl_le_p(s->vga.vram_ptr + bank_addr);
                break;
            case 8:
                ret = ldq_le_p(s->vga.vram_ptr + bank_addr);
                break;
            }
        }
    }
    
    return ret;
}

/* Write to VBE memory with banking support */
static void vbe_mem_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    VBEState *vbe = &s->vbe;
    uint32_t physical_addr;
    
    if (!vbe->vbe_enabled) {
        /* Fall back to standard VGA memory access */
        return;
    }
    
    /* Handle banked vs linear framebuffer mode */
    if (vbe->vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_LFB_ENABLED) {
        /* Linear framebuffer mode - direct VRAM access */
        if (addr < vbe->vbe_size) {
            physical_addr = addr;
            switch (size) {
            case 1:
                s->vga.vram_ptr[addr] = val;
                break;
            case 2:
                stw_le_p(s->vga.vram_ptr + addr, val);
                break;
            case 4:
                stl_le_p(s->vga.vram_ptr + addr, val);
                break;
            case 8:
                stq_le_p(s->vga.vram_ptr + addr, val);
                break;
            }
        } else {
            return;
        }
    } else {
        /* Banked mode - 64KB window */
        uint32_t bank_addr = (addr % VBE_DISPI_BANK_SIZE) + vbe->bank_offset;
        if (bank_addr < vbe->vbe_size) {
            physical_addr = bank_addr;
            switch (size) {
            case 1:
                s->vga.vram_ptr[bank_addr] = val;
                break;
            case 2:
                stw_le_p(s->vga.vram_ptr + bank_addr, val);
                break;
            case 4:
                stl_le_p(s->vga.vram_ptr + bank_addr, val);
                break;
            case 8:
                stq_le_p(s->vga.vram_ptr + bank_addr, val);
                break;
            }
        } else {
            return;
        }
    }
    
    /* Notify display of the update */
    vbe_update_display(s, physical_addr, size);
}

/* Memory operations for VBE LFB region */
static const MemoryRegionOps vbe_lfb_ops = {
    .read = vbe_mem_read,
    .write = vbe_mem_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
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
    NVGFState *s = opaque;
    
    /* Handle VBE DISPI register access */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        return s->vbe.vbe_index;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        return vbe_read_reg(s, s->vbe.vbe_index);
    }
    
    /* Use the comprehensive BAR0 register handler for other addresses */
    return nv_bar0_readl(opaque, addr, size);
}

static void geforce_prmvio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    /* Handle VBE DISPI register access */
    if (addr == VBE_DISPI_IOPORT_INDEX) {
        s->vbe.vbe_index = val & 0xFFFF;
        return;
    } else if (addr == VBE_DISPI_IOPORT_DATA) {
        vbe_write_reg(s, s->vbe.vbe_index, val & 0xFFFF);
        return;
    }
    
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
    
    /* Initialize VBE support */
    vbe_init(s);
    
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
    memory_region_init_io(&s->lfb, OBJECT(s), &vbe_lfb_ops, s,
                          "geforce3-lfb", NV_LFB_SIZE);
    
    /* Map memory regions */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->mmio);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_MEM_TYPE_32, &vga->vram);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->crtc);
    pci_register_bar(pci_dev, 3, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->lfb);
    
    /* Initialize DDC and EDID */
    geforce_ddc_init(s);
    
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