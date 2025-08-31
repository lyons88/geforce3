# GeForce3 Nouveau Driver Compatibility Fix

This document describes the changes made to fix nouveau driver "unknown chipset" detection errors when using the GeForce3 emulation in QEMU.

## Problem Description

The nouveau driver performs chipset detection by reading specific NVIDIA registers, particularly the PMC_BOOT_0 register at offset 0x000000. The original GeForce3 emulation was missing these critical registers, causing nouveau to fail with "unknown chipset" and "fail to probe 0000:00:03.0" errors.

## Solution Overview

The fix adds proper NVIDIA-specific register emulation that matches what the nouveau driver expects for GeForce3 (NV20 architecture) chipsets.

## Key Changes Made

### 1. Added NVIDIA Register Definitions

```c
/* NVIDIA register offsets */
#define NV_PMC_BOOT_0           0x000000
#define NV_PMC_INTR_0           0x000100
#define NV_PMC_INTR_EN_0        0x000140
#define NV_PBUS_PCI_NV_1        0x001804

/* NV20 (GeForce3) architecture constants */
#define NV_ARCH_20              0x20
#define NV_IMPL_GEFORCE3        0x00
```

### 2. Enhanced Device State Structure

Added NVIDIA-specific registers to the NVGFState structure:

```c
/* NVIDIA-specific registers */
uint32_t pmc_boot_0;
uint32_t pmc_intr_0;
uint32_t pmc_intr_en_0;
uint32_t architecture;
uint32_t implementation;
```

### 3. Implemented Critical Functions

#### nv_compute_boot0()
Calculates the PMC_BOOT_0 register value in the format nouveau expects:
- Bits 31-20: Architecture (0x02 for shifted NV20)
- Bits 19-16: Architecture again (0x02 for NV20)  
- Bits 15-4: Implementation details
- Result: 0x02200000 for standard GeForce3

#### nv_apply_model_ids()
Sets up the device architecture and implementation values:
- Architecture: NV_ARCH_20 (0x20)
- Implementation: NV_IMPL_GEFORCE3 (0x00)
- Computes and stores PMC_BOOT_0 value

#### nv_bar0_readl()
Comprehensive BAR0 register read handler that responds to nouveau's register queries:
- PMC_BOOT_0: Returns computed chipset identification
- PMC_INTR_0: Returns interrupt status
- PMC_INTR_EN_0: Returns interrupt enable mask
- PBUS_PCI_NV_1: Returns PCI vendor/device ID mirror

### 4. Updated Memory Operations

Modified the PRMVIO read/write operations to use the new register handlers, ensuring proper response to nouveau driver register accesses.

## Register Values

| Register | Offset | Value | Purpose |
|----------|--------|-------|---------|
| PMC_BOOT_0 | 0x000000 | 0x02200000 | Chipset identification (critical) |
| PMC_INTR_0 | 0x000100 | 0x00000000 | Interrupt status |
| PMC_INTR_EN_0 | 0x000140 | 0x00000000 | Interrupt enable |
| PBUS_PCI_NV_1 | 0x001804 | 0x10DE0200 | PCI ID mirror |

## Verification

The `verify_nouveau.sh` script validates the implementation:

```bash
./verify_nouveau.sh
```

Expected output shows PMC_BOOT_0 = 0x02200000, which nouveau recognizes as NV20 (GeForce3) architecture.

## Testing Integration

When integrated with QEMU, test with:

```bash
# Boot Linux guest with GeForce3 device
qemu-system-x86_64 -device geforce3 -boot d linux.iso

# In guest, check nouveau detection:
dmesg | grep nouveau
lspci | grep NVIDIA
```

Expected success indicators:
- `nouveau: chipset NV20 detected`
- `nouveau: successfully probed 0000:00:03.0`
- No "unknown chipset" errors in dmesg

## Compatibility

This fix maintains full backward compatibility with existing VGA functionality while adding nouveau driver support. The changes are minimal and surgical, affecting only the specific registers nouveau requires for chipset detection.

## Architecture Details

The GeForce3 uses the NV20 architecture in NVIDIA's naming scheme:
- **NV20**: Base GeForce3 architecture
- **0x20**: Architecture identifier in PMC_BOOT_0 register
- **0x0200**: PCI device ID for standard GeForce3

The nouveau driver specifically looks for these values to identify and properly initialize GeForce3 hardware support.