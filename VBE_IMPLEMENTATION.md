# VBE DISPI Implementation Documentation

## Overview

This document describes the comprehensive VBE (VESA BIOS Extensions) DISPI (Display Interface) implementation added to the QEMU GeForce3 emulation. The implementation provides full compatibility with the Bochs VBE model while maintaining integration with the existing QEMU GeForce emulation.

## Features Implemented

### 1. Complete VBE DISPI Register Set
- **VBE_DISPI_INDEX_ID** (0x0): VBE version identification (read-only, returns VBE_DISPI_ID5)
- **VBE_DISPI_INDEX_XRES** (0x1): Horizontal resolution
- **VBE_DISPI_INDEX_YRES** (0x2): Vertical resolution  
- **VBE_DISPI_INDEX_BPP** (0x3): Bits per pixel (8, 15, 16, 24, 32)
- **VBE_DISPI_INDEX_ENABLE** (0x4): Enable/disable and mode flags
- **VBE_DISPI_INDEX_BANK** (0x5): Banking window selection
- **VBE_DISPI_INDEX_VIRT_WIDTH** (0x6): Virtual screen width
- **VBE_DISPI_INDEX_VIRT_HEIGHT** (0x7): Virtual screen height
- **VBE_DISPI_INDEX_X_OFFSET** (0x8): Horizontal panning offset
- **VBE_DISPI_INDEX_Y_OFFSET** (0x9): Vertical panning offset

### 2. VBE Banking Support
- 64KB banking window at standard granularity
- Automatic bank switching and bounds checking
- Compatible with legacy software expecting banked access
- Bank register updates memory mapping transparently

### 3. Linear Framebuffer (LFB) Mode
- Direct VRAM access without banking
- Large memory regions support
- Optimal performance for modern applications
- Controlled via VBE_DISPI_LFB_ENABLED flag

### 4. Virtual Resolution and Panning
- Virtual screen dimensions larger than physical display
- X/Y offset registers for smooth panning/scrolling
- Automatic display start address calculation
- Bounds checking for virtual resolution and offsets

### 5. Mode Validation and VGA Fallback
- Comprehensive parameter validation
- Memory requirement checking against available VRAM
- Automatic fallback to VGA mode on invalid parameters
- Detailed error logging for debugging

### 6. Enhanced Memory Access
- Banking vs linear mode detection
- Efficient dirty region tracking for display updates
- Display update notifications for optimal refresh
- Support for 1, 2, 4, and 8-byte memory accesses

## I/O Port Interface

The VBE DISPI interface uses standard ports:
- **0x01CE**: Index register (selects which VBE register to access)
- **0x01CF**: Data register (reads/writes the selected VBE register)

## Memory Layout

- **BAR 0**: PRMVIO registers and VBE I/O ports
- **BAR 1**: VGA VRAM (standard VGA memory)
- **BAR 2**: CRTC registers
- **BAR 3**: Linear framebuffer (VBE LFB access)

## Constants and Limits

```c
#define VBE_DISPI_MAX_XRES      4096    // Maximum horizontal resolution
#define VBE_DISPI_MAX_YRES      4096    // Maximum vertical resolution  
#define VBE_DISPI_MAX_BPP       32      // Maximum bits per pixel
#define VBE_DISPI_BANK_SIZE     65536   // 64KB banking window size
```

## Supported BPP Modes
- 8-bit: Palette-based color
- 15-bit: RGB555 (5:5:5)
- 16-bit: RGB565 (5:6:5) 
- 24-bit: RGB888 (8:8:8)
- 32-bit: RGBA8888 (8:8:8:8)

## Enable Flags

```c
#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80
```

## Usage Example

```c
// Set VBE mode: 1024x768x32bpp with virtual 1280x1024 and LFB
outw(0x01CE, VBE_DISPI_INDEX_XRES);     outw(0x01CF, 1024);
outw(0x01CE, VBE_DISPI_INDEX_YRES);     outw(0x01CF, 768);
outw(0x01CE, VBE_DISPI_INDEX_BPP);      outw(0x01CF, 32);
outw(0x01CE, VBE_DISPI_INDEX_VIRT_WIDTH);  outw(0x01CF, 1280);
outw(0x01CE, VBE_DISPI_INDEX_VIRT_HEIGHT); outw(0x01CF, 1024);
outw(0x01CE, VBE_DISPI_INDEX_ENABLE);   outw(0x01CF, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

// Pan to position (256, 128)
outw(0x01CE, VBE_DISPI_INDEX_X_OFFSET); outw(0x01CF, 256);
outw(0x01CE, VBE_DISPI_INDEX_Y_OFFSET); outw(0x01CF, 128);
```

## Compatibility

The implementation is fully compatible with:
- Bochs VBE DISPI interface
- Standard VBE software and drivers
- Both legacy (banked) and modern (LFB) applications
- QEMU VGA subsystem and display management
- NVIDIA GeForce3 register layout and behavior

## Error Handling

- Invalid register access returns 0 and logs error
- Invalid mode parameters trigger VGA fallback
- Memory overflow conditions are detected and handled
- Virtual resolution validation prevents invalid configurations
- Banking bounds checking prevents VRAM overflow

## Performance Optimizations

- Efficient dirty region tracking for display updates
- Minimal memory access validation overhead
- Direct VRAM access in LFB mode
- Optimized display update notifications
- Banking window caching for repeated accesses

## Testing

The implementation has been thoroughly tested with:
- Various VBE modes (8, 15, 16, 24, 32 bpp)
- Banking vs linear framebuffer modes
- Mode switching scenarios
- Virtual resolution and panning
- VGA fallback conditions
- Edge cases and error conditions
- Memory boundary conditions
- Register access validation

This VBE DISPI implementation provides comprehensive VBE support matching the Bochs implementation while maintaining full compatibility with QEMU's VGA subsystem and the GeForce3 emulation architecture.