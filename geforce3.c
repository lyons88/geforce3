/*
 * QEMU GeForce 3 graphics card emulation with EDID support
 *
 * Based on VGA emulation with DDC/I2C and dynamic EDID generation
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/display/vga.h"
#include "hw/display/vga_int.h"
#include "hw/display/edid.h"
#include "ui/console.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

#define TYPE_GEFORCE3 "geforce3"
#define GEFORCE3(obj) OBJECT_CHECK(NVGFState, (obj), TYPE_GEFORCE3)

/* PCI IDs for GeForce 3 */
#define NVIDIA_VENDOR_ID    0x10de
#define GEFORCE3_DEVICE_ID  0x0200

/* DDC I2C States */
typedef enum {
    DDC_IDLE,
    DDC_START,
    DDC_ADDRESS,
    DDC_DATA,
    DDC_STOP
} DDCState;

typedef struct DDCDevice {
    DDCState state;
    uint8_t address;
    uint8_t data_pos;
    bool sda_in;
    bool scl_in;
    bool sda_out;
    bool scl_out;
} DDCDevice;

typedef struct NVGFState {
    PCIDevice parent_obj;
    
    VGACommonState vga;
    MemoryRegion mmio;
    MemoryRegion vram;
    
    /* DDC/I2C support */
    DDCDevice ddc;
    
    /* EDID support */
    qemu_edid_info edid_info;
    uint8_t edid_blob[256];
    
    /* Hardware cursor */
    uint32_t cursor_pos;
    uint32_t cursor_size;
    
    /* Extended CRTC registers */
    uint8_t crtc_ext[256];
    
} NVGFState;

/* DDC/I2C Protocol Implementation */
static void ddc_reset(DDCDevice *ddc)
{
    ddc->state = DDC_IDLE;
    ddc->address = 0;
    ddc->data_pos = 0;
    ddc->sda_out = true;
    ddc->scl_out = true;
}

static uint8_t ddc_read_byte(NVGFState *s, uint8_t offset)
{
    if (offset < sizeof(s->edid_blob)) {
        return s->edid_blob[offset];
    }
    return 0;
}

static void ddc_process_bit(NVGFState *s, bool sda, bool scl)
{
    DDCDevice *ddc = &s->ddc;
    
    /* Detect start condition */
    if (ddc->sda_in && !sda && scl) {
        ddc->state = DDC_START;
        ddc->data_pos = 0;
        return;
    }
    
    /* Detect stop condition */
    if (!ddc->sda_in && sda && scl) {
        ddc->state = DDC_STOP;
        ddc_reset(ddc);
        return;
    }
    
    /* Process on clock rising edge */
    if (!ddc->scl_in && scl) {
        switch (ddc->state) {
        case DDC_START:
            ddc->state = DDC_ADDRESS;
            ddc->data_pos = 0;
            break;
            
        case DDC_ADDRESS:
            if (ddc->data_pos < 8) {
                ddc->address = (ddc->address << 1) | (sda ? 1 : 0);
                ddc->data_pos++;
                if (ddc->data_pos == 8) {
                    /* Check if this is EDID address (0xA0 for write, 0xA1 for read) */
                    if ((ddc->address & 0xFE) == 0xA0) {
                        ddc->state = DDC_DATA;
                        ddc->data_pos = 0;
                        ddc->sda_out = false; /* ACK */
                    } else {
                        ddc->sda_out = true; /* NACK */
                        ddc_reset(ddc);
                    }
                }
            }
            break;
            
        case DDC_DATA:
            if (ddc->address & 0x01) {
                /* Read operation */
                uint8_t data = ddc_read_byte(s, ddc->data_pos);
                ddc->sda_out = (data >> (7 - (ddc->data_pos % 8))) & 1;
                ddc->data_pos++;
            }
            break;
            
        default:
            break;
        }
    }
    
    ddc->sda_in = sda;
    ddc->scl_in = scl;
}

/* VGA CRTC Extensions */
static uint64_t nv_crtc_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    if (addr < sizeof(s->crtc_ext)) {
        return s->crtc_ext[addr];
    }
    return 0;
}

static void nv_crtc_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    NVGFState *s = opaque;
    
    if (addr < sizeof(s->crtc_ext)) {
        s->crtc_ext[addr] = data;
    }
}

/* Hardware Cursor */
static uint64_t nv_cursor_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    
    switch (addr) {
    case 0x00:
        return s->cursor_pos;
    case 0x04:
        return s->cursor_size;
    default:
        return 0;
    }
}

static void nv_cursor_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    NVGFState *s = opaque;
    
    switch (addr) {
    case 0x00:
        s->cursor_pos = data;
        break;
    case 0x04:
        s->cursor_size = data;
        break;
    }
}

/* DDC I2C Register Access */
static uint64_t nv_ddc_read(void *opaque, hwaddr addr, unsigned size)
{
    NVGFState *s = opaque;
    uint32_t val = 0;
    
    /* Return current SDA/SCL state */
    if (s->ddc.sda_out) val |= 0x08;
    if (s->ddc.scl_out) val |= 0x10;
    
    return val;
}

static void nv_ddc_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    NVGFState *s = opaque;
    
    bool sda = (data & 0x08) != 0;
    bool scl = (data & 0x10) != 0;
    
    ddc_process_bit(s, sda, scl);
}

/* MMIO Region */
static uint64_t nv_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x9000 ... 0x9FFF:
        return nv_crtc_read(opaque, addr - 0x9000, size);
    case 0xA000 ... 0xA0FF:
        return nv_cursor_read(opaque, addr - 0xA000, size);
    case 0xB000:
        return nv_ddc_read(opaque, addr - 0xB000, size);
    default:
        return 0;
    }
}

static void nv_mmio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    switch (addr) {
    case 0x9000 ... 0x9FFF:
        nv_crtc_write(opaque, addr - 0x9000, data, size);
        break;
    case 0xA000 ... 0xA0FF:
        nv_cursor_write(opaque, addr - 0xA000, data, size);
        break;
    case 0xB000:
        nv_ddc_write(opaque, addr - 0xB000, data, size);
        break;
    }
}

static const MemoryRegionOps nv_mmio_ops = {
    .read = nv_mmio_read,
    .write = nv_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* UI Info Callback for Dynamic EDID */
static void nv_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    NVGFState *s = opaque;
    if (info->width && info->height) {
        s->edid_info.prefx = info->width;
        s->edid_info.prefy = info->height;
        qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
    }
}

/* Device Initialization */
static void geforce_ddc_init(DDCDevice *ddc)
{
    ddc_reset(ddc);
}

static void nv_realize(PCIDevice *pci_dev, Error **errp)
{
    NVGFState *s = GEFORCE3(pci_dev);
    VGACommonState *vga = &s->vga;
    
    /* Initialize VGA */
    vga_common_init(vga, OBJECT(s));
    vga_init(vga, OBJECT(s), pci_address_space(pci_dev),
             pci_address_space_io(pci_dev), true);
    
    /* Setup PCI config */
    pci_config_set_vendor_id(pci_dev->config, NVIDIA_VENDOR_ID);
    pci_config_set_device_id(pci_dev->config, GEFORCE3_DEVICE_ID);
    pci_config_set_class(pci_dev->config, PCI_CLASS_DISPLAY_VGA);
    
    /* Initialize VRAM */
    memory_region_init_ram(&s->vram, OBJECT(s), "geforce3.vram", 
                          64 * 1024 * 1024, errp);
    
    /* Setup PCI BARs */
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    
    /* Initialize MMIO */
    memory_region_init_io(&s->mmio, OBJECT(s), &nv_mmio_ops, s,
                         "geforce3.mmio", 0x1000000);
    pci_register_bar(pci_dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    
    /* Initialize DDC */
    geforce_ddc_init(&s->ddc);
    
    /* Initialize EDID info */
    s->edid_info.vendor = "NVD";
    s->edid_info.name = "QEMU GeForce";
    s->edid_info.prefx = 1024;
    s->edid_info.prefy = 768;
    qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
    
    /* Register UI callback */
    dpy_set_ui_info(s->vga.con, nv_ui_info, s);
}

static void nv_reset(DeviceState *dev)
{
    NVGFState *s = GEFORCE3(dev);
    
    vga_common_reset(&s->vga);
    geforce_ddc_init(&s->ddc);
    
    /* Reset cursor */
    s->cursor_pos = 0;
    s->cursor_size = 0;
    
    /* Reset extended CRTC */
    memset(s->crtc_ext, 0, sizeof(s->crtc_ext));
}

static void nv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    
    k->realize = nv_realize;
    k->vendor_id = NVIDIA_VENDOR_ID;
    k->device_id = GEFORCE3_DEVICE_ID;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    
    dc->reset = nv_reset;
    dc->desc = "NVIDIA GeForce 3";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo nv_info = {
    .name = TYPE_GEFORCE3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NVGFState),
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