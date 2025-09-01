# VBE Fallback Implementation for GeForce3

## Overview

This implementation adds VBE (VESA BIOS Extensions) fallback support to the GeForce3 emulation, allowing it to work with standard VBE calls while providing GeForce-specific extensions when available.

## Architecture

The VBE fallback mechanism intercepts I/O port accesses to the standard VBE ports (0x01CE/0x01CF) and handles them through a two-tier approach:

1. **Primary**: GeForce-specific VBE handling for supported registers
2. **Fallback**: Standard VGA VBE handling for unsupported registers

## Key Components

### 1. VBE I/O Port Handling

- **VBE_DISPI_IOPORT_INDEX (0x01CE)**: VBE register index port
- **VBE_DISPI_IOPORT_DATA (0x01CF)**: VBE register data port

### 2. VBE Fallback Functions

#### `geforce_vbe_read_fallback()`
- Handles reads from VBE ports
- Returns GeForce-specific values for supported registers (ID, enable, resolution, etc.)
- Falls back to standard VGA VBE for unsupported registers
- Sets `vbe_fallback_active` flag when fallback is used

#### `geforce_vbe_write_fallback()`
- Handles writes to VBE ports
- Processes GeForce-specific VBE register writes
- Falls back to standard VGA VBE for unsupported operations
- Resets fallback state on index register changes

### 3. State Management

The `NVGFState` structure includes:
- `vbe_index`: Current VBE register index
- `vbe_regs[16]`: GeForce-specific VBE register values
- `vbe_enabled`: VBE mode enabled flag
- `vbe_fallback_active`: Tracks when standard VGA VBE is active

### 4. Supported VBE Registers

The GeForce implementation handles these standard VBE registers:
- `VBE_DISPI_INDEX_ID`: Returns VBE_DISPI_ID5 (latest VBE version)
- `VBE_DISPI_INDEX_XRES`: Horizontal resolution
- `VBE_DISPI_INDEX_YRES`: Vertical resolution
- `VBE_DISPI_INDEX_BPP`: Bits per pixel
- `VBE_DISPI_INDEX_ENABLE`: VBE enable/disable control
- `VBE_DISPI_INDEX_BANK`: Bank switching (for compatibility)
- `VBE_DISPI_INDEX_VIRT_WIDTH/HEIGHT`: Virtual screen dimensions
- `VBE_DISPI_INDEX_X/Y_OFFSET`: Screen offset

## Initialization

The VBE system is initialized in `geforce_vbe_init()`:
- Sets default VBE register values (1024x768@32bpp)
- Initializes state flags
- Called after VGA initialization in the device realize function

## VMState Integration

VBE state is preserved across save/restore operations through the VMState mechanism:
- VBE index and register array
- Enable and fallback state flags
- Integrated with existing GeForce3 VMState structure

## Benefits

1. **Compatibility**: Works with both GeForce-aware and standard VBE software
2. **Seamless Fallback**: Automatically uses standard VGA VBE when GeForce VBE doesn't handle a request
3. **State Preservation**: Proper save/restore functionality for migration
4. **Standards Compliance**: Follows VESA VBE specification for I/O port behavior

## Testing

The implementation has been validated with a comprehensive test suite that verifies:
- VBE ID detection
- Enable/disable functionality
- Resolution setting and reading
- Fallback activation for unsupported registers
- State management across operations

## Integration with Bochs Compatibility

This implementation matches the VBE behavior of Bochs GeForce emulation:
- GeForce-specific VBE extensions are tried first
- Standard VGA VBE is used as fallback
- I/O port routing is handled transparently
- Maintains compatibility with existing guest software