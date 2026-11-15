# sa3-3ds top-level build.
#
# Two halves, as in pmd-red-3ds:
#   make prelink   (Linux/WSL, where your ROM and an arm-none-eabi GCC are)
#                  builds SAT-R/sa3 with PLATFORM=3ds, game + platform core
#                  partially linked -> build/sa3-game.o
#                  It contains data from your ROM: keep it to yourself.
#                  WIDE=1: the 400x240 picture (fills the top screen 1:1)
#                  instead of the GBA's 240x160.
#   make 3ds       (devkitPro) links the 3DS backend with build/sa3-game.o
#                  -> build/sa3-3ds.3dsx

ROOT     := $(abspath .)
UPSTREAM ?= $(ROOT)/build/upstream
BUILD    := $(ROOT)/build
WIDE     ?= 0
JOBS     ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all prelink 3ds clean
all: 3ds

# ---------------------------------------------------------------------------
# Game half

PRELINK := $(BUILD)/sa3-game.o

# A plain target: sa3's own Makefile knows what to rebuild. Switching WIDE
# needs a clean game build (make clean first): the picture size is compiled in.
prelink:
	@test -f $(UPSTREAM)/baserom_sa3.gba || { echo "Missing $(UPSTREAM)/baserom_sa3.gba (run tools/bootstrap.sh, then copy your ROM there)"; false; }
	@$(MAKE) -C $(UPSTREAM) PLATFORM=3ds WIDE=$(WIDE) PORT_TOOLS=$(ROOT)/tools -j$(JOBS)
	@cp $(UPSTREAM)/sa3-game.o $(PRELINK)
	@echo "  ->   $(PRELINK) (partial; WIDE=$(WIDE))"

clean:
	rm -rf $(BUILD)/3ds $(PRELINK) $(BUILD)/sa3-3ds.*
	-$(MAKE) -C $(UPSTREAM) PLATFORM=3ds tidy

# ---------------------------------------------------------------------------
# 3DS half

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRU      := $(DEVKITPRO)/libctru
TOOLS_3DS := $(DEVKITPRO)/tools/bin
CC_3DS    := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH_3DS  := -marm -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

APP_TITLE  := Sonic Advance 3
APP_DESC   := sa3-3ds port (personal build)
APP_AUTHOR := SAT-R / sa3-3ds

3DS_OBJ := $(BUILD)/3ds
3DS_ELF := $(BUILD)/sa3-3ds.elf

$(3DS_OBJ)/main.o: port/3ds/main.c port/core/port.h
	@mkdir -p $(dir $@)
	@$(CC_3DS) -O2 -g -Wall $(ARCH_3DS) -D__3DS__ -I$(CTRU)/include -c $< -o $@
	@echo "  CC   $<"

$(PRELINK):
	@echo "Missing $(PRELINK). Put sa3-game.o in the build folder (or run make prelink)."
	@false

$(3DS_ELF): $(3DS_OBJ)/main.o $(PRELINK)
	@$(CC_3DS) -specs=3dsx.specs -g $(ARCH_3DS) -Wl,-Map,$(BUILD)/sa3-3ds.map -Wl,--no-warn-rwx-segments \
		-o $@ $(3DS_OBJ)/main.o $(PRELINK) -L$(CTRU)/lib -lctru -lm
	@echo "  LD   $@"

$(BUILD)/sa3-3ds.smdh: port/3ds/icon.png
	@$(TOOLS_3DS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" $< $@

3ds: $(BUILD)/sa3-3ds.3dsx
$(BUILD)/sa3-3ds.3dsx: $(3DS_ELF) $(BUILD)/sa3-3ds.smdh
	@$(TOOLS_3DS)/3dsxtool $< $@ --smdh=$(BUILD)/sa3-3ds.smdh
	@echo "  3DSX $@"
