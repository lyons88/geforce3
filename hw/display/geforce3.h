#ifndef HW_DISPLAY_GEFORCE3_H
#define HW_DISPLAY_GEFORCE3_H

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/vga_int.h"
#include "ui/console.h"

#define TYPE_GEFORCE3 "geforce3"
#define GEFORCE3(obj) OBJECT_CHECK(NVGFState, (obj), TYPE_GEFORCE3)

/* DDC/I2C State structure */
typedef struct DDCState {
    bool scl_out;
    bool sda_out;
    bool scl_in;
    bool sda_in;
} DDCState;

/* GeForce3 State structure */
typedef struct NVGFState {
    PCIDevice pci_dev;
    VGACommonState vga;
    QemuConsole *con;
    
    /* Memory regions */
    MemoryRegion mmio;
    MemoryRegion vram;
    MemoryRegion lfb;
    
    /* I2C/DDC support */
    bitbang_i2c_interface bbi2c;
    DDCState ddc;
    I2CBus *i2c_bus;
    
    /* Device registers */
    uint32_t gpio_regs[32];
    uint32_t crtc_regs[256];
    
    /* VBE support */
    uint16_t vbe_mode;
    uint32_t vbe_fb_base;
    uint32_t vbe_fb_size;
    
    /* Device state */
    bool vga_enabled;
    bool lfb_enabled;
    
} NVGFState;

/* Function declarations */
void geforce3_init(NVGFState *s);
void geforce3_reset(DeviceState *dev);

#endif /* HW_DISPLAY_GEFORCE3_H */