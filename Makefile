BOARD ?= picorom_msx

ifeq ($(BOARD),coco_proto_260402)
  # FujiDOS 1.0 release, RichStephens/fujidos-coco
  ROM_IMAGE = fujidos.rom
else ifeq ($(BOARD),picorom_coco)
  ROM_IMAGE = hdbdw3bc3.rom
else
  ROM_IMAGE = config-msx.rom
endif

BUILD_DIR = build/$(BOARD)
BUILD_MAKE = $(BUILD_DIR)/Makefile
FIRMWARE = fujiversal_$(BOARD).uf2

MSX_DIR = msxio
ROM_CFILES = $(addprefix $(MSX_DIR)/src/,main.c)
ROM_AFILES = $(addprefix $(MSX_DIR)/src/,portio.s timeout.s)
ROM_H = $(BUILD_DIR)/rom.h
UNAPI_OUT = $(CURDIR)/build/unapi
UNAPI_SRC = $(wildcard msx/unapi/src/*.[chs] msx/unapi/src/*.inc msx/unapi/src/header/*.asm) \
            msx/unapi/Makefile msx/unapi/pragma.inc

# Boards with /RD decode FFFFh, so they also serve the UNAPI ROM in subslot 1.
ifneq ($(shell grep -c RD_PIN boards/$(BOARD).pio),0)
  UNAPI_ROM ?= $(UNAPI_OUT)/r2r/msxrom/unapi.rom
  UNAPI_H = $(BUILD_DIR)/unapi_rom.h
endif
UF2_BINARY = $(BUILD_DIR)/fujiversal_$(BOARD).uf2

SRC = main.cpp board_defs.h setup_sm.cpp setup_sm.h FujiBusPacket.cpp	\
      FujiBusPacket.h UserROM.h UserROM.cpp Mapper.h Subslot.h CoCoDisk.h	\
      WD1773.h WD1773.cpp SectorLink.h fujiDeviceID.h fujiCommandID.h	\
      diag_uart.cpp diag_uart.h $(ROM_H) $(UNAPI_H)

$(BUILD_DIR)/$(FIRMWARE): $(SRC) $(BUILD_MAKE)
	defoogi make -C $(BUILD_DIR)

$(BUILD_MAKE): CMakeLists.txt boards/$(BOARD).pio
	defoogi cmake -B $(BUILD_DIR) -DBOARD=$(BOARD)

upload: $(BUILD_DIR)/$(FIRMWARE)
	defoogi sudo picotool load -v -x $(UF2_BINARY) -f

picorom_msx picorom_coco msxrp2350 msx_proto_260402 coco_proto_260402:
	$(MAKE) BOARD=$@

all: $(BOARD)

clean:
	rm -rf build

TEST_DIR = build/tests

.PHONY: tests
tests:
	mkdir -p $(TEST_DIR)
	$(CXX) -std=c++20 -Wall -Wextra -O2 -o $(TEST_DIR)/wd1773_test tests/wd1773_test.cpp WD1773.cpp
	$(CXX) -std=c++20 -Wall -Wextra -O2 -o $(TEST_DIR)/sectorlink_test tests/sectorlink_test.cpp
	$(TEST_DIR)/wd1773_test
	$(TEST_DIR)/sectorlink_test

# Don't leave a truncated target behind when a recipe fails
.DELETE_ON_ERROR:

$(ROM_H): $(ROM_IMAGE) | $(BUILD_DIR)
	defoogi xxd -i -n disk_rom $< > $@

$(ROM_IMAGE): $(ROM_CFILES) $(ROM_AFILES)
	defoogi make -C $(MSX_DIR)

$(UNAPI_H): $(UNAPI_ROM) | $(BUILD_DIR)
	defoogi xxd -i -n unapi_rom $< > $@

$(UNAPI_ROM): $(UNAPI_SRC)
	$(MAKE) unapi-rom

# The MSX-UNAPI cartridge (msx/unapi).
unapi-rom:
	AS_DEFAULT=z88dk-z80asm defoogi make -C msx/unapi R2R_DIR=$(UNAPI_OUT)/r2r \
	  BUILD_DIR=$(UNAPI_OUT)/obj CACHE_DIR=$(UNAPI_OUT)/_cache

.PHONY: unapi-rom

$(BUILD_DIR):
	mkdir -p $@
