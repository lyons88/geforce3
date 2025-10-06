/*
 * QEMU NVIDIA GeForce3 GPU emulation
 * 
 * Based on Bochs GeForce emulation, ported to QEMU
 * This implementation provides basic VGA passthrough with DDC/I2C support
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/display/vga.h"
#include "hw/display/vga_int.h"
#include "hw/display/edid.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

#define TYPE_GEFORCE3 "geforce3"
#define GEFORCE3(obj) OBJECT_CHECK(NVGFState, (obj), TYPE_GEFORCE3)

#define NV_VENDOR_ID    0x10de
#define NV_DEVICE_ID    0x0201  /* GeForce3 Ti 200 */

/* DDC/I2C registers */
#define NV_PRAMDAC_I2C_PORT_0   0x681030
#define NV_PRAMDAC_I2C_PORT_1   0x681034

/* Static EDID data (1024x768@75Hz) */
static const uint8_t static_edid_data[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, /* header */
    0x22, 0xf0, 0x6c, 0x28, 0x01, 0x01, 0x01, 0x01, /* vendor/product */
    0x02, 0x16, 0x01, 0x04, 0xa0, 0x34, 0x20, 0x78, /* version/basic */
    0x23, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26, /* chroma info */
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0x81, 0x80, /* established timing */
    0x81, 0x40, 0x81, 0xc0, 0x81, 0x00, 0x95, 0x00, /* standard timing */
    0xa9, 0x40, 0xb3, 0x00, 0x01, 0x01, 0x02, 0x3a, /* detailed timing 1 */
    0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x09, 0x25, 0x21, 0x00, 0x00, 0x1e,
    0x8c, 0x0a, 0xd0, 0x8a, 0x20, 0xe0, 0x2d, 0x10, /* detailed timing 2 */
    0x10, 0x3e, 0x96, 0x00, 0x13, 0x8e, 0x21, 0x00,
    0x00, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x47, /* display name */
    0x65, 0x46, 0x6f, 0x72, 0x63, 0x65, 0x33, 0x20,
    0x44, 0x69, 0x73, 0x70, 0x00, 0x00, 0x00, 0xfd, /* range limits */
    0x00, 0x31, 0x56, 0x1d, 0x71, 0x11, 0x00, 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x65,
    /* Extension blocks filled with zeros */
    [128 ... 255] = 0x00
};

typedef struct DDCState {
    I2CBus *bus;
    const uint8_t *edid;
    int address;
    int reg;
} DDCState;

typedef struct NVGFState {
    PCIDevice parent_obj;
    
    VGACommonState vga;
    MemoryRegion vga_io;
    MemoryRegion mmio;
    MemoryRegion vram;
    
    /* DDC/I2C */
    DDCState ddc;
    
    /* Dynamic EDID support */
    qemu_edid_info edid_info;
    uint8_t edid_blob[256];
    
    /* Device registers */
    uint32_t pramdac_i2c[2];
    uint32_t enable_vga;
    
} NVGFState;

/* DDC/I2C Implementation */
static void geforce_ddc_init(NVGFState *s)
{
    /* Initialize EDID info with default values */
    s->edid_info.width_mm = 520;   /* 520mm = ~20.5 inches */
    s->edid_info.height_mm = 320;  /* 320mm = ~12.6 inches (4:3 aspect) */
    s->edid_info.prefx = 1024;     /* Default preferred resolution */
    s->edid_info.prefy = 768;
    s->edid_info.maxx = 1600;      /* Maximum supported resolution */
    s->edid_info.maxy = 1200;
    
    /* Generate dynamic EDID */
    qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
    
    /* Initialize DDC with dynamic EDID */
    s->ddc.edid = s->edid_blob;
    s->ddc.address = 0;
    s->ddc.reg = 0;
    
    qemu_log_mask(LOG_TRACE, "GeForce3: DDC initialized with dynamic EDID (%dx%d)\n", 
                  s->edid_info.prefx, s->edid_info.prefy);
}

static uint8_t geforce_ddc_read(DDCState *ddc)
{
    if (ddc->reg < 256) {
        uint8_t val = ddc->edid[ddc->reg];
        ddc->reg = (ddc->reg + 1) % 256;
        return val;
    }
    return 0xff;
}

static void geforce_ddc_write(DDCState *ddc, uint8_t addr, uint8_t data)
{
    if (addr == 0xa0) { /* EDID address */
        ddc->reg = data;
    }
}

/* MMIO register access */
static uint64_t nv_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint64_t val = 0;
    
    switch (addr) {
    case NV_PRAMDAC_I2C_PORT_0:
        val = s->pramdac_i2c[0];
        break;
    case NV_PRAMDAC_I2C_PORT_1:
        val = s->pramdac_i2c[1];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "GeForce3: unimplemented read at 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
    
    return val;
}

static void nv_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    
    switch (addr) {
    case NV_PRAMDAC_I2C_PORT_0:
        s->pramdac_i2c[0] = val;
        /* Handle I2C operations */
        if (val & 0x1) {
            uint8_t data = geforce_ddc_read(&s->ddc);
            s->pramdac_i2c[0] = (s->pramdac_i2c[0] & ~0xff00) | (data << 8);
        }
        break;
    case NV_PRAMDAC_I2C_PORT_1:
        s->pramdac_i2c[1] = val;
        geforce_ddc_write(&s->ddc, (val >> 8) & 0xff, val & 0xff);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "GeForce3: unimplemented write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps nv_mmio_ops = {
    .read = nv_mmio_read,
    .write = nv_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* VGA passthrough support */
static uint64_t nv_vga_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    return vga_ioport_read(&s->vga, addr);
}

static void nv_vga_ioport_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NVGFState *s = opaque;
    vga_ioport_write(&s->vga, addr, val);
}

static const MemoryRegionOps nv_vga_ops = {
    .read = nv_vga_ioport_read,
    .write = nv_vga_ioport_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* UI info callback for dynamic EDID updates */
static void nv_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    NVGFState *s = opaque;
    
    if (info->width && info->height) {
        /* Update EDID info with new display dimensions */
        s->edid_info.prefx = info->width;
        s->edid_info.prefy = info->height;
        
        /* Update physical dimensions if provided */
        if (info->width_mm && info->height_mm) {
            s->edid_info.width_mm = info->width_mm;
            s->edid_info.height_mm = info->height_mm;
        }
        
        /* Regenerate EDID with new info */
        qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
        
        qemu_log_mask(LOG_TRACE, "GeForce3: EDID updated to %dx%d\n", 
                      s->edid_info.prefx, s->edid_info.prefy);
    }
}

/* Device initialization */
static void nv_realize(PCIDevice *pci_dev, Error **errp)
{
    NVGFState *s = GEFORCE3(pci_dev);
    VGACommonState *vga = &s->vga;
    
    /* Initialize VGA */
    vga_common_init(vga, OBJECT(s));
    vga_init(vga, OBJECT(s), pci_address_space(pci_dev), 
             pci_address_space_io(pci_dev), true);
    
    /* Setup memory regions */
    memory_region_init_io(&s->mmio, OBJECT(s), &nv_mmio_ops, s, 
                         "geforce3-mmio", 0x1000000);
    memory_region_init_io(&s->vga_io, OBJECT(s), &nv_vga_ops, s,
                         "geforce3-vga", 0x1000);
    
    /* Register PCI BARs */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    pci_register_bar(pci_dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &s->vga_io);
    
    /* Initialize DDC/I2C */
    geforce_ddc_init(s);
    
    /* Create console */
    s->vga.con = graphic_console_init(DEVICE(pci_dev), 0, 
                                     &vga_ops, &s->vga);
    
    /* Register UI info callback for dynamic EDID */
    dpy_set_ui_info(s->vga.con, nv_ui_info, s);
    
    qemu_log_mask(LOG_TRACE, "GeForce3: device realized\n");
}

static void nv_reset(DeviceState *dev)
{
    NVGFState *s = GEFORCE3(dev);
    
    vga_common_reset(&s->vga);
    
    s->pramdac_i2c[0] = 0;
    s->pramdac_i2c[1] = 0;
    s->enable_vga = 1;
    
    /* Reinitialize DDC */
    geforce_ddc_init(s);
}

static void nv_init(Object *obj)
{
    /* Object initialization */
}

static Property nv_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", NVGFState, vga.vram_size_mb, 64),
    DEFINE_PROP_END_OF_LIST(),
};

static void nv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    
    pc->realize = nv_realize;
    pc->vendor_id = NV_VENDOR_ID;
    pc->device_id = NV_DEVICE_ID;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->subsystem_vendor_id = NV_VENDOR_ID;
    pc->subsystem_id = NV_DEVICE_ID;
    
    dc->reset = nv_reset;
    device_class_set_props(dc, nv_properties);
    dc->desc = "NVIDIA GeForce3";
    dc->user_creatable = true;
}

static const TypeInfo nv_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NVGFState),
    .instance_init = nv_init,
    .class_init = nv_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void nv_register_types(void)
{
    type_register_static(&nv_info);
}

type_init(nv_register_types)