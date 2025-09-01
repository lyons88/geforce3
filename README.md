# GeForce3 Emulation for QEMU

Advanced GeForce3 graphics card emulation ported from Bochs to QEMU with comprehensive features and logging.

## Features

### Multi-Model Support
Support for 8 different GeForce models via `-device geforce,model=xxx`:
- `geforce-ddr` - GeForce DDR (NV10)
- `geforce2-mx` - GeForce2 MX (NV11) 
- `geforce2-gts` - GeForce2 GTS (NV15)
- `geforce3` - GeForce3 (NV20) - Default
- `geforce3-ti200` - GeForce3 Ti 200 (NV20)
- `geforce3-ti500` - GeForce3 Ti 500 (NV20)
- `geforce4-mx` - GeForce4 MX (NV17)
- `geforce4-ti` - GeForce4 Ti (NV25)

### VRAM Configuration
Configurable VRAM size from 64MB to 512MB via `-device geforce,vramsize=xxM`

### VBE Fallback Support
Seamless VBE fallback from QEMU VGA STD when GeForce VBE is not active or requested.
This is additive - does not interfere with existing GeForce functionality.

### Comprehensive Logging
All device operations are logged using `qemu_log_mask(LOG_GUEST_ERROR, ...)`:
- All MMIO reads/writes (BAR0/BAR2) with address, value, and size
- VBE and CRTC register accesses
- Mode sets and framebuffer changes  
- Legacy VGA port accesses
- DDC/I2C operations
- Unsupported feature accesses where guest could get confused
- Logging throttling to prevent spam while maintaining coverage

### Device Properties
Full command-line support:
```bash
-device geforce,model=geforce3-ti500,vramsize=256M,romfile=/path/to/rom.bin
```

## Architecture

### Memory Layout
- **BAR0**: MMIO registers (4KB) - NVIDIA control registers
- **BAR1**: Video RAM (64-512MB) - Frame buffer memory  
- **BAR2**: CRTC registers (4KB) - Display controller and DDC

### VGA Compatibility
- Full VGA compatibility layer through QEMU VGA infrastructure
- Legacy VGA port support with comprehensive logging
- Standard VGA modes supported alongside GeForce extensions

### DDC/I2C Support  
- EDID generation and management
- Dynamic resolution updates
- Monitor information via DDC

## Implementation Notes

### Additive Design
All enhancements are additive - no existing functionality was removed:
- VBE fallback works alongside existing VGA
- Logging added without changing core logic
- Model support extends existing GeForce3 base
- All compilation fixes preserved

### Nouveau Driver Compatibility
- Proper PMC_BOOT_0 register implementation for chipset detection
- Architecture and implementation codes for all supported models
- PCI device IDs correctly set based on model selection

### Error Handling
- Comprehensive validation of device properties
- Graceful fallback for unsupported operations
- Clear logging of potential guest confusion points

## Usage Examples

```bash
# Basic GeForce3 with default 128MB VRAM
-device geforce3

# GeForce3 Ti 500 with 256MB VRAM
-device geforce,model=geforce3-ti500,vramsize=256M

# GeForce4 Ti with maximum 512MB VRAM and custom ROM
-device geforce,model=geforce4-ti,vramsize=512M,romfile=/path/to/bios.rom

# GeForce2 MX with minimal 64MB VRAM
-device geforce,model=geforce2-mx,vramsize=64M
```

## Development Status

- ✅ Multi-model support (8 GeForce variants)
- ✅ VRAM configuration (64-512MB)
- ✅ Comprehensive logging with throttling
- ✅ VBE fallback integration
- ✅ Device property support
- ✅ DDC/I2C and EDID support
- ✅ VGA compatibility layer
- ✅ Nouveau driver compatibility
- ⚠️ ROM file loading (property supported, loading TODO)
- ⚠️ Advanced 3D acceleration (basic framework present)

## Logging Output

When enabled, you'll see detailed logs like:
```
GeForce3: Device initialization started
GeForce3: Model=geforce3-ti500 VRAM=256MB ROM=none
GeForce3: Configured as GeForce3 Ti 500 (NV20)
GeForce3: VRAM configured to 256MB
GeForce3: VBE fallback initialized
GeForce3: Memory regions mapped - BAR0(MMIO) BAR1(VRAM) BAR2(CRTC)
GeForce3: BAR0 MMIO read addr=0x00000000 size=4 value=0x20200000
GeForce3: PMC_BOOT_0 read: 0x20200000 (arch=0x20 impl=0x02)
```
