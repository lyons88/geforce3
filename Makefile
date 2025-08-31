# GeForce3 Emulation Build Configuration
# This file is for integration with QEMU build system

# Source files
SOURCES = hw/display/geforce3.c

# Object files  
OBJECTS = $(SOURCES:.c=.o)

# QEMU includes (adjust path as needed)
QEMU_INCLUDES = -I/usr/include/qemu -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include

# Compiler flags
CFLAGS = -Wall -Wextra -std=gnu99 $(QEMU_INCLUDES)

# Default target
all: $(OBJECTS)

# Compile rule
%.o: %.c
	gcc $(CFLAGS) -c $< -o $@

# Clean rule
clean:
	rm -f $(OBJECTS)

# Syntax check
check: hw/display/geforce3.c
	gcc $(CFLAGS) -fsyntax-only hw/display/geforce3.c

.PHONY: all clean check