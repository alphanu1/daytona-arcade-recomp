# enDjinn: the repository's extern/endjinn (platform/dreamcast-native/scripts/fetch_endjinn.sh) unless ENDJINN_ROOT is set.
ENDJINN_ROOT ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../extern/endjinn)
ifeq ($(wildcard $(ENDJINN_ROOT)/base_link.mk),)
$(error no enDjinn at $(ENDJINN_ROOT): run platform/dreamcast-native/scripts/fetch_endjinn.sh or set ENDJINN_ROOT)
endif

.DEFAULT_GOAL := all
include $(ENDJINN_ROOT)/base_link.mk

$(OBJS): Makefile local.cfg.mk
# Presentation only: global fast-math aborts the live race at tick 563.
# Preserve finite-input guards at asset/geometry boundaries.
$(filter $(ENJ_BUILDDIR)/code/render/%.o $(ENJ_BUILDDIR)/code/geometry.o,$(OBJS)): ENJ_CFLAGS += -ffast-math -fno-finite-math-only
# Retain compiler assembly: SH4 objdump can decode jump-table data as FPSCR ops.
$(ENJ_BUILDDIR)/code/geometry.o: ENJ_CFLAGS += -save-temps=obj
HANDOPT_ASM_OBJS := $(ENJ_BUILDDIR)/code/geometry.o
# Opt-in inspection build for hand tuning.  model_submission.h is inlined into
# model_renderer.c, so this is where its generated SH4 code lives.
ifdef NATIVE_HANDOPT
HANDOPT_ASM_OBJS += $(ENJ_BUILDDIR)/code/practice.o \
                    $(ENJ_BUILDDIR)/code/render/models/model_renderer.o \
                    $(ENJ_BUILDDIR)/code/render/materials/materials.o
$(HANDOPT_ASM_OBJS): ENJ_CFLAGS += -save-temps=obj -fverbose-asm
endif
# A generated .s otherwise wins over enDjinn's generic %.c -> %.o rule, so a
# changed header can silently assemble stale hand-optimization output.
$(HANDOPT_ASM_OBJS): $(ENJ_BUILDDIR)/%.o: %.c $(ENJ_MAKEFILE) $(ENJ_ASSETS)
	@mkdir -p $(dir $@)
	@echo "Building $@ from $<..."
	$(ENJ_CC) $(ENJ_INCLUDES) $(ENJ_CFLAGS) $(DEFINES) $(ENJ_DEPFLAGS) -c $< -o $@
ifdef NATIVE_INTENT_DATA_DIR
$(ENJ_BUILDDIR)/code/main.o: $(abspath $(NATIVE_INTENT_DATA_DIR))/race_intent_data.h
endif
# NATIVE_GAMEPLAY_OPT (local.cfg.mk): the gameplay units' optimisation. Appended to DEFINES, which follows
# ENJ_CFLAGS and its -O3 on the compile line, so the variant's -O level is the one that counts.
NATIVE_GAMEPLAY_OBJS := $(addprefix $(ENJ_BUILDDIR)/code/,practice.o drive.o contact.o road.o path.o visibility.o gameplay/results.o)
ifdef NATIVE_GAMEPLAY_OPT_FLAGS
$(filter $(NATIVE_GAMEPLAY_OBJS),$(OBJS)): DEFINES += $(NATIVE_GAMEPLAY_OPT_FLAGS)
endif
# The sound effects' code outside the LTO unit (include/audio/race_sound_hooks.h): the game's code and its
# placement stay those of a build without it.
$(addprefix $(ENJ_BUILDDIR)/code/audio/,sfx.o race_sound.o idle_wait.o): DEFINES += -fno-lto
# The memory dispatchers (code/nm_mem.c, include/nm_mem.h): SH4ZAM's inline forms without their asserts (the
# dispatcher establishes the alignment and size each form requires before calling it).
$(ENJ_BUILDDIR)/code/nm_mem.o: DEFINES += -DNDEBUG
# The scene loader (scene_loader.c: the attract's load in the title's frames) too.
$(ENJ_BUILDDIR)/code/scene_loader.o: DEFINES += -fno-lto
# The props (props.c, props_scene.c: cones) and Expert's world actors (expert_actors.c) too.
$(addprefix $(ENJ_BUILDDIR)/code/,props.o props_scene.o expert_actors.o): DEFINES += -fno-lto
# The game start and the select screens (gameplay/select_mode.h, render/ui/select_render.h): outside the LTO unit.
$(addprefix $(ENJ_BUILDDIR)/code/,gameplay/select_mode.o render/ui/select_render.o): DEFINES += -fno-lto
# The attract's 2D layer (render/ui/attract_2d_render.h): outside the LTO unit too.
$(ENJ_BUILDDIR)/code/render/ui/attract_2d_render.o: DEFINES += -fno-lto
# Settings, records and the VMU (include/game_save.h): outside the LTO unit too, never in the race's hot path.
$(filter $(ENJ_BUILDDIR)/code/save/%.o $(ENJ_BUILDDIR)/code/gameplay/settings_mode.o,$(OBJS)): DEFINES += -fno-lto
# race_sound.c computes from gameplay state (distances, levels) as tests/trace_race_native.c checks it.
$(ENJ_BUILDDIR)/code/audio/race_sound.o: DEFINES += -fno-fast-math -ffp-contract=off
# AICAflow's SH4 library is built in its own checkout (local.cfg.mk, AICAFLOW_ROOT); the firmware is embedded.
# Its own Makefile always runs (FORCE): it tracks the library's sources and replaces the archive only when it
# differs, so a changed AICAflow pin (or ABI) rebuilds it, and an unchanged one keeps its date and relinks nothing.
ifndef NATIVE_TRANSFER_BENCH
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: $(AICAFLOW_LIB)
$(AICAFLOW_LIB): FORCE_AICAFLOW
	$(MAKE) -C $(@D)
.PHONY: FORCE_AICAFLOW
FORCE_AICAFLOW:
$(ENJ_BUILDDIR)/code/audio/sound.o: $(AICAFLOW_ROOT)/firmware/aicaflow.drv include/audio/music_map.h include/audio/sfx.h
$(ENJ_BUILDDIR)/code/audio/sound.o: DEFINES += $(NATIVE_SOUND_DEFINES)
endif
# NATIVE_HOT_ORDER (local.cfg.mk): relink when the ordering file changes.
ifeq ($(NATIVE_HOT_ORDER),1)
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: $(NATIVE_HOT_ORDER_FILE)
endif
# NATIVE_LAYOUT (local.cfg.mk, tools/hot_layout.py): the ordering file from config/hot_layout.tsv, and after the link
# a loud warning for every hot entry that is not where the config puts it (make check fails on it).
ifeq ($(NATIVE_LAYOUT),1)
$(NATIVE_LAYOUT_LD): config/hot_layout.tsv tools/hot_layout.py
	@mkdir -p $(dir $@)
	python3 tools/hot_layout.py ld $< -o $@
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: $(NATIVE_LAYOUT_LD)
.PHONY: layout-check
all: layout-check
layout-check: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf
	@python3 tools/hot_layout.py check config/hot_layout.tsv $< --warn
endif

# The disc image (recipes/README.md, "Disc image"): enDjinn's image target, mkdcdisc with ENJ_ROMDIR at the root.
.PHONY: cdi
cdi: $(ENJ_BINDIR)/$(ENJ_BASENAME).cdi $(if $(filter 1,$(NATIVE_LAYOUT)),layout-check)
ifneq ($(filter cdi %.cdi,$(MAKECMDGOALS)),)
ifndef NATIVE_DISC
$(error make cdi needs NATIVE_DISC=1: a dcload build reads its assets from /pc)
endif
endif

comma := ,
# The build's options that change its heap (tools/heap_budget.py).
HEAP_BUDGET_FLAGS = $(if $(NATIVE_SAMPLE_PROFILE),--profile) $(if $(filter 0,$(NATIVE_SQ_STREAM)),--no-streams) \
  $(if $(filter 0,$(NATIVE_SQ_CAR_STREAM)),--no-car-streams) --world-stream-courses $(or $(NATIVE_WORLD_STREAM_COURSES),5) \
  --stream-pool $(or $(NATIVE_WORLD_STREAM_POOL),458752) \
  $(if $(NATIVE_DIAG_LATE_CAPTURE),--late-capture $(NM_LATE_CAPTURE_POOL_BYTES)) $(if $(NATIVE_FRAME_COMPARE),--frame-compare)
.PHONY: check
check: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf
ifdef NATIVE_TRANSFER_BENCH
	sh tests/check_build.sh $(ENJ_BUILDDIR)/tests/transfer/main.o $<
else
	sh tests/check_build.sh $(ENJ_BUILDDIR)/code/main.o $<
endif
ifeq ($(NATIVE_LAYOUT),1)
	python3 tools/hot_layout.py check config/hot_layout.tsv $<
endif
	python3 tests/test_practice_render_split.py
	python3 tests/test_matrix_load.py
	python3 tools/inspect_quad_strips.py --self-test
	python3 tests/test_sq_quads.py
	python3 tests/test_render_profile.py
	python3 tests/test_inspect_pause.py
	python3 tests/test_inspect_camera.py
	python3 tests/test_title_mode.py
	python3 tests/test_select_mode.py
	python3 tests/test_settings_mode.py
	python3 tests/test_track_surface.py
	python3 tests/test_practice_assets_reload.py
	python3 tests/test_drop_log.py
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -DNM_RACE_CONTROLS_CHECK tests/race_controls_wrap.c -o $(ENJ_BUILDDIR)/race-input-check
	$(ENJ_BUILDDIR)/race-input-check
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/test_podium.c -o $(ENJ_BUILDDIR)/podium-check
	$(ENJ_BUILDDIR)/podium-check
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
	  -DNM_STEER_CURVE=$(NATIVE_STEER_CURVE) -DNM_STEER_DEADZONE=$(NATIVE_STEER_DEADZONE) tests/test_steer_curve.c -lm \
	  -o $(ENJ_BUILDDIR)/steer-curve-check
	$(ENJ_BUILDDIR)/steer-curve-check
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/test_race_sound.c -lm -o $(ENJ_BUILDDIR)/race-sound-check
	$(ENJ_BUILDDIR)/race-sound-check
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude tests/test_game_save.c \
	  code/save/save_data.c code/save/save_store.c code/save/game_save.c -o $(ENJ_BUILDDIR)/game-save-check
	$(ENJ_BUILDDIR)/game-save-check
	cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude -Itests/kos_shim \
	  -pthread tests/test_scene_loader.c -o $(ENJ_BUILDDIR)/scene-loader-check
	$(ENJ_BUILDDIR)/scene-loader-check
ifndef NATIVE_TRANSFER_BENCH
	cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
	  -I$(AICAFLOW_ROOT)/driver/include -I$(AICAFLOW_ROOT)/driver/format/include -I$(AICAFLOW_ROOT)/driver/sh4/include \
	  -pthread tests/test_sound.c -o $(ENJ_BUILDDIR)/sound-check
	$(ENJ_BUILDDIR)/sound-check
	cc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iinclude \
	  -I$(AICAFLOW_ROOT)/driver/include -I$(AICAFLOW_ROOT)/driver/format/include -I$(AICAFLOW_ROOT)/driver/sh4/include \
	  tests/test_sfx.c -o $(ENJ_BUILDDIR)/sfx-check
	$(ENJ_BUILDDIR)/sfx-check $(if $(wildcard $(ENJ_ROMDIR)/sfx/race.dsx),$(abspath $(ENJ_ROMDIR))/)
	cc -std=gnu11 -O1 -Wall -Wextra -Werror -DNM_SFX_VOICE_STREAM=1 -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -Iinclude -I$(AICAFLOW_ROOT)/driver/include -I$(AICAFLOW_ROOT)/driver/format/include \
	  -I$(AICAFLOW_ROOT)/driver/sh4/include tests/test_sfx.c -o $(ENJ_BUILDDIR)/sfx-voices-check
	$(ENJ_BUILDDIR)/sfx-voices-check
endif
ifndef NATIVE_TRANSFER_BENCH
	cc -std=c11 -O1 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off -fsanitize=address,undefined -Iinclude -I$(abspath $(RACE_ASSET_DIR)) -I$(abspath $(NATIVE_PODIUM_DATA_DIR)) tests/test_podium_scene.c code/drive.c code/contact.c code/road.c code/path.c -lm -o $(ENJ_BUILDDIR)/podium-scene-check
	$(ENJ_BUILDDIR)/podium-scene-check "$(ENJ_ROMDIR)"
endif
# The props (cones) against the original game's runs (tests/test_props.c): NATIVE_PROPS_RUNS=<dir with A..G.bin of
# course 0's cones> and/or NATIVE_PROPS_COURSE_RUNS=<dir with *.bin of m2segment --trace-race R --trace-props RUN.bin
# on any course: each scene's course comes with the run>, and a race-start header with the prop data.
ifneq ($(NATIVE_PROPS_RUNS)$(NATIVE_PROPS_COURSE_RUNS),)
	cc -std=c11 -O1 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off -fsanitize=address,undefined -Iinclude \
	  -I$(abspath $(NATIVE_RACE_START_DATA_DIR)) -I$(abspath $(RACE_ASSET_DIR)) tests/test_props.c code/props.c code/drive.c code/contact.c -lm \
	  -o $(ENJ_BUILDDIR)/props-check
	$(ENJ_BUILDDIR)/props-check "$(ENJ_ROMDIR)" $(if $(NATIVE_PROPS_RUNS),$(addprefix $(NATIVE_PROPS_RUNS)/,A.bin B.bin C.bin D.bin E.bin G.bin)) \
	  $(if $(NATIVE_PROPS_COURSE_RUNS),$(sort $(wildcard $(NATIVE_PROPS_COURSE_RUNS)/*.bin)))
endif
# The lumas of Expert's placed actors (include/render/models/clock_light.h) against the per-face reference, on the
# asset set's world pack and actor file (tests/test_clock_light.c; the recipe runs it on every course's).
ifneq ($(wildcard $(ENJ_ROMDIR)/course1-actors.dna),)
	cc -std=c11 -O2 -Wall -Wextra -Werror -fno-fast-math -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -Iinclude tests/test_clock_light.c code/meshpack.c -lm -o $(ENJ_BUILDDIR)/clock-light-check
	$(ENJ_BUILDDIR)/clock-light-check "$(ENJ_ROMDIR)/course1-world.dnm" "$(ENJ_ROMDIR)/course1-actors.dna"
endif
ifndef NATIVE_TRANSFER_BENCH
	python3 tests/test_results_phase.py "$(NATIVE_RESULT_DATA_DIR)"
# The heap this ELF needs for these assets (include/heap.h, tools/heap_budget.py "Budgets"): the first load, then a
# whole play session through every course the asset set holds (boot into the attract; select, race, finish and podium
# music, name entry, attract again per course; the course is a run-time choice). Fails when less than 256 KiB would
# never be taken or less than 512 KiB stays free while racing (plus the frame comparison's capture where built).
	python3 tests/test_heap_budget.py
	python3 tools/heap_budget.py "$(ENJ_ROMDIR)" --stage $(or $(NATIVE_SCENERY_STAGE),6) --elf $< $(HEAP_BUDGET_FLAGS)
	python3 tools/heap_budget.py "$(ENJ_ROMDIR)" --stage $(or $(NATIVE_SCENERY_STAGE),6) --elf $< $(HEAP_BUDGET_FLAGS) \
	  --session 0$(if $(wildcard $(ENJ_ROMDIR)/course2-world.dnm),$(comma)2)$(if $(wildcard $(ENJ_ROMDIR)/course1-world.dnm),$(comma)1)$(comma)0
	python3 tests/test_late_capture.py
endif

.PHONY: check-queue
check-queue:
	python3 tests/test_endjinn_deferred_queue.py "$(ENDJINN_ROOT)"
