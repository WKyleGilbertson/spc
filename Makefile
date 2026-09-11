KER ?= /home/beagle/lab/kernel/BeagleV-Fire-ubuntu/linux/
MODULE_DIR := $(shell pwd)

# User-space compiler settings
CC := gcc
CFLAGS := -O2

# Safely query Git hash or fallback without stderr noise
GIT_CI := $(shell git -C $(MODULE_DIR) rev-parse HEAD 2>/dev/null || echo "unknown")
BUILD_TIMESTAMP := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

ccflags-y += -DGIT_CI=\"$(GIT_CI)\"
ccflags-y += -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"

# Default target builds the kernel module and then the user-space applications
all: modules apps

modules:
	@echo "Using kernel dev tree: $(KER)"
	@echo "Building with Git hash: $(GIT_CI)"
	@echo "Build timestamp: $(BUILD_TIMESTAMP)"
	make -C $(KER) M=$(MODULE_DIR) modules

# Target to compile all user-space applications
apps: spc_mmap_test spc_reader arm_ch0

spc_mmap_test: spc_mmap_test.c spc.h
	$(CC) $(CFLAGS) spc_mmap_test.c -o spc_mmap_test

spc_reader: spc_reader.c spc.h
	$(CC) $(CFLAGS) spc_reader.c -o spc_reader

arm_ch0: arm_ch0.c spc.h
	$(CC) $(CFLAGS) arm_ch0.c -o arm_ch0

clean:
	# Clean kernel build artifacts
	make -C $(KER) M=$(MODULE_DIR) clean
	# Clean user-space binaries
	rm -f spc_mmap_test spc_reader arm_ch0

obj-m += spc.o
