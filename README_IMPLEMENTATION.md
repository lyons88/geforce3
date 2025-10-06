# GeForce3 QEMU Emulation with Dynamic EDID

This implementation provides a NVIDIA GeForce3 graphics card emulation for QEMU with the following features:

## Key Features Implemented

### 1. Dynamic EDID Support
- **qemu_edid_info** structure in NVGFState for storing display information
- **edid_blob[256]** buffer for generated EDID data  
- **edid_enabled** flag to control EDID functionality
- Dynamic EDID generation using `qemu_edid_generate()`

### 2. Enhanced DDC/I2C Implementation
- **geforce_ddc_init()** function initializes I2C bus and DDC device
- **geforce_ddc_read()** and **geforce_ddc_write()** functions handle DDC communication
- Dynamic EDID data served through DDC protocol
- Support for standard DDC addresses (0x50/0x51)

### 3. VGA Pass-through and Compatibility
- Full VGA common state integration
- VGA I/O port operations preserved
- Standard VGA memory access maintained
- VGA reset functionality preserved

### 4. PRMVIO Mirrors and LFB Support
- PRMVIO memory region (0x1000 bytes) for VGA register mirrors
- Large frame buffer (LFB) support through VGA VRAM
- Memory-mapped I/O operations for register access

### 5. CRTC Extensions and VBE Support
- Dedicated CRTC memory region with DDC integration
- CRTC status and control register support
- VBE register array for VESA BIOS Extensions
- VBE index register handling

### 6. UI Info Callback for Dynamic EDID
- **geforce_ui_info()** callback registered with QEMU console
- Automatic EDID regeneration when display resolution changes
- Real-time EDID updates propagated to DDC device
- Support for maximum resolution tracking

## Technical Implementation Details

### Memory Layout
- **BAR 0**: PRMVIO registers (0x1000 bytes)
- **BAR 1**: VGA VRAM (frame buffer)
- **BAR 2**: CRTC registers with DDC at offset 0x50-0x5F

### EDID Generation
- Uses QEMU's standard EDID generation functions
- Vendor ID: "NVD " (NVIDIA)
- Device name: "GeForce3"
- Supports resolution preferences and maximums
- 256-byte EDID blob compatible with DDC protocol

### DDC Protocol
- Standard I2C DDC implementation
- Address 0x50 for EDID read access
- SDA/SCL pin simulation for I2C communication
- Automatic EDID data serving through I2C DDC device

## Integration with QEMU

This implementation follows QEMU's device model:
- Inherits from PCIDevice base class
- Uses standard QEMU memory regions
- Integrates with QEMU's graphics console system
- Compatible with QEMU's build system (meson)

## Building

This code is designed to be integrated into QEMU's build system:

1. Copy files to QEMU source tree
2. Add to appropriate hw/display/ directory
3. Update meson.build files to include geforce3 module
4. Build QEMU with the new device

## Usage

```bash
qemu-system-x86_64 -device geforce3 [other options]
```

The device will automatically:
- Initialize with default EDID (1024x768 preferred, 1600x1200 max)
- Respond to DDC EDID requests
- Update EDID when display resolution changes
- Provide VGA compatibility for basic operations