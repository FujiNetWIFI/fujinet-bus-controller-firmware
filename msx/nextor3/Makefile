# FujiNet Nextor 3 ROM: Konamiman's kernel base file plus a driver bank that
# reaches the FujiNet through its UNAPI ROM.

NEXTOR_VERSION = 3.0.0
BASE_URL  = https://github.com/Konamiman/Nextor/releases/download/kernel-v$(NEXTOR_VERSION)/Nextor-$(NEXTOR_VERSION).base.dat

AS        = z88dk-z80asm
BUILD_DIR = ../../build/nextor3
BASE      = $(BUILD_DIR)/base/Nextor-$(NEXTOR_VERSION).base.dat
DRIVER    = $(BUILD_DIR)/driver.bin
ROM       = $(BUILD_DIR)/nextor3.rom

# 16K less the page 0 code and the bank switching code
DRIVER_SIZE = 16080

all: $(ROM)

# Driver Development Guide 3.1: page 0 code from the base file, the driver
# bank number, the driver, then the base file's ASCII16 switching code.
$(ROM): $(DRIVER) $(BASE)
	{ cat $(BASE); \
	  head -c 255 $(BASE); \
	  head -c 255 $(BASE) | tail -c 1; \
	  cat $(DRIVER); \
	  tail -c 48 $(BASE); } > $@
	@echo "$@: $$(wc -c < $@) bytes"

$(DRIVER): src/driver.asm src/nextor3.inc | $(BUILD_DIR)
	$(AS) -b -I=src -O=$(BUILD_DIR) -o=driver.raw src/driver.asm
	@n=$$(wc -c < $(BUILD_DIR)/driver.raw); \
	if [ $$n -gt $(DRIVER_SIZE) ]; then \
		echo "driver is $$n bytes, over $(DRIVER_SIZE)"; exit 1; fi; \
	echo "driver: $$n of $(DRIVER_SIZE) bytes"; \
	{ cat $(BUILD_DIR)/driver.raw; \
	  head -c $$(( $(DRIVER_SIZE) - n )) /dev/zero | LC_ALL=C tr '\0' '\377'; } > $@

$(BASE):
	mkdir -p $(dir $@)
	curl -sSfL -o $@ $(BASE_URL)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(DRIVER) $(BUILD_DIR)/driver.raw $(BUILD_DIR)/src $(ROM)

distclean:
	rm -rf $(BUILD_DIR)

.DELETE_ON_ERROR:

.PHONY: all clean distclean
