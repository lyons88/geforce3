# GeForce3 QEMU Device Emulation Makefile
# This demonstrates how the GeForce3 device would be integrated into QEMU's build system

# Compiler settings for QEMU device development
CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -fPIC -O2
INCLUDES = -I. -I/usr/include/qemu -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include

# Object files
OBJS = geforce3.o

# Target library (would be integrated into QEMU's hw/display/ directory)
TARGET = libgeforce3.so

# Default target
all: $(TARGET)

# Build the shared library
$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $^

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Install to QEMU plugins directory (example)
install: $(TARGET)
	@echo "Install target would copy to QEMU device directory"
	@echo "In actual QEMU build, this would be part of hw/display/Makefile"

# Documentation target
docs:
	@echo "GeForce3 QEMU Device Implementation"
	@echo "==================================="
	@echo "This implementation provides:"
	@echo "- Dynamic EDID generation using qemu_edid_generate()"
	@echo "- UI info callbacks for display change detection"
	@echo "- DDC/I2C interface for EDID communication"
	@echo "- PCI device integration with proper memory regions"
	@echo "- Support for resolutions beyond static 1024x768"

# Check syntax without full QEMU dependencies
syntax-check:
	$(CC) $(CFLAGS) -fsyntax-only -Wno-implicit-function-declaration geforce3.c

.PHONY: all clean install docs syntax-check