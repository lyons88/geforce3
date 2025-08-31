# GeForce3 QEMU Device Makefile
# This builds the GeForce3 device emulation for QEMU

# Target object file
GEFORCE3_OBJ = geforce3.o

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
QEMU_CFLAGS = -I/usr/include/qemu -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include

# Build target
all: $(GEFORCE3_OBJ)

$(GEFORCE3_OBJ): geforce3.c
	$(CC) $(CFLAGS) $(QEMU_CFLAGS) -c -o $@ $<

# Clean build artifacts
clean:
	rm -f $(GEFORCE3_OBJ)

# Install (copy to QEMU device directory)
install: $(GEFORCE3_OBJ)
	@echo "GeForce3 device object built successfully"
	@echo "To integrate with QEMU, add $(GEFORCE3_OBJ) to your QEMU build"

.PHONY: all clean install