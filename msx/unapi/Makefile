PRODUCT = unapi
PLATFORMS += msxrom

SRC_DIRS = src src/%PLATFORM%
PLATFORM_COMBOS = msxrom+=msx

# Pinned MekkoGX, fetched on first build. Its MSXROM_FLAGS is made `?=` so
# the -subtype=callext below survives.
MEKKOGX_URL ?= https://github.com/FozzTexx/MekkoGX.git
MEKKOGX_REF ?= c9442e0
CACHE_DIR   ?= _cache
MEKKOGX     ?= $(CACHE_DIR)/mekkogx-$(MEKKOGX_REF)

# The literal included path, so make can remake it on a fresh checkout.
$(MEKKOGX)/mekkogx/toplevel-rules.mk:
	mkdir -p $(dir $(MEKKOGX))
	rm -rf $(MEKKOGX).tmp
	git clone $(MEKKOGX_URL) $(MEKKOGX).tmp
	git -C $(MEKKOGX).tmp checkout --detach \
	  $$(git -C $(MEKKOGX).tmp rev-parse --verify --quiet '$(MEKKOGX_REF)^{commit}' \
	     || git -C $(MEKKOGX).tmp rev-parse --verify 'origin/$(MEKKOGX_REF)^{commit}')
	sed -i.orig 's/^MSXROM_FLAGS = /MSXROM_FLAGS ?= /' $(MEKKOGX).tmp/mekkogx/platforms/msxrom.mk
	mv $(MEKKOGX).tmp $(MEKKOGX)

# Otherwise the clone rule above would be the default goal.
.DEFAULT_GOAL := all

include $(MEKKOGX)/mekkogx/toplevel-rules.mk

MSXROM_FLAGS = +msx -subtype=callext
MSX_EXTRA_DEPS = src/header/msx_rom_header.asm
LDFLAGS_EXTRA_MSXROM = -Ca-Isrc/header -create-app
