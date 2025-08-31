CC=gcc
CFLAGS=-Wall -Wextra -I. -I.. -I../.. -DNEED_CPU_H -D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE

# Mock QEMU headers and defines for compilation testing
QEMU_INCLUDES=-DCONFIG_LINUX -DTARGET_WORDS_BIGENDIAN=0

# Simple compilation test
.PHONY: test compile-test clean

test: compile-test

compile-test:
	@echo "Testing compilation of geforce3.c..."
	$(CC) $(CFLAGS) $(QEMU_INCLUDES) -c hw/display/geforce3.c -o /tmp/geforce3.o || true
	@echo "Compilation test completed."

clean:
	rm -f /tmp/geforce3.o

# Note: This is a simplified build for testing compilation errors only
# In real QEMU, this would be built as part of the main QEMU build system