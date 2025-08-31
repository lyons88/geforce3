/*
 * QEMU GeForce3 GPU emulation
 * 
 * Copyright (c) 2025 lyons88
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/display/vga.h"
#include "hw/display/vga_int.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"

#define TYPE_GEFORCE3 "geforce3"
#define GEFORCE3(obj) OBJECT_CHECK(Geforce3State, (obj), TYPE_GEFORCE3)

#define GEFORCE3_VENDOR_ID    0x10DE
#define GEFORCE3_DEVICE_ID    0x0200
#define GEFORCE3_REVISION     0xA1
#define GEFORCE3_CLASS_CODE   0x030000

#define GEFORCE3_VRAM_SIZE    (64 * 1024 * 1024) /* 64MB VRAM */
#define GEFORCE3_MMIO_SIZE    (16 * 1024 * 1024) /* 16MB MMIO */

/* Register offsets */
#define NV_PMC_BOOT_0         0x000000
#define NV_PMC_INTR_0         0x000100
#define NV_PMC_INTR_EN_0      0x000140
#define NV_PMC_ENABLE         0x000200

#define NV_PFIFO_CACHE1_PUSH0 0x003200
#define NV_PFIFO_CACHE1_PULL0 0x003250

#define NV_PGRAPH_DEBUG_0     0x400080
#define NV_PGRAPH_DEBUG_1     0x400084
#define NV_PGRAPH_DEBUG_2     0x400088
#define NV_PGRAPH_DEBUG_3     0x40008C

/* EDID constants */
#define EDID_SIZE 256

typedef struct {
    char vendor[4];
    char product[2];
    uint32_t serial;
    uint8_t week;
    uint8_t year;
    uint8_t version;
    uint8_t revision;
    uint8_t data[EDID_SIZE];
} EdidInfo;

typedef struct {
    PCIDevice parent_obj;
    
    VGACommonState vga;
    QemuConsole *con;
    
    /* Memory regions */
    MemoryRegion vram;
    MemoryRegion mmio;
    
    /* Registers */
    uint32_t regs[0x1000000 / 4];
    
    /* Graphics state */
    uint32_t gr_ctx;
    uint32_t nv_ctx_cache[8][32];
    
    /* VRAM */
    uint8_t *vram_ptr;
    uint32_t vram_size;
    
    /* EDID info */
    EdidInfo edid_info;
    
    /* IRQ state */
    uint32_t pending_interrupts;
    
    /* Engine state */
    uint32_t pgraph_trapped_addr;
    uint32_t pgraph_trapped_data;
    uint32_t pgraph_state;
    
    /* FIFO state */
    uint32_t pfifo_cache[32];
    uint32_t pfifo_ptr;
    
} Geforce3State;

static const VMStateDescription vmstate_geforce3 = {
    .name = "geforce3",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Geforce3State),
        VMSTATE_STRUCT(vga, Geforce3State, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_UINT32_ARRAY(regs, Geforce3State, 0x1000000 / 4),
        VMSTATE_UINT32(gr_ctx, Geforce3State),
        VMSTATE_UINT32(pending_interrupts, Geforce3State),
        VMSTATE_UINT32(pgraph_trapped_addr, Geforce3State),
        VMSTATE_UINT32(pgraph_trapped_data, Geforce3State),
        VMSTATE_UINT32(pgraph_state, Geforce3State),
        VMSTATE_UINT32_ARRAY(pfifo_cache, Geforce3State, 32),
        VMSTATE_UINT32(pfifo_ptr, Geforce3State),
        VMSTATE_END_OF_LIST()
    }
};

static void geforce3_update_irq(Geforce3State *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    
    if (s->pending_interrupts) {
        pci_set_irq(d, 1);
    } else {
        pci_set_irq(d, 0);
    }
}

static uint64_t geforce3_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Geforce3State *s = opaque;
    uint32_t val = 0;
    
    if (addr >= 0x1000000) {
        return 0;
    }
    
    addr &= ~3; /* Align to 32-bit boundary */
    val = s->regs[addr / 4];
    
    switch (addr) {
    case NV_PMC_BOOT_0:
        val = (GEFORCE3_DEVICE_ID << 16) | GEFORCE3_VENDOR_ID;
        break;
    case NV_PMC_INTR_0:
        val = s->pending_interrupts;
        break;
    case NV_PGRAPH_DEBUG_0:
    case NV_PGRAPH_DEBUG_1:
    case NV_PGRAPH_DEBUG_2:
    case NV_PGRAPH_DEBUG_3:
        /* Graphics debug registers */
        break;
    default:
        /* Return stored register value */
        break;
    }
    
    return val;
}

static void geforce3_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Geforce3State *s = opaque;
    
    if (addr >= 0x1000000) {
        return;
    }
    
    addr &= ~3; /* Align to 32-bit boundary */
    s->regs[addr / 4] = val;
    
    switch (addr) {
    case NV_PMC_INTR_0:
        /* Clear interrupts by writing 1 to bits */
        s->pending_interrupts &= ~val;
        geforce3_update_irq(s);
        break;
    case NV_PMC_INTR_EN_0:
        /* Interrupt enable */
        break;
    case NV_PMC_ENABLE:
        /* Enable/disable engines */
        break;
    case NV_PFIFO_CACHE1_PUSH0:
        /* FIFO push */
        if (s->pfifo_ptr < 32) {
            s->pfifo_cache[s->pfifo_ptr++] = val;
        }
        break;
    case NV_PGRAPH_DEBUG_0:
    case NV_PGRAPH_DEBUG_1:
    case NV_PGRAPH_DEBUG_2:
    case NV_PGRAPH_DEBUG_3:
        /* Graphics debug registers */
        break;
    default:
        /* Store register value */
        break;
    }
}

static const MemoryRegionOps geforce3_mmio_ops = {
    .read = geforce3_mmio_read,
    .write = geforce3_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void geforce3_init_edid(Geforce3State *s)
{
    const char *vendor_id = "NVD";
    
    /* Initialize EDID structure */
    memset(&s->edid_info, 0, sizeof(s->edid_info));
    
    /* This line has the compilation error #1 - passing const char* to void* */
    memcpy(s->edid_info.vendor, vendor_id, sizeof(vendor_id));
    
    s->edid_info.product[0] = 0x00;
    s->edid_info.product[1] = 0x02; /* GeForce3 */
    s->edid_info.serial = 0x12345678;
    s->edid_info.week = 1;
    s->edid_info.year = 2001 - 1990; /* EDID year offset */
    s->edid_info.version = 1;
    s->edid_info.revision = 3;
    
    /* Basic EDID data structure */
    s->edid_info.data[0] = 0x00; /* Header */
    s->edid_info.data[1] = 0xFF;
    s->edid_info.data[2] = 0xFF;
    s->edid_info.data[3] = 0xFF;
    s->edid_info.data[4] = 0xFF;
    s->edid_info.data[5] = 0xFF;
    s->edid_info.data[6] = 0xFF;
    s->edid_info.data[7] = 0x00;
}

static int geforce_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    Geforce3State *s = opaque;
    
    if (idx != 0) {
        return -1;
    }
    
    info->width = 1024;
    info->height = 768;
    info->depth = 32;
    
    return 0;
}

static void geforce3_realize(PCIDevice *dev, Error **errp)
{
    Geforce3State *s = GEFORCE3(dev);
    VGACommonState *vga = &s->vga;
    
    /* Initialize VGA */
    vga_common_init(vga, OBJECT(dev), errp);
    vga->con = graphic_console_init(DEVICE(dev), 0, &vga_ops, vga);
    s->con = vga->con;
    
    /* Allocate VRAM */
    s->vram_size = GEFORCE3_VRAM_SIZE;
    memory_region_init_ram(&s->vram, OBJECT(dev), "geforce3.vram", 
                          s->vram_size, errp);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    
    /* Setup MMIO */
    memory_region_init_io(&s->mmio, OBJECT(dev), &geforce3_mmio_ops, s,
                         "geforce3.mmio", GEFORCE3_MMIO_SIZE);
    
    /* Register PCI BARs */
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    
    /* Initialize EDID */
    geforce3_init_edid(s);
    
    /* Set up UI info callback - this line has compilation error #2 */
    if (s->con) {
        qemu_console_set_ui_info(vga->con, geforce_ui_info, s);
    }
    
    /* Clear registers */
    memset(s->regs, 0, sizeof(s->regs));
    s->pending_interrupts = 0;
    s->gr_ctx = 0;
    s->pfifo_ptr = 0;
}

static void geforce3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    
    pc->realize = geforce3_realize;
    pc->vendor_id = GEFORCE3_VENDOR_ID;
    pc->device_id = GEFORCE3_DEVICE_ID;
    pc->revision = GEFORCE3_REVISION;
    pc->class_id = GEFORCE3_CLASS_CODE;
    
    /* This line has compilation error #3 - no 'reset' member in DeviceClass */
    dc->reset = vga_common_reset;
    
    dc->vmsd = &vmstate_geforce3;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* This function signature has compilation error #4 - wrong signature */
static void nv_class_init(ObjectClass *klass, void *data)
{
    geforce3_class_init(klass, data);
}

static const TypeInfo geforce3_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Geforce3State),
    /* This line references the function with wrong signature - error #4 */
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

type_init(geforce3_register_types)