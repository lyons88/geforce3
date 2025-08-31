# GeForce3 emulation for QEMU
# This file integrates the GeForce3 device with QEMU's meson build system

geforce3_ss = ss.source_set()
geforce3_ss.add(files('geforce3.c'))

# Add dependencies
geforce3_ss.add(hw_display)
geforce3_ss.add(hw_pci)
geforce3_ss.add(hw_i2c)

# Register the module
hw_display_modules += {'geforce3': geforce3_ss}