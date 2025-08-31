/*
 * QEMU GeForce3 GPU Emulation with Dynamic EDID Support
 *
 * This implementation provides GeForce3 emulation for QEMU with proper
 * integration of QEMU's EDID generation system for dynamic display detection.
 */

#include "geforce3.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

/* UI info callback for dynamic display updates */
static void geforce3_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    GeForce3State *s = opaque;
    
    if (info->width_mm > 0 && info->height_mm > 0) {
        s->edid_info.width_mm = info->width_mm;
        s->edid_info.height_mm = info->height_mm;
    }
    
    if (info->xoff == 0 && info->yoff == 0 && 
        info->width > 0 && info->height > 0) {
        s->current_width = info->width;
        s->current_height = info->height;
        
        /* Update EDID with new display information */
        geforce3_update_edid(s);
    }
}

/* Update EDID data using QEMU's dynamic EDID generation */
void geforce3_update_edid(GeForce3State *s)
{
    /* Set up EDID info structure with current display parameters */
    s->edid_info.prefx = s->current_width;
    s->edid_info.prefy = s->current_height;
    s->edid_info.maxx = s->current_width;
    s->edid_info.maxy = s->current_height;
    
    /* Use QEMU's EDID generation instead of static data */
    qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
    s->edid_ready = true;
    
    qemu_log_mask(LOG_GUEST, "GeForce3: Updated EDID for %dx%d display\n",
                  s->current_width, s->current_height);
}

/* DDC/I2C communication for EDID data */
static uint32_t geforce3_ddc_read(GeForce3State *s, uint32_t addr)
{
    uint32_t val = 0;
    
    if (!s->edid_ready) {
        return 0xff;
    }
    
    if (addr < sizeof(s->edid_blob)) {
        val = s->edid_blob[addr];
    }
    
    return val;
}

/* Handle DDC/I2C register updates */
void geforce3_ddc_i2c_update(GeForce3State *s)
{
    /* Simple DDC implementation for EDID access */
    /* This would be expanded for full I2C protocol support */
}

/* MMIO read handler */
uint64_t geforce3_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    GeForce3State *s = opaque;
    uint64_t val = 0;
    
    switch (addr) {
    case GEFORCE3_I2C_DATA:
        /* DDC data read for EDID */
        val = geforce3_ddc_read(s, s->regs[GEFORCE3_I2C_DATA / 4] & 0xff);
        break;
        
    case GEFORCE3_I2C_CLK:
        val = s->i2c_ddc_scl ? 1 : 0;
        break;
        
    case GEFORCE3_I2C_STATUS:
        val = s->edid_ready ? 1 : 0;
        break;
        
    default:
        if (addr < GEFORCE3_MMIO_SIZE) {
            val = s->regs[addr / 4];
        }
        break;
    }
    
    return val;
}

/* MMIO write handler */
void geforce3_mmio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    GeForce3State *s = opaque;
    
    switch (addr) {
    case GEFORCE3_I2C_DATA:
        s->regs[addr / 4] = data;
        geforce3_ddc_i2c_update(s);
        break;
        
    case GEFORCE3_I2C_CLK:
        s->i2c_ddc_scl = (data & 1) ? true : false;
        geforce3_ddc_i2c_update(s);
        break;
        
    default:
        if (addr < GEFORCE3_MMIO_SIZE) {
            s->regs[addr / 4] = data;
        }
        break;
    }
}

static const MemoryRegionOps geforce3_mmio_ops = {
    .read = geforce3_mmio_read,
    .write = geforce3_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* Device initialization */
static void geforce3_realize(PCIDevice *pci_dev, Error **errp)
{
    GeForce3State *s = GEFORCE3(pci_dev);
    
    /* Initialize display parameters with reasonable defaults */
    s->current_width = 1024;
    s->current_height = 768;
    s->current_depth = 32;
    
    /* Set up EDID info structure */
    s->edid_info.vendor = "QEM";
    s->edid_info.name = "QEMU GeForce3";
    s->edid_info.serial = "1";
    s->edid_info.width_mm = 300;  /* Default 300mm width */
    s->edid_info.height_mm = 225; /* Default 225mm height (4:3 aspect) */
    s->edid_info.prefx = s->current_width;
    s->edid_info.prefy = s->current_height;
    s->edid_info.maxx = 2048;     /* Maximum supported width */
    s->edid_info.maxy = 1536;     /* Maximum supported height */
    
    /* Generate initial EDID */
    geforce3_update_edid(s);
    
    /* Set up memory regions */
    memory_region_init_io(&s->mmio, OBJECT(s), &geforce3_mmio_ops, s,
                          "geforce3.mmio", GEFORCE3_MMIO_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->mmio);
    
    memory_region_init_ram(&s->vram, OBJECT(s), "geforce3.vram",
                           GEFORCE3_VRAM_SIZE, errp);
    if (*errp) {
        return;
    }
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->vram);
    
    /* Create console for display output */
    s->console = graphic_console_init(DEVICE(pci_dev), 0, NULL, s);
    
    /* Register UI info callback for dynamic display detection */
    dpy_set_ui_info(s->console, geforce3_ui_info, s, false);
    
    qemu_log_mask(LOG_GUEST, "GeForce3: Device initialized with EDID support\n");
}

/* Device reset */
static void geforce3_reset(DeviceState *dev)
{
    GeForce3State *s = GEFORCE3(dev);
    
    /* Reset DDC/I2C state */
    s->i2c_ddc_in = false;
    s->i2c_ddc_out = false;
    s->i2c_ddc_scl = true;
    s->i2c_ddc_sda = true;
    
    /* Clear MMIO registers */
    memset(s->regs, 0, sizeof(s->regs));
    
    /* Regenerate EDID to ensure it's fresh */
    geforce3_update_edid(s);
}

/* Class initialization */
static void geforce3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    
    pc->realize = geforce3_realize;
    pc->vendor_id = PCI_VENDOR_ID_NVIDIA;
    pc->device_id = PCI_DEVICE_ID_GEFORCE3;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->subsystem_vendor_id = PCI_VENDOR_ID_NVIDIA;
    pc->subsystem_id = PCI_DEVICE_ID_GEFORCE3;
    
    dc->reset = geforce3_reset;
    dc->desc = "NVIDIA GeForce3 with Dynamic EDID";
    
    /* This device supports hotplug */
    dc->hotpluggable = true;
}

static const TypeInfo geforce3_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GeForce3State),
    .class_init = geforce3_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* Module registration */
static void geforce3_register_types(void)
{
    type_register_static(&geforce3_info);
}

type_init(geforce3_register_types)