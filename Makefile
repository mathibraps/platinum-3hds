# Nintendo 3DS build (devkitPro devkitARM + libctru).
#
#   python3 tools/extract_rom.py <your Platinum US .nds> romfs
#   make            # pokeplatinum.3dsx, which includes romfs/ (the game's data)
#   make meta       # the HOME menu icon, banner and jingle (from art/ or the game)
#   make cia        # Platinum3HDS.cia, installable (needs makerom, bannertool)

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro with the 3ds-dev group: https://devkitpro.org/wiki/Getting_Started)
endif

BUILD := build-3ds
include sources.mk
.DEFAULT_GOAL := all

TARGET := pokeplatinum
APP_TITLE := Platinum 3HDS
APP_DESCRIPTION := Pokemon Platinum, native 3DS source port
APP_AUTHOR := pokeplatinum port
META := $(BUILD)/meta
# The icon made by `make meta` (from art/cover.png, or the title screen's
# Giratina), else libctru's.
APP_ICON := $(if $(wildcard $(META)/icon.png),$(META)/icon.png,$(DEVKITPRO)/libctru/default_icon.png)

# The installable build: same program, the game data inside it.
CIA := Platinum3HDS.cia
APP_PRODUCT_CODE := CTR-H-PL3D
APP_UNIQUE_ID := 0xB3D45
MAKEROM ?= ../tools-host/makerom/makerom
BANNERTOOL ?= ../tools-host/bannertool/bannertool

PREFIX := $(DEVKITPRO)/devkitARM/bin/arm-none-eabi-
CC := $(PREFIX)gcc
NM := $(PREFIX)nm
LIBCTRU := $(DEVKITPRO)/libctru

ARCH := -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=hard -mtp=soft
CFLAGS := $(COMMON_CFLAGS) $(EXTRA_CFLAGS) -O2 -g -ffunction-sections $(ARCH) -D__3DS__ $(INCLUDES) -I$(LIBCTRU)/include $(PREFIX_HEADER) -MMD -MP
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--no-wchar-size-warning -Wl,--no-enum-size-warning -Wl,-Map,$(BUILD)/$(TARGET).map -L$(LIBCTRU)/lib
LIBS := -lcitro2d -lcitro3d -lctru -lm

FRONTEND_3DS := frontend/main_3ds.c frontend/gpu2d_3ds.c frontend/gpu3d_3ds.c frontend/input_script.c
OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(PORT_SRCS) $(FRONTEND_3DS)) $(BUILD)/obj/gpu2d_shader.o $(BUILD)/obj/gpu3d_shader.o \
	$(patsubst %.c,$(BUILD)/obj/game/%.o,$(GAME_SRCS)) \
	$(patsubst %.c,$(BUILD)/obj/game/lib/%.o,$(LIB_SRCS)) \
	$(patsubst %.c,$(BUILD)/obj/nns/%.o,$(NNS_SRCS)) \
	$(patsubst %.c,$(BUILD)/obj/sdk/%.o,$(SDK_SRCS)) $(SND7_OBJS)

all: $(TARGET).3dsx

$(TARGET).3dsx: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).smdh
	@test -f romfs/demo/title/titledemo.narc || (echo "romfs/ is empty: run tools/extract_rom.py first" && false)
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@ --smdh=$(BUILD)/$(TARGET).smdh --romfs=romfs

$(BUILD)/$(TARGET).smdh: $(APP_ICON) Makefile
	@mkdir -p $(BUILD)
	$(DEVKITPRO)/tools/bin/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $< $@

$(META)/.stamp: tools/make_app_meta.sh tools/make_app_art.py tools/make_app_art_from.py tools/cut_jingle.py $(wildcard art/*.png)
	APP_NAME="$(APP_TITLE)" tools/make_app_meta.sh
	@touch $@

meta: $(META)/.stamp

$(BUILD)/banner.bnr: $(META)/.stamp
	$(BANNERTOOL) makebanner -i $(META)/banner.png -a $(META)/jingle.wav -o $@

$(BUILD)/cia.smdh: $(META)/.stamp Makefile
	$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(META)/icon.png -o $@

$(CIA): $(BUILD)/$(TARGET).elf $(BUILD)/banner.bnr $(BUILD)/cia.smdh meta/app.rsf
	@test -f romfs/demo/title/titledemo.narc || (echo "romfs/ is empty: run tools/extract_rom.py first" && false)
	$(MAKEROM) -f cia -o $@ -elf $< -rsf meta/app.rsf -icon $(BUILD)/cia.smdh -banner $(BUILD)/banner.bnr -exefslogo -target t \
		"-DAPP_TITLE=$(APP_TITLE)" -DAPP_PRODUCT_CODE=$(APP_PRODUCT_CODE) -DAPP_UNIQUE_ID=$(APP_UNIQUE_ID) -DAPP_ROMFS=romfs

cia: $(CIA)

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/obj/game/%.o: $(GAMESRC_DIR)/%.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GAME_CFLAGS) -c -o $@ $<

$(BUILD)/obj/nns/%.o: $(NNSSRC_DIR)/%.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GAME_CFLAGS) $(NNS_CFLAGS) $(NNS_PRIVATE_INCLUDES) -c -o $@ $<

$(BUILD)/obj/sdk/%.o: $(SDKSRC_DIR)/%.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(GAME_CFLAGS) $(SDK_PRIVATE_INCLUDES) -c -o $@ $<

# The front end talks to libctru, which is built with GCC's default (short) enums.
$(BUILD)/obj/frontend/%_3ds.o: frontend/%_3ds.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iplatform/src -I$(BUILD) -fshort-enums -c -o $@ $<

# Every file of the extracted ROM, which the build checks it can find at start.
$(BUILD)/obj/frontend/main_3ds.o: $(BUILD)/romfs_files.h
$(BUILD)/romfs_files.h: FORCE
	@python3 -I tools/gen_romfs_list.py romfs $@
FORCE:

# The GPU vertex shaders (3D engine, 2D composition), assembled with picasso
# and embedded as data (gGpu3DShader, gGpu2DShader).
$(BUILD)/%.shbin: frontend/%.v.pica
	@mkdir -p $(dir $@)
	$(DEVKITPRO)/tools/bin/picasso -o $@ $<

$(BUILD)/gpu3d_shader.c: $(BUILD)/gpu3d.shbin tools/bin2c.py
	python3 -I tools/bin2c.py $< $@ gGpu3DShader

$(BUILD)/gpu2d_shader.c: $(BUILD)/gpu2d.shbin tools/bin2c.py
	python3 -I tools/bin2c.py $< $@ gGpu2DShader

$(BUILD)/obj/%_shader.o: $(BUILD)/%_shader.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -O2 -c -o $@ $<

$(BUILD)/obj/%.o: %.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies written by -MMD.
-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD) $(TARGET).3dsx $(CIA)

.PHONY: all clean meta cia
.SECONDARY:
