# OutRunners on a SEGA NAOMI (ports/naomi/README.md). With KallistiOS built for KOS_SUBARCH=naomi:
#
#   source /opt/toolchains/dc/kos-naomi/environ.sh
#   make -C ports/naomi
#
# Output: orunners_naomi.elf here. build_naomi.sh makes the netboot image from it and the player's assets.

ROOT := ../..
TARGET = orunners_naomi.elf

RUNTIME = machine io sound ym3438 memory_census tasks video workers multipcm sound_chips sound_cpu \
	game assets assets_media assets_midi assets_pack json png inflate
GAME_C := $(shell find $(ROOT)/src -name '*.c')
OBJS = main.o naomi_log.o naomi_dimmfs.o naomi_dimmcomms.o \
	$(addprefix runtime/,$(addsuffix .o,$(RUNTIME))) \
	$(patsubst $(ROOT)/src/%.c,game/%.o,$(GAME_C))

# the game's C reads the work RAM through typed lvalues (game/support.h): no strict aliasing anywhere.
# ORUNNERS_PACKED_PENS: the tiles two pens to a byte (naomi_assets.py). Two tasks of about 3.5 KB of stack each
# on Linux: 128 KB each is plenty (runtime/src/tasks.c)
OUR_CFLAGS = -O2 -fno-strict-aliasing -Wno-stringop-overflow -DORUNNERS_PACKED_PENS -DTASK_STACK_SIZE='(128u << 10)' \
	-I$(ROOT)/runtime/include -I$(ROOT)/src -I.

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

ifneq ($(KOS_SUBARCH),naomi)
$(error KOS_SUBARCH is $(KOS_SUBARCH): source the NAOMI KallistiOS tree's environ.sh)
endif

%.o: %.c
	kos-cc $(OUR_CFLAGS) -c $< -o $@

runtime/%.o: $(ROOT)/runtime/src/%.c
	@mkdir -p $(dir $@)
	kos-cc $(OUR_CFLAGS) -c $< -o $@

game/%.o: $(ROOT)/src/%.c
	@mkdir -p $(dir $@)
	kos-cc $(OUR_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm

clean:
	rm -rf $(TARGET) *.o runtime game

.PHONY: all clean
