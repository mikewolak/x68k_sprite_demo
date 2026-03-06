################################################################################
# X68000 Full-Disk Track Loader - Makefile
#
# Builds a bootable XDF floppy image using a full-disk track loader.
# The loader reads 8KB per half-track (one side of each cylinder) and
# alternates head 0/1 across all 77 cylinders, giving ~1.2MB payload capacity.
#
# Pure C99 port: only start.s and memsize.s remain as assembly.
################################################################################

PROJECT = sprite_plex

SRC_DIR  = src
INC_DIR  = include
DATA_DIR = data
OBJ_DIR  = obj
BUILD_DIR = build
TOOLS_DIR = tools

ELF = $(BUILD_DIR)/$(PROJECT).elf
BIN = $(BUILD_DIR)/$(PROJECT).bin
XDF = $(BUILD_DIR)/$(PROJECT).xdf

PREFIX = m68k-elf
CC = $(PREFIX)-gcc
AS = $(PREFIX)-as
LD = $(PREFIX)-ld
OC = $(PREFIX)-objcopy
NM = $(PREFIX)-nm

# Demo resolution: 256x256 (default) or 512x512
# Override at build time:  make DEMO_RES=512x512
DEMO_RES ?= 256x256

ifeq ($(DEMO_RES),256x256)
  _RES_CDEF  = -DDEMO_RES=256
  _RES_ADEF  = --defsym DEMO_RES=256
else ifeq ($(DEMO_RES),512x512)
  _RES_CDEF  = -DDEMO_RES=512
  _RES_ADEF  = --defsym DEMO_RES=512
else
  $(error Unknown DEMO_RES=$(DEMO_RES). Valid values: 256x256 512x512)
endif

AFLAGS = -march=68000 -mcpu=68000 -M -I$(INC_DIR) $(_RES_ADEF)

CFLAGS  = -m68000 -march=68000 -mcpu=68000 -mtune=68000
CFLAGS += -std=c99
CFLAGS += -MMD
CFLAGS += -Wall -Wextra -Werror -Wno-int-to-pointer-cast -Wno-shift-negative-value
CFLAGS += -Os -fomit-frame-pointer -fno-ident -freorder-blocks -fno-strict-aliasing -ffunction-sections
CFLAGS += -I$(INC_DIR) $(_RES_CDEF)

LDFLAGS  = -L$(shell $(CC) -print-libgcc-file-name | xargs dirname) -lgcc
LDFLAGS += --gc-sections --print-gc-sections -N -Ttext 0x2000

export LANG=C
SHELL = /bin/bash -o pipefail
VSCFILTER = 2>&1 | sed "s/:\([0-9][0-9]*\)\(:[0-9][0-9]*\)*: \([Ff][a]tal [Ee][r]ror\|[Ee][r]ror\|[Ww][a]rning\): /(\1): \l\3: /;" || true
VSLFILTER = 2>&1 | sed "s/:\(.*\): undefined reference/: error: \1: undefined reference/;" || true

C_SOURCES    = $(wildcard $(SRC_DIR)/*.c)
ASM_SOURCES  = $(wildcard $(SRC_DIR)/*.s $(SRC_DIR)/*.asm)
DATA_SOURCES = $(wildcard $(DATA_DIR)/*.s)

C_OBJS    = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(C_SOURCES))
ASM_OBJS  = $(patsubst $(SRC_DIR)/%.s,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(ASM_OBJS))
DATA_OBJS = $(patsubst $(DATA_DIR)/%.s,$(OBJ_DIR)/%.o,$(DATA_SOURCES))

ALL_OBJS  = $(ASM_OBJS) $(C_OBJS) $(DATA_OBJS)

START_OBJ  = $(OBJ_DIR)/start.o
OTHER_OBJS = $(filter-out $(START_OBJ),$(ALL_OBJS))

################################################################################

.PHONY: all clean setup run sprites

all: setup sprites $(XDF)

run: all
	mame x68000 -flop1 $(XDF) -rompath roms -skip_gameinfo -window -nomaximize -resolution 1024x768 -bios ipl10

# Generate sprite data from PNG
sprites: $(DATA_DIR)/sprite_patterns.s $(DATA_DIR)/sprite_palette.s

$(DATA_DIR)/sprite_patterns.s $(DATA_DIR)/sprite_palette.s: $(DATA_DIR)/spritesheet_16x16.png $(TOOLS_DIR)/convert_sprites.py
	@echo "Converting sprite sheet..."
	@python3 $(TOOLS_DIR)/convert_sprites.py $(DATA_DIR)/spritesheet_16x16.png $(DATA_DIR)/sprite_patterns.s $(DATA_DIR)/sprite_palette.s

setup:
	@mkdir -p $(OBJ_DIR) $(BUILD_DIR) $(TOOLS_DIR)
	@touch $(OBJ_DIR)/_.d

clean:
	rm -rf $(OBJ_DIR)/*.o $(OBJ_DIR)/*.d $(BUILD_DIR)/*
	mkdir -p $(OBJ_DIR) $(BUILD_DIR)
	echo ' ' > $(OBJ_DIR)/_.d

################################################################################

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.s
	@echo "AS  $<"
	@$(AS) $(AFLAGS) $< -o $@ $(VSCFILTER)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.asm
	@echo "AS  $<"
	@$(AS) $(AFLAGS) $< -o $@ $(VSCFILTER)

$(OBJ_DIR)/%.o: $(DATA_DIR)/%.s
	@echo "AS  $<"
	@$(AS) $(AFLAGS) $< -o $@ $(VSCFILTER)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@ $(VSCFILTER)

$(ELF): $(START_OBJ) $(OTHER_OBJS)
	@echo "LD  $@"
	@$(LD) $(START_OBJ) $(OTHER_OBJS) $(LDFLAGS) -o $@ $(VSLFILTER)

$(BIN): $(ELF)
	@echo "OC  $@"
	@$(OC) -O binary $< $@
	@BSS_START=$$($(NM) $< | grep __bss_start | awk '{print "0x" $$1}') && \
	START_ADDR=$$($(NM) $< | grep " _start$$" | awk '{print "0x" $$1}') && \
	SIZE=$$(($$BSS_START - $$START_ADDR)) && \
	ROUNDED_SIZE=$$(( ($$SIZE + 0x1FFF) & ~0x1FFF )) && \
	CURRENT_SIZE=$$(wc -c < $@ | tr -d ' ') && \
	PAD_SIZE=$$(($$ROUNDED_SIZE - $$CURRENT_SIZE)) && \
	if [ $$PAD_SIZE -gt 0 ]; then \
		dd if=/dev/zero bs=1 count=$$PAD_SIZE >> $@ 2>/dev/null; \
		echo "Padded binary from $$CURRENT_SIZE to $$ROUNDED_SIZE bytes (+$$PAD_SIZE bytes)"; \
	fi

$(TOOLS_DIR)/makexdf: $(TOOLS_DIR)/makexdf.c
	@echo "Building makexdf tool..."
	@gcc -O2 -Wall $< -o $@

$(XDF): $(BIN) $(TOOLS_DIR)/makexdf
	@echo "Creating XDF disk image..."
	@$(TOOLS_DIR)/makexdf $< $@

################################################################################

-include $(OBJ_DIR)/*.d

################################################################################
