KER ?= /home/beagle/lab/kernel/BeagleV-Fire-ubuntu/linux/
MODULE_DIR := $(shell pwd)

# Safely query Git hash or fallback without stderr noise
GIT_CI := $(shell git -C $(MODULE_DIR) rev-parse HEAD 2>/dev/null || echo "unknown")
BUILD_TIMESTAMP := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

ccflags-y += -DGIT_CI=\"$(GIT_CI)\"
ccflags-y += -DBUILD_TIMESTAMP=\"$(BUILD_TIMESTAMP)\"

all:
	@echo "Using kernel dev tree: $(KER)"
	@echo "Building with Git hash: $(GIT_CI)"
	@echo "Build timestamp: $(BUILD_TIMESTAMP)"
	make -C $(KER) M=$(MODULE_DIR) modules

clean:
	make -C $(KER) M=$(MODULE_DIR) clean

obj-m += spc.o