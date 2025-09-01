# GeForce3 QEMU Emulation

GeForce 3 graphics card emulation for QEMU with dynamic EDID support.

## Features

- **VGA Emulation**: Full VGA compatibility with extended CRTC registers
- **Dynamic EDID**: Automatic EDID generation based on display characteristics
- **DDC/I2C Protocol**: Complete I2C implementation for monitor communication
- **Hardware Cursor**: Dedicated cursor position and size control
- **PCI Integration**: Standard PCI graphics device with memory regions

## Building

```bash
make
```

## Testing

Run the EDID test suite:
```bash
make test
./test_edid
```

## Integration

This device can be integrated into QEMU by adding `geforce3.c` to the display device build and using:
```bash
qemu-system-x86_64 -device geforce3
```

## Implementation

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for detailed technical documentation.
