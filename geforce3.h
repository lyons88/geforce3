#ifndef GEFORCE3_H
#define GEFORCE3_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/display/edid.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_GEFORCE3 "geforce3"
OBJECT_DECLARE_SIMPLE_TYPE(GeForce3State, GEFORCE3)

/* GeForce3 PCI vendor/device IDs */
#define PCI_VENDOR_ID_NVIDIA    0x10de
#define PCI_DEVICE_ID_GEFORCE3  0x0200

/* GeForce3 memory regions */
#define GEFORCE3_MMIO_SIZE      0x1000000  /* 16MB MMIO space */
#define GEFORCE3_VRAM_SIZE      0x8000000  /* 128MB VRAM */

/* DDC/I2C registers for EDID */
#define GEFORCE3_I2C_DATA       0x0036
#define GEFORCE3_I2C_CLK        0x0037
#define GEFORCE3_I2C_STATUS     0x0038

typedef struct GeForce3State {
    PCIDevice parent_obj;

    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion vram;
    
    /* Display and EDID support */
    QemuConsole *console;
    qemu_edid_info edid_info;
    uint8_t edid_blob[256];
    bool edid_ready;
    
    /* DDC/I2C state for EDID communication */
    bool i2c_ddc_in;
    bool i2c_ddc_out;
    bool i2c_ddc_scl;
    bool i2c_ddc_sda;
    
    /* Current display mode information */
    uint32_t current_width;
    uint32_t current_height;
    uint32_t current_depth;
    
    /* MMIO register state */
    uint32_t regs[GEFORCE3_MMIO_SIZE / 4];
    
} GeForce3State;

/* Function declarations */
void geforce3_update_edid(GeForce3State *s);
void geforce3_ddc_i2c_update(GeForce3State *s);
uint64_t geforce3_mmio_read(void *opaque, hwaddr addr, unsigned size);
void geforce3_mmio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size);

#endif /* GEFORCE3_H */