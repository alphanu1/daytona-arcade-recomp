ENJ_BASENAME := daytona-native
ENJ_CFLAGS += -I$(abspath include)
# Native title data is a separate, bounded PAL4 pack. It is embedded so the
# title can replace the released race assets without a filesystem dependency.
TITLE_SCENE_DIR ?= ../../build/dc-native-timing.Xsmcer/prepared
ifeq ($(wildcard $(TITLE_SCENE_DIR)/native_scene.inc),)
$(error TITLE_SCENE_DIR must contain prepared native_scene.inc)
endif
ENJ_CFLAGS += -I$(abspath $(TITLE_SCENE_DIR))
ifdef AUTO_START
ifeq ($(AUTO_START),attract)
ENJ_CFLAGS += -DDAYTONA_AUTOSTART=2
else
ENJ_CFLAGS += -DDAYTONA_AUTOSTART
endif
endif
ifdef RACE_ASSET_DIR
ENJ_CFLAGS += -DDAYTONA_RACE_TICK_LIMIT=$(or $(RACE_TICK_LIMIT),0) -I$(abspath $(RACE_ASSET_DIR))
ifdef NATIVE_DRIVING_TEST
$(error RACE_ASSET_DIR uses the ordinary race loop, not the player input script)
endif
endif
ifdef NATIVE_INDEXED_PACK_BENCH
ifndef NATIVE_TRANSFER_BENCH
$(error NATIVE_INDEXED_PACK_BENCH requires NATIVE_TRANSFER_BENCH=1)
endif
ENJ_CFLAGS += -DNATIVE_INDEXED_PACK_BENCH
endif
ifdef NATIVE_TRANSFER_BENCH
ENJ_CODEDIR := ./tests/transfer
OBJS += $(ENJ_BUILDDIR)/code/material_pack.o
ifneq ($(strip $(RACE_ASSET_DIR)$(NATIVE_DISC)),)
$(error NATIVE_TRANSFER_BENCH requires the ROM-free network foundation)
endif
ENJ_CFLAGS += -DNATIVE_TRANSFER_BENCH
endif
# The game is one fixed Dreamcast product. Only the explicit ROM-free transfer
# diagnostic omits gameplay; the former driving/models/materials/render ladder
# selected retired bring-up programs and no longer selects product code.
ifndef NATIVE_TRANSFER_BENCH
ENJ_BASENAME := daytona-native-driving-3d-textured-world
# 1: bilinear on opaque model faces; with NATIVE_FILTER_ALL=1 (the default,
# below) on every 3D texture. 0: unfiltered, point sampling everywhere. Mipmapped
# textures keep their mip chain in both: the PVR core selects the level from
# its D value in every filter mode (KOS dc/pvr/pvr_header.h: mip_bias, bits 11-8,
# and filter_mode, bits 14-13, are separate TSP word fields; only the two
# PVR_FILTER_TRILINEAR passes blend levels, so only they need a mipmapped texture).
NATIVE_BILINEAR ?= 1
# Road surface class (asphalt, kerbs, lines): 0 point sampled (default), 1
# bilinear like the other opaque faces, as the arcade does. Needs NATIVE_BILINEAR=1.
# NATIVE_FILTER_ALL=1 filters the road whatever this says.
NATIVE_ROAD_FILTER ?= 0
# Defaults of 14 October 2026 (project decisions; the configuration before them is NATIVE_SCENERY_STAGE=3
# NATIVE_FILTER_ALL=0 NATIVE_PAL16=8888 NATIVE_MIP_CLASSES=0): scenery stage 6, every 3D texture bilinear with the
# punch-through alpha reference at 0x80 and the catch-fence stipple point sampled, ARGB1555 palette RAM, mip D adjust
# per class. Mip chains are a property of the asset set (ENJ_ROMDIR): build/dc-native-mipmaps.ds44Xc/assets-mip-select
# (chains on trees, signs, objects and cutouts; none on the road-surface class or the grandstands) is the recommended
# set with mipmaps, build/dc-native-strips2.31FI1U/assets-strips the set without.
NATIVE_SCENERY_STAGE ?= 6
ifeq ($(NATIVE_BILINEAR),1)
NATIVE_FILTER_ALL ?= 1
endif
NATIVE_PAL16 ?= 1555
NATIVE_MIP_CLASSES ?= 1
endif
# Scenery stage 0..6 (include/render/scene/track_surface.h); default 6: everything, the world objects included
# (3 was the default until 14 October 2026: road, walls, stands, ground). The ELF name carries -scenery<n>.
ifdef NATIVE_SCENERY_STAGE
ENJ_BASENAME := $(ENJ_BASENAME)-scenery$(NATIVE_SCENERY_STAGE)
ENJ_CFLAGS += -DNATIVE_SCENERY_STAGE=$(NATIVE_SCENERY_STAGE)
endif
ifeq ($(UNTEXTURED_BENCH),1)
ENJ_BASENAME := $(ENJ_BASENAME)-untextured-bench
ENJ_CFLAGS += -DDAYTONA_UNTEXTURED_BENCH
endif
ifdef NATIVE_PROFILE
ENJ_BASENAME := $(ENJ_BASENAME)-profile
ENJ_CFLAGS += -DNATIVE_PROFILE
endif
ifdef NATIVE_PROFILE_VERIFY
ifndef NATIVE_PROFILE
$(error NATIVE_PROFILE_VERIFY requires NATIVE_PROFILE=1)
endif
# Expensive byte-oracles are separate from phase timing.
ENJ_CFLAGS += -DNATIVE_PROFILE_VERIFY
endif
ifdef NATIVE_CAPTURE
ENJ_BASENAME := $(ENJ_BASENAME)-capture
ENJ_CFLAGS += -DNATIVE_CAPTURE
endif
ifdef NATIVE_ROAD_CHECK
ENJ_BASENAME := $(ENJ_BASENAME)-road-check
ENJ_CFLAGS += -DNATIVE_ROAD_CHECK
endif
ifdef NATIVE_DRIVE_CHECK
ENJ_BASENAME := $(ENJ_BASENAME)-drive-check
ENJ_CFLAGS += -DNATIVE_DRIVE_CHECK
endif
ifdef NATIVE_CONTACT_CHECK
ENJ_BASENAME := $(ENJ_BASENAME)-contact-check
ENJ_CFLAGS += -DNATIVE_CONTACT_CHECK
endif
ifdef NATIVE_PATH_CHECK
ENJ_BASENAME := $(ENJ_BASENAME)-path-check
ENJ_CFLAGS += -DNATIVE_PATH_CHECK
endif
ifdef NATIVE_WORLD_CACHE_BYTES
ifndef NATIVE_DRIVING_TEST
$(error Cache pressure override is restricted to the finite hardware test)
endif
ENJ_CFLAGS += -DNM_WORLD_MATERIAL_BYTES=$(NATIVE_WORLD_CACHE_BYTES)
endif
ifdef NATIVE_DRIVING_TEST
ifdef NATIVE_DISC
$(error NATIVE_DRIVING_TEST is a finite network diagnostic, not a user CDI)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-test
ENJ_CFLAGS += -DNATIVE_DRIVING_TEST
endif
ifeq ($(NATIVE_ROAD_FILTER),1)
ifneq ($(NATIVE_BILINEAR),1)
$(error NATIVE_ROAD_FILTER=1 requires NATIVE_BILINEAR=1)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-roadfilter
ENJ_CFLAGS += -DNM_ROAD_FILTER=1
else ifneq ($(NATIVE_ROAD_FILTER),0)
ifndef NATIVE_TRANSFER_BENCH
$(error NATIVE_ROAD_FILTER must be 0 or 1)
endif
endif
# Prebuilt world streams (include/render/models/model_submission.h, NM_SQ_STREAM): 1 (default) the world's batches are
# written from records built at load, no face walk and no queue; 0 the queue path alone (sq_walk_baked, sq_emit_batches),
# for A/B measurements on hardware. Both submit the same stream word for word (tests/stream_sim, SIM_NO_STREAM=1).
ifndef NATIVE_TRANSFER_BENCH
NATIVE_SQ_STREAM ?= 1
ifeq ($(NATIVE_SQ_STREAM),0)
ENJ_BASENAME := $(ENJ_BASENAME)-nostream
ENJ_CFLAGS += -DNM_SQ_STREAM=0
else ifneq ($(NATIVE_SQ_STREAM),1)
$(error NATIVE_SQ_STREAM must be 0 or 1)
endif
endif
# NATIVE_WORLD_STREAM_COURSES (default 5: Beginner and Advanced; include/course_assets.h NM_COURSE_WORLD_STREAMS): a bit
# per course slot whose world takes the whole prebuilt world stream tables. The others (Expert, slot 1: its 26,487
# faces' records would need 2.5 MB, which the heap does not have, tools/heap_budget.py --session) take the record
# cache of NATIVE_WORLD_STREAM_POOL bytes (default 458752; model_submission.h NM_SQ_STREAM_POOL): the records of the
# meshes the frames walk, built in the idle wait for the PVR (code/audio/idle_wait.c); a mesh without them takes the
# queue path, which submits the same words. 0: no cache (every world batch of those courses on the queue path).
# ELF suffixes -wstreamsN when not 5, -wpoolN when not 458752.
NATIVE_WORLD_STREAM_COURSES ?= 5
$(if $(filter 0 1 2 3 4 5 6 7,$(NATIVE_WORLD_STREAM_COURSES)),,$(error NATIVE_WORLD_STREAM_COURSES must be 0..7))
ifneq ($(NATIVE_WORLD_STREAM_COURSES),5)
ENJ_BASENAME := $(ENJ_BASENAME)-wstreams$(NATIVE_WORLD_STREAM_COURSES)
ENJ_CFLAGS += -DNM_COURSE_WORLD_STREAMS=$(NATIVE_WORLD_STREAM_COURSES)u
endif
NATIVE_WORLD_STREAM_POOL ?= 458752
$(if $(shell [ "$(NATIVE_WORLD_STREAM_POOL)" -ge 0 ] 2>/dev/null && echo ok),,$(error NATIVE_WORLD_STREAM_POOL must be a byte count))
ifneq ($(NATIVE_WORLD_STREAM_POOL),458752)
ENJ_BASENAME := $(ENJ_BASENAME)-wpool$(NATIVE_WORLD_STREAM_POOL)
ENJ_CFLAGS += -DNM_SQ_STREAM_POOL=$(NATIVE_WORLD_STREAM_POOL)u
endif
# Prebuilt car streams (include/render/models/model_submission.h, NM_SQ_CAR_STREAM): 1 (default) a car part whose bounds
# lie inside the guard view is written by a decide pass and a write pass over its faces, positions transformed once
# (lean form), no queue and no vertex marks; 0 every car part on the queue path (sq_walk_car, sq_emit_batches), for A/B
# measurements on hardware. Both submit the same stream word for word (tests/stream_sim, SIM_NO_CAR_STREAM=1).
# NATIVE_SQ_STREAM=0 turns both stream paths off.
ifndef NATIVE_TRANSFER_BENCH
NATIVE_SQ_CAR_STREAM ?= 1
ifeq ($(NATIVE_SQ_CAR_STREAM),0)
ENJ_BASENAME := $(ENJ_BASENAME)-nocarstream
ENJ_CFLAGS += -DNM_SQ_CAR_STREAM=0
else ifneq ($(NATIVE_SQ_CAR_STREAM),1)
$(error NATIVE_SQ_CAR_STREAM must be 0 or 1)
endif
endif
# NATIVE_FILTER_ALL=1 (default with NATIVE_BILINEAR=1): every 3D texture bilinear (road, punch-through cutouts,
#   cutout cores, sky) except the catch-fence stipple; the HUD and 2D screens stay point sampled. 0: opaque faces only
#   (NATIVE_BILINEAR), the road-surface class after NATIVE_ROAD_FILTER.
ifeq ($(NATIVE_FILTER_ALL),1)
ifneq ($(NATIVE_BILINEAR),1)
$(error NATIVE_FILTER_ALL=1 requires NATIVE_BILINEAR=1)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-filterall
ENJ_CFLAGS += -DNM_FILTER_ALL=1 -DNM_ROAD_FILTER=1
else ifneq ($(filter-out 0,$(NATIVE_FILTER_ALL)),)
$(error NATIVE_FILTER_ALL must be 0 or 1)
endif
# NATIVE_STIPPLE_FILTER=1: under NATIVE_FILTER_ALL=1 the catch-fence stipple (and, in the sets with a fence rule up to
#   assets-ssfar4eu, its sparser level-of-detail stipples) is filtered like the other cutouts, as before 14 October 2026
#   (a filtered one-texel checkerboard is a flat half alpha: no fence at PT_ALPHA_REF 0xFF, a smear at 0x80). Default 0:
#   point sampled (model_renderer.c), as the arcade's unfiltered screen checker. With the recommended assets-ssfar4f the
#   plain stipple is drawn at every distance, so 0 matters there at every distance, not only near.
ifeq ($(NATIVE_STIPPLE_FILTER),1)
ifneq ($(NATIVE_FILTER_ALL),1)
$(error NATIVE_STIPPLE_FILTER=1 requires NATIVE_FILTER_ALL=1 (otherwise no punch-through polygon is filtered))
endif
ENJ_BASENAME := $(ENJ_BASENAME)-stipplefilter
ENJ_CFLAGS += -DNM_STIPPLE_FILTER=1
else ifneq ($(filter-out 0,$(NATIVE_STIPPLE_FILTER)),)
$(error NATIVE_STIPPLE_FILTER must be 0 or 1)
endif
# NATIVE_PT_ALPHA_REF=1..255 (decimal or 0x hex) | none: the punch-through alpha reference register PT_ALPHA_REF,
#   written once after pvr_init (code/main.c). Default 0x80 with NATIVE_FILTER_ALL=1, otherwise none: the register
#   keeps its reset value 0xFF and is not written (point-sampled builds unchanged). The ELF name carries -ptref<value>
#   when the value is not the default of its configuration.
ifeq ($(NATIVE_FILTER_ALL),1)
NATIVE_PT_ALPHA_REF_DEFAULT := 0x80
else
NATIVE_PT_ALPHA_REF_DEFAULT := none
endif
NATIVE_PT_ALPHA_REF ?= $(NATIVE_PT_ALPHA_REF_DEFAULT)
ifndef NATIVE_TRANSFER_BENCH
ifneq ($(NATIVE_PT_ALPHA_REF),$(NATIVE_PT_ALPHA_REF_DEFAULT))
ENJ_BASENAME := $(ENJ_BASENAME)-ptref$(NATIVE_PT_ALPHA_REF)
endif
ifneq ($(NATIVE_PT_ALPHA_REF),none)
ifeq ($(shell echo '$(NATIVE_PT_ALPHA_REF)' | grep -Ex '[1-9][0-9]{0,2}|0[xX][0-9a-fA-F]{1,2}'),)
$(error NATIVE_PT_ALPHA_REF must be none or 1..255 (decimal or 0x hex))
endif
ENJ_CFLAGS += -DNM_PT_ALPHA_REF=$(NATIVE_PT_ALPHA_REF)
endif
endif
ifeq ($(NATIVE_BILINEAR),1)
ENJ_CFLAGS += -DNATIVE_BILINEAR
else
ifndef NATIVE_TRANSFER_BENCH
ENJ_BASENAME := $(ENJ_BASENAME)-unfiltered
endif
endif
ifdef NATIVE_CAMERA
$(error NATIVE_CAMERA: the model renderer has no camera diagnostic)
endif
ifdef NATIVE_BOUNDS_VERIFY
ENJ_BASENAME := $(ENJ_BASENAME)-bounds-verify
ENJ_CFLAGS += -DNATIVE_BOUNDS_VERIFY
endif
ifdef NATIVE_FRAME_COMPARE
ifndef NATIVE_DRIVING_TEST
$(error NATIVE_FRAME_COMPARE requires NATIVE_DRIVING_TEST)
endif
ENJ_CFLAGS += -DNATIVE_FRAME_COMPARE
endif
ifdef NATIVE_LIGHT_VERIFY
ENJ_CFLAGS += -DNATIVE_LIGHT_VERIFY
endif
ifdef NATIVE_CACHE_PROFILE
ifeq ($(strip $(NATIVE_DRIVING_TEST)$(filter-out 0,$(RACE_TICK_LIMIT))),)
$(error NATIVE_CACHE_PROFILE requires a finite race/test tick limit)
endif
ifdef NATIVE_PROFILE
$(error Use cache profiling separately from timing/readback profiling)
endif
NATIVE_CACHE_PROFILE_EVENT ?= PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE
ENJ_CFLAGS += -DNATIVE_CACHE_PROFILE -DNATIVE_CACHE_PROFILE_EVENT=$(NATIVE_CACHE_PROFILE_EVENT)
endif
# NATIVE_DISC=1: a build for a disc image (make NATIVE_DISC=1 cdi, recipes/README.md). The assets are read from
# NATIVE_ASSET_ROOT, default /cd/ (mkdcdisc puts ENJ_ROMDIR's contents at the disc's root); the file reads take
# the disc's paths (scene_loader.c, audio/sound.c: a GD-ROM read blocks only its own thread). Without dcload
# there is no /pc: the diagnostics that write files there are refused.
# NATIVE_ASSET_ROOT: where the game opens its assets (default /pc/, dcload's host directory).
ifdef NATIVE_DISC
ifneq ($(strip $(NATIVE_CAPTURE)$(NATIVE_DIAG_LATE_CAPTURE)$(NATIVE_SAMPLE_PROFILE)$(NATIVE_FRAME_COMPARE)),)
$(error NATIVE_DISC cannot write captures or profiles to /pc: no dcload on a disc)
endif
NATIVE_ASSET_ROOT ?= /cd/
ifneq ($(filter /pc%,$(NATIVE_ASSET_ROOT)),)
$(error NATIVE_DISC reads its assets from the disc, not $(NATIVE_ASSET_ROOT))
endif
# enDjinn's own asset directory (none of its textures, sounds or fonts are used): the asset root itself, so the
# image target does not add an empty directory to ENJ_ROMDIR.
ROMBASEPATH := $(ENJ_ROMDIR)
ENJ_BASENAME := $(ENJ_BASENAME)-disc
ENJ_CFLAGS += -DNATIVE_DISC
# NATIVE_DISC_CONSOLE=1: the console's lines still go to the serial port (without it they are dropped: main.c).
ifeq ($(NATIVE_DISC_CONSOLE),1)
ENJ_BASENAME := $(ENJ_BASENAME)-console
ENJ_CFLAGS += -DNM_DISC_CONSOLE
endif
endif
ifdef NATIVE_ASSET_ROOT
ENJ_CFLAGS += -DNM_ASSET_ROOT='"$(NATIVE_ASSET_ROOT)"'
endif
# Cars and scenery are always resident (DNT6 palette packs). The variable only
# names the build: existing build scripts pass it and expect "-resident-car".
ifdef NATIVE_RESIDENT_CAR
ENJ_BASENAME := $(ENJ_BASENAME)-resident-car
endif
ifndef NATIVE_TRANSFER_BENCH
ifndef RACE_ASSET_DIR
$(error Game build requires RACE_ASSET_DIR with the 40-car race assets)
endif
ifndef NAME_ASSET_DIR
$(error Game build requires NAME_ASSET_DIR with offline name_data.h, name_entry_data.h and labels/name_label_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NAME_ASSET_DIR))
ifndef NATIVE_RESULT_PAL4_DIR
$(error Game build requires NATIVE_RESULT_PAL4_DIR with result_pal4_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_RESULT_PAL4_DIR))
ifndef NATIVE_RESULT_DATA_DIR
$(error Game build requires NATIVE_RESULT_DATA_DIR with result_actor_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_RESULT_DATA_DIR))
ifndef NATIVE_RESULT_LABEL_DIR
$(error Game build requires NATIVE_RESULT_LABEL_DIR with result_label_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_RESULT_LABEL_DIR))
ifndef NATIVE_RECORD_DATA_DIR
$(error Game build requires NATIVE_RECORD_DATA_DIR with race_record_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_RECORD_DATA_DIR))
ifndef NATIVE_INTENT_DATA_DIR
$(error Game build requires NATIVE_INTENT_DATA_DIR with race_intent_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_INTENT_DATA_DIR))
ifndef NATIVE_PODIUM_DATA_DIR
$(error Game build requires NATIVE_PODIUM_DATA_DIR with podium_data.h)
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_PODIUM_DATA_DIR))
ifndef NATIVE_RACE_START_DATA_DIR
$(error Game build requires NATIVE_RACE_START_DATA_DIR with race_start_data.h (prepare_intro.py --race-start))
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_RACE_START_DATA_DIR))
endif
ifdef NATIVE_HUD_DIR
# Arcade race HUD (tools/prepare_hud.py output: hud_data.h; its hud/ files go
# into the asset root). Without it the race keeps the debug text.
ifeq ($(wildcard $(NATIVE_HUD_DIR)/hud_data.h),)
$(error NATIVE_HUD_DIR must contain hud_data.h (tools/prepare_hud.py))
endif
ENJ_CFLAGS += -DNATIVE_HUD -I$(abspath $(NATIVE_HUD_DIR))
ifdef NATIVE_HUD_DEBUG_TEXT
# Developers: keep the "SH4 x of 16.7 ms | GPU | late" text beside the HUD.
ENJ_CFLAGS += -DNATIVE_HUD_DEBUG_TEXT
endif
# NATIVE_DEBUG_LABELS=1: the developer labels (inspector keys, SH4/GPU times, attract stage) whenever the HUD is not
#   drawn (attract, podium cinematic), as before 18 October 2026. Default: only while the inspector is open.
ifdef NATIVE_DEBUG_LABELS
ENJ_CFLAGS += -DNATIVE_DEBUG_LABELS
endif
# NATIVE_HUD_RADAR_PANEL=1 (default): the CONDITION panel's translucent background (the original's stipple), ~100x102
#   blended pixels a frame (+0.04 ms render, measured 16 October 2026). 0 leaves it out; the radar's icons are drawn
#   either way.
NATIVE_HUD_RADAR_PANEL ?= 1
ifeq ($(NATIVE_HUD_RADAR_PANEL),1)
ENJ_CFLAGS += -DNM_HUD_RADAR_PANEL=1
else ifeq ($(NATIVE_HUD_RADAR_PANEL),0)
ENJ_BASENAME := $(ENJ_BASENAME)-noradarpanel
else
$(error NATIVE_HUD_RADAR_PANEL must be 0 or 1)
endif
# The gear indicator b0c0 (include/hud.h nm_hud_gear; needs a HUD pack with NM_HUD_GEAR), manual races.
#   NATIVE_HUD_GEAR_GATE=1 (default, chosen 18 October 2026): the lever's H-gate stays, showing the current
#   gear (as the 2001 Dreamcast version); 0: the original's, the lever animates for 31 ticks after a shift, then
#   "(n)" over "SHIFT" (suffix -nogate).
#   NATIVE_HUD_GEAR_DIGIT (with the gate): right (default, chosen 19 October 2026): the "(n)" digit to the
#   right of the gate, centred on it, from the shift on (the lever still animates), the needle behind both; 0: the
#   gate alone (suffix -gateonly). NATIVE_HUD_GEAR_DIGIT_AT_X/Y (both): the digit's cell elsewhere, arcade pixels
#   of its top left (default 297, 76; suffix -gearat<X>x<Y>). include/render/ui/hud_draw.h nm_hud_gear_place;
#   pictures: tools/hud_gear_preview.py.
#   NATIVE_HUD_GEAR_AUTO=1: the indicator in automatic races too, on the automatic gear.
NATIVE_HUD_GEAR_GATE ?= 1
ifeq ($(NATIVE_HUD_GEAR_GATE),1)
ENJ_CFLAGS += -DNM_HUD_GEAR_GATE=1
else ifeq ($(NATIVE_HUD_GEAR_GATE),0)
ENJ_BASENAME := $(ENJ_BASENAME)-nogate
else
$(error NATIVE_HUD_GEAR_GATE must be 0 or 1)
endif
ifeq ($(NATIVE_HUD_GEAR_GATE),1)
NATIVE_HUD_GEAR_DIGIT ?= right
else
NATIVE_HUD_GEAR_DIGIT ?= 0
endif
ifeq ($(NATIVE_HUD_GEAR_DIGIT),right)
ifeq ($(NATIVE_HUD_GEAR_GATE),0)
$(error NATIVE_HUD_GEAR_DIGIT=right needs NATIVE_HUD_GEAR_GATE=1)
endif
ENJ_CFLAGS += -DNM_HUD_GEAR_DIGIT=1
ifneq ($(NATIVE_HUD_GEAR_DIGIT_AT_X)$(NATIVE_HUD_GEAR_DIGIT_AT_Y),)
ifeq ($(NATIVE_HUD_GEAR_DIGIT_AT_X),)
$(error NATIVE_HUD_GEAR_DIGIT_AT_X and _Y go together)
endif
ifeq ($(NATIVE_HUD_GEAR_DIGIT_AT_Y),)
$(error NATIVE_HUD_GEAR_DIGIT_AT_X and _Y go together)
endif
ENJ_CFLAGS += -DNM_HUD_GEAR_DIGIT_AT_X=$(NATIVE_HUD_GEAR_DIGIT_AT_X) -DNM_HUD_GEAR_DIGIT_AT_Y=$(NATIVE_HUD_GEAR_DIGIT_AT_Y)
ENJ_BASENAME := $(ENJ_BASENAME)-gearat$(NATIVE_HUD_GEAR_DIGIT_AT_X)x$(NATIVE_HUD_GEAR_DIGIT_AT_Y)
endif
else ifeq ($(NATIVE_HUD_GEAR_DIGIT),0)
ifeq ($(NATIVE_HUD_GEAR_GATE),1)
ENJ_BASENAME := $(ENJ_BASENAME)-gateonly
endif
else
$(error NATIVE_HUD_GEAR_DIGIT must be 0 or right)
endif
ifeq ($(NATIVE_HUD_GEAR_AUTO),1)
ENJ_CFLAGS += -DNM_HUD_GEAR_AUTO=1
ENJ_BASENAME := $(ENJ_BASENAME)-gearauto
endif
# NATIVE_TR_AUTOSORT=1 (measurement): the translucent list in the PVR's auto-sort mode (per-pixel depth sort) instead
#   of the default pre-sort mode, where the polygons are drawn in submission order (the car effects' smoke, sorted back
#   to front by the renderer: model_submission.h, "Translucent effects"; code/main.c).
ifeq ($(NATIVE_TR_AUTOSORT),1)
ENJ_CFLAGS += -DNM_TR_AUTOSORT=1
ENJ_BASENAME := $(ENJ_BASENAME)-trautosort
else ifneq ($(filter-out 0,$(NATIVE_TR_AUTOSORT)),)
$(error NATIVE_TR_AUTOSORT must be 0 or 1)
endif
# NATIVE_SMOKE_MODE (material_pack.h, NM_SMOKE_MODE): how the cars' smoke (the arcade's screen-door checker materials,
#   e409) is drawn. Default tr-screen with NATIVE_SMOKE_POINT=1 NATIVE_SMOKE_TIGHT=1 (chosen 16 October 2026,
#   hardware race E: render 8.75 ms mean / 15.4 max, 87 frames over 12 ms, none over 16.67, late frames as without
#   screen checker; pt-screen 8.95 / 17.0, one over 16.67; tr 8.71 / 14.5).
#   tr-screen (default): a per-PIXEL screen checkerboard like the arcade's: every vertex of a smoke face takes the face's
#     nearest depth (one 1/w, so u is linear on the screen) and u gets half a texture width per pixel of x+y; the
#     texture is the smoke shape beside an all-transparent half (128x64), so every other pixel reads the transparent
#     half. Drawn on the translucent list, pre-sorted back to front (MODULATE, SRCALPHA/INVSRCALPHA, no depth write): a
#     pixel takes the smoke texel or keeps the scene, the nearest smoke wins as on the arcade, and the PVR pays one
#     pre-sorted translucent sample a covered pixel instead of punch-through passes through every hole layer.
#   tr: translucent list, alpha 0.5, pre-sorted back to front (ELF suffix -smoketr).
#   pt-tex: punch-through list, the smoke texture with every other TEXEL transparent, point sampled; depth written,
#     no sort. The checker lives in texture space: it scales and turns with each puff (suffix -smokepttex).
#   pt-screen: the tr-screen checker on the punch-through list, depth written, no sort; costs a punch-through pass per
#     hole layer (suffix -smokeptscreen).
# NATIVE_SMOKE_POINT (pt-screen / tr-screen; default 1 with tr-screen, 0 with pt-screen): the checker texture point
#   sampled instead of bilinear (tr-screen: alpha then 0 or 1). NATIVE_SMOKE_TIGHT (pt-screen / tr-screen; default 1
#   with tr-screen, 0 with pt-screen): tight smoke geometry, each face drawn as up to NATIVE_SMOKE_PIECES=1..3 (default
#   2) rectangles of its texture that hold every texel a sample can see (model_submission.h, sq_tr_tight_prepare).
#   Suffixes only where they differ from the mode's default (-bilinear/-point, -loose/-tight). The make always passes
#   NM_SMOKE_MODE/POINT/TIGHT; the C defaults (material_pack.h, model_submission.h: tr, 0, 0) serve host tools that
#   choose their mode themselves (tests/stream_sim/build_sim.py).
NATIVE_SMOKE_MODE ?= tr-screen
ifeq ($(NATIVE_SMOKE_MODE),tr-screen)
NATIVE_SMOKE_POINT ?= 1
NATIVE_SMOKE_TIGHT ?= 1
endif
NATIVE_SMOKE_POINT ?= 0
NATIVE_SMOKE_TIGHT ?= 0
ifeq ($(NATIVE_SMOKE_MODE),pt-tex)
ENJ_CFLAGS += -DNM_SMOKE_MODE=1
ENJ_BASENAME := $(ENJ_BASENAME)-smokepttex
else ifeq ($(NATIVE_SMOKE_MODE),pt-screen)
ENJ_CFLAGS += -DNM_SMOKE_MODE=2
ENJ_BASENAME := $(ENJ_BASENAME)-smokeptscreen
else ifeq ($(NATIVE_SMOKE_MODE),tr-screen)
ENJ_CFLAGS += -DNM_SMOKE_MODE=3
else ifeq ($(NATIVE_SMOKE_MODE),tr)
ENJ_CFLAGS += -DNM_SMOKE_MODE=0
ENJ_BASENAME := $(ENJ_BASENAME)-smoketr
else
$(error NATIVE_SMOKE_MODE must be tr-screen, tr, pt-tex or pt-screen)
endif
ifneq ($(filter $(NATIVE_SMOKE_POINT)$(NATIVE_SMOKE_TIGHT),11 01 10),)
ifeq ($(filter $(NATIVE_SMOKE_MODE),pt-screen tr-screen),)
$(error NATIVE_SMOKE_POINT and NATIVE_SMOKE_TIGHT need NATIVE_SMOKE_MODE=pt-screen or tr-screen)
endif
endif
ifeq ($(NATIVE_SMOKE_POINT),1)
ENJ_CFLAGS += -DNM_SMOKE_POINT=1
ifneq ($(NATIVE_SMOKE_MODE),tr-screen)
ENJ_BASENAME := $(ENJ_BASENAME)-point
endif
else ifeq ($(NATIVE_SMOKE_POINT),0)
ENJ_CFLAGS += -DNM_SMOKE_POINT=0
ifeq ($(NATIVE_SMOKE_MODE),tr-screen)
ENJ_BASENAME := $(ENJ_BASENAME)-bilinear
endif
else
$(error NATIVE_SMOKE_POINT must be 0 or 1)
endif
ifeq ($(NATIVE_SMOKE_TIGHT),1)
ENJ_CFLAGS += -DNM_SMOKE_TIGHT=1
ifneq ($(NATIVE_SMOKE_MODE),tr-screen)
ENJ_BASENAME := $(ENJ_BASENAME)-tight
endif
ifneq ($(NATIVE_SMOKE_PIECES),)
ifeq ($(filter $(NATIVE_SMOKE_PIECES),1 2 3),)
$(error NATIVE_SMOKE_PIECES must be 1, 2 or 3)
endif
ENJ_CFLAGS += -DNM_SMOKE_PIECES=$(NATIVE_SMOKE_PIECES)
ENJ_BASENAME := $(ENJ_BASENAME)-pieces$(NATIVE_SMOKE_PIECES)
endif
else ifeq ($(NATIVE_SMOKE_TIGHT),0)
ENJ_CFLAGS += -DNM_SMOKE_TIGHT=0
ifeq ($(NATIVE_SMOKE_MODE),tr-screen)
ENJ_BASENAME := $(ENJ_BASENAME)-loose
endif
else
$(error NATIVE_SMOKE_TIGHT must be 0 or 1)
endif
ifeq ($(NATIVE_HUD_STRIPS),1)
# Measurement/fallback: each HUD rectangle as a strip of four vertices (four TA
# records) instead of one PVR sprite (two); same corners, same pixels.
ENJ_BASENAME := $(ENJ_BASENAME)-hudstrips
ENJ_CFLAGS += -DNATIVE_HUD_STRIPS
endif
endif
ifdef NATIVE_FRAME_PROFILE
# Two updater timestamps only; does not enable per-face NATIVE_PROFILE timers.
ENJ_CFLAGS += -DNATIVE_FRAME_PROFILE
endif
ifdef NATIVE_HANDOPT
# Keep generated .s files separate from ordinary objects: make does not track
# command-line option changes when deciding whether an object is current.
ENJ_BASENAME := $(ENJ_BASENAME)-handopt
endif
ifdef NATIVE_NAME_DATA_DIR
ifndef NATIVE_RACE_MOVEMENT_CHECK
$(error NATIVE_NAME_DATA_DIR currently requires NATIVE_RACE_MOVEMENT_CHECK)
endif
ENJ_CFLAGS += -DNATIVE_NAME_DATA -I$(abspath $(NATIVE_NAME_DATA_DIR))
endif
ENJ_BUILDDIR := build/$(ENJ_BASENAME)
$(ENJ_BUILDDIR)/code/render/ui/title_render.o: $(abspath $(TITLE_SCENE_DIR))/native_scene.inc
ifdef NATIVE_RACE_MOVEMENT_CHECK
ifndef RACE_ASSET_DIR
$(error NATIVE_RACE_MOVEMENT_CHECK requires the 40-car race assets)
endif
ENJ_CFLAGS += -DNATIVE_RACE_MOVEMENT_CHECK
endif
ifdef NATIVE_INTENT_DATA_DIR
ifdef NATIVE_CONTACT_CHECK
ENJ_CFLAGS += -DNATIVE_INTENT_DATA -I$(abspath $(NATIVE_INTENT_DATA_DIR))
endif
endif
ENJ_INJECT_QFONT := 1
ifdef NATIVE_SAMPLE_PROFILE
# Statistical SH4 profile (include/sample_profile.h): TMU1 samples the
# interrupted PC, tagged with frame section and scene phase; written to the
# asset root after shutdown. Diagnostic, not a game build.
ENJ_BASENAME := $(ENJ_BASENAME)-sample-profile
ENJ_CFLAGS += -DNATIVE_SAMPLE_PROFILE
endif
# Punch-through chunk size of the PVR's ISP_FEED_CFG in vertices (code/main.c). Default 512; KOS's own value is 64.
# NATIVE_DIAG_PT_CHUNK is the former name of this setting and still accepted.
ifdef NATIVE_DIAG_PT_CHUNK
NATIVE_PT_CHUNK ?= $(NATIVE_DIAG_PT_CHUNK)
endif
NATIVE_PT_CHUNK ?= 512
ifneq ($(filter-out 32 64 128 256 512,$(NATIVE_PT_CHUNK)),)
$(error NATIVE_PT_CHUNK must be 32, 64, 128, 256 or 512)
endif
ifneq ($(NATIVE_PT_CHUNK),512)
ENJ_BASENAME := $(ENJ_BASENAME)-ptchunk$(NATIVE_PT_CHUNK)
endif
ENJ_CFLAGS += -DNM_PT_CHUNK=$(NATIVE_PT_CHUNK)
# Measurement build for the PVR render time (not a game build):
# NATIVE_DIAG_CAR_OPAQUE=1: the cars' punch-through faces on the opaque list, alpha ignored (wrong picture).
ifeq ($(NATIVE_DIAG_CAR_OPAQUE),1)
ENJ_BASENAME := $(ENJ_BASENAME)-caropaque
ENJ_CFLAGS += -DNM_DIAG_CAR_OPAQUE=1
endif
# More measurement builds for the PVR render time (13 October 2026). Each removes or alters ONE thing of the picture
# and names the ELF; none is a game build, none changes what gameplay reads (race state, the drawn-car count of the
# detail pass, checkpoints, the scripted flow). Without them the code is unchanged.
# NATIVE_DIAG_RENDER_STATS=1: changes nothing of the picture; counts the PVR render time of every frame of the run
#   (the "Practice PVR" line covers the first 596 frames only) and prints means, a histogram and the worst frames with
#   their race tick after the run (code/practice.c). The measurement ELFs are built with it.
ifeq ($(NATIVE_DIAG_RENDER_STATS),1)
ENJ_BASENAME := $(ENJ_BASENAME)-renderstats
ENJ_CFLAGS += -DNM_DIAG_RENDER_STATS=1
endif
# NATIVE_DIAG_DROP_LOG=1: changes nothing of the picture or the race; logs every displayed frame of the practice scene
#   whose page flip came more than one vblank after the previous one: run and race tick, phase, scene, updater time
#   and simulation ticks, wait, registration, PVR render time, and the cause (pvr, cpu-update, cpu-registration,
#   cpu-other, load, other; rules in code/practice.c). KOS's PVR statistics hook pvr_sync_stats is
#   wrapped at link time to time each scene's registration, render and flip (KOS is unchanged). Printed after the run
#   (DROP lines: the first 200 records, counts by phase and by 500-tick range, DROP-SUMMARY);
#   tools/drop_log_report.py summarises a console log.
ifeq ($(NATIVE_DIAG_DROP_LOG),1)
ifdef NATIVE_SAMPLE_PROFILE
$(error NATIVE_DIAG_DROP_LOG and NATIVE_SAMPLE_PROFILE both wrap the practice updater)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-droplog
ENJ_CFLAGS += -DNM_DIAG_DROP_LOG=1
ENJ_LDFLAGS += -Wl,--wrap=pvr_sync_stats
endif
# NATIVE_DIAG_FRAME_TRACE=1 (needs NATIVE_DIAG_DROP_LOG=1): every displayed frame's times and, for the run ticks
#   NATIVE_DIAG_FRAME_TRACE_LO..HI, what they were spent on (include/frame_trace.h). Changes nothing of the picture or
#   the race; the marks cost a register read each. Printed after the run; tools/frame_trace_report.py reads them.
ifeq ($(NATIVE_DIAG_FRAME_TRACE),1)
ifneq ($(NATIVE_DIAG_DROP_LOG),1)
$(error NATIVE_DIAG_FRAME_TRACE needs NATIVE_DIAG_DROP_LOG=1)
endif
ifdef NATIVE_DIAG_LISTS
$(error NATIVE_DIAG_FRAME_TRACE and NATIVE_DIAG_LISTS both wrap pvr_list_begin)
endif
NATIVE_DIAG_FRAME_TRACE_LO ?= 0
NATIVE_DIAG_FRAME_TRACE_HI ?= 0
ENJ_BASENAME := $(ENJ_BASENAME)-frametrace
ENJ_CFLAGS += -DNM_DIAG_FRAME_TRACE=1 -DNM_FT_LO=$(NATIVE_DIAG_FRAME_TRACE_LO) -DNM_FT_HI=$(NATIVE_DIAG_FRAME_TRACE_HI)
ENJ_LDFLAGS += -Wl,--wrap=pvr_list_begin -Wl,--wrap=pvr_list_finish
endif
# NATIVE_DIAG_LATE_CAPTURE=N (1..64; needs NATIVE_DIAG_DROP_LOG=1): pictures of the late frames (include/late_capture.h,
#   rules in code/practice.c): the first N late frames that are not a load or caused by a capture, the N worst as far as
#   the slots allow, and the frame shown before each where lateness is known in time. Copied from the frame buffer
#   in the frame's last punch-through callback (a copy delays that registration and competes with a running render
#   for VRAM: frames it may delay that are late get the cause "capture" and are not captured), kept in a pool
#   allocated before any asset, written after the run to <asset root>NAME.bin and NAME.csv (dcload /pc/). Changes
#   only timing: the race and its checkpoints are those of the build without it.
#   NATIVE_DIAG_LATE_CAPTURE_FULL=1: 640x480 packed 24-bit as in the frame buffer (921,600 bytes a picture) instead
#   of 320x240 RGB565 2x2 box (153,600). NATIVE_DIAG_LATE_CAPTURE_SPARE=S: slots beyond the N for the worst frames
#   after the first N and the "before" pictures (default 4, full resolution 0; set: ELF suffix -spareS).
#   NATIVE_DIAG_LATE_CAPTURE_NAME: NAME, the file name in the asset root (default late-capture, full resolution
#   late-capture-full). The check target's heap budget counts the pool and accepts
#   1 MiB free at the peak and while the scene runs (nothing is allocated after the race scene load).
#   tools/late_capture_report.py makes the PNGs, captions and a contact sheet.
ifdef NATIVE_DIAG_LATE_CAPTURE
ifneq ($(NATIVE_DIAG_DROP_LOG),1)
$(error NATIVE_DIAG_LATE_CAPTURE needs NATIVE_DIAG_DROP_LOG=1)
endif
ifneq ($(shell [ "$(NATIVE_DIAG_LATE_CAPTURE)" -ge 1 ] 2>/dev/null && [ "$(NATIVE_DIAG_LATE_CAPTURE)" -le 64 ] && echo ok),ok)
$(error NATIVE_DIAG_LATE_CAPTURE must be a number of slots from 1 to 64)
endif
ifeq ($(NATIVE_DIAG_LATE_CAPTURE_FULL),1)
NATIVE_DIAG_LATE_CAPTURE_SPARE ?= 0
NM_LATE_CAPTURE_IMAGE_BYTES := 921600
ENJ_CFLAGS += -DNM_LATE_CAPTURE_FULL_RES=1
LATE_CAPTURE_SUFFIX := full
else
NATIVE_DIAG_LATE_CAPTURE_SPARE ?= 4
NM_LATE_CAPTURE_IMAGE_BYTES := 153600
LATE_CAPTURE_SUFFIX :=
endif
ifneq ($(shell [ "$(NATIVE_DIAG_LATE_CAPTURE_SPARE)" -ge 0 ] 2>/dev/null && [ "$(NATIVE_DIAG_LATE_CAPTURE_SPARE)" -le 64 ] && echo ok),ok)
$(error NATIVE_DIAG_LATE_CAPTURE_SPARE must be a number from 0 to 64)
endif
NM_LATE_CAPTURE_SLOTS := $(shell echo $$(($(NATIVE_DIAG_LATE_CAPTURE)+$(NATIVE_DIAG_LATE_CAPTURE_SPARE))))
NM_LATE_CAPTURE_POOL_BYTES := $(shell echo $$(($(NM_LATE_CAPTURE_SLOTS)*$(NM_LATE_CAPTURE_IMAGE_BYTES))))
NATIVE_DIAG_LATE_CAPTURE_NAME ?= late-capture$(if $(LATE_CAPTURE_SUFFIX),-full)
ifeq ($(origin NATIVE_DIAG_LATE_CAPTURE_SPARE),command line)
LATE_CAPTURE_SUFFIX := $(LATE_CAPTURE_SUFFIX)-spare$(NATIVE_DIAG_LATE_CAPTURE_SPARE)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-latecap$(NATIVE_DIAG_LATE_CAPTURE)$(LATE_CAPTURE_SUFFIX)
ENJ_CFLAGS += -DNM_DIAG_LATE_CAPTURE=$(NATIVE_DIAG_LATE_CAPTURE) -DNM_LATE_CAPTURE_FIRST=$(NATIVE_DIAG_LATE_CAPTURE) \
  -DNM_LATE_CAPTURE_SLOTS=$(NM_LATE_CAPTURE_SLOTS) '-DNM_LATE_CAPTURE_NAME="$(NATIVE_DIAG_LATE_CAPTURE_NAME)"'
endif
# NATIVE_DIAG_PMU=1: SH4 performance counters per frame section (updater, registration callbacks): instruction and
#   operand cache misses, pipeline freeze cycles by each, instructions issued and cycles, per scene phase, after the
#   run ("PMU" lines; code/practice.c). Takes both counters (PRFC0, KOS's optional perf_cntr_timer, is restored).
ifeq ($(NATIVE_DIAG_PMU),1)
ifneq ($(strip $(NATIVE_SAMPLE_PROFILE)$(NATIVE_CACHE_PROFILE)$(NATIVE_PROFILE)),)
$(error NATIVE_DIAG_PMU cannot share the performance counters or the updater with NATIVE_SAMPLE_PROFILE, NATIVE_CACHE_PROFILE or NATIVE_PROFILE)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-pmu
ENJ_CFLAGS += -DNM_DIAG_PMU=1
endif
# Code placement (14 October 2026). NATIVE_HOT_ORDER=1: the per-frame hot functions (list callbacks, world and car
#   stream paths, transforms, emitters, assembly writers, the hottest gameplay routines, enDjinn's frame loop, KOS's
#   PVR/store-queue helpers) are placed contiguously at the start of .text in call order, from a section ordering
#   file (GNU ld --section-ordering-file; every object is already compiled with -ffunction-sections, KOS's library
#   too). KOS's linker script is unchanged. Default file config/hot_order.ld (tools/hot_order.py writes it;
#   tools/icache_layout.py reports the layout of an ELF). NATIVE_HOT_ORDER_FILE=<file> selects another. The file
#   names the sections of a build without LTO; under the default NATIVE_LTO=one most functions are renamed or
#   inlined, so use it with NATIVE_LTO=0. Under LTO the ltrans object still has one section per function and
#   variable: tools/pin_layout.py writes a file from a reference build's link map (NATIVE_LINK_MAP) that puts a
#   changed build's code and data back at the reference's offsets, for an A/B whose placement is the same.
ifeq ($(NATIVE_HOT_ORDER),1)
NATIVE_HOT_ORDER_FILE ?= $(abspath config/hot_order.ld)
ifeq ($(wildcard $(NATIVE_HOT_ORDER_FILE)),)
$(error NATIVE_HOT_ORDER_FILE $(NATIVE_HOT_ORDER_FILE) does not exist)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-hotorder
ENJ_LDFLAGS += -Wl,--section-ordering-file=$(NATIVE_HOT_ORDER_FILE)
else ifneq ($(filter-out 0,$(NATIVE_HOT_ORDER)),)
$(error NATIVE_HOT_ORDER must be 0 or 1)
endif
# NATIVE_ALIGN_FUNCTIONS=n / NATIVE_ALIGN_LOOPS=n (4, 8, 16, 32): -falign-functions / -falign-loops for every unit
#   (GCC's SH4 defaults: functions 4, loops 4, jumps 2). Measurement option: names the ELF.
ifdef NATIVE_ALIGN_FUNCTIONS
ifeq ($(filter $(NATIVE_ALIGN_FUNCTIONS),4 8 16 32),)
$(error NATIVE_ALIGN_FUNCTIONS must be 4, 8, 16 or 32)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-alignfn$(NATIVE_ALIGN_FUNCTIONS)
ENJ_CFLAGS += -falign-functions=$(NATIVE_ALIGN_FUNCTIONS)
endif
ifdef NATIVE_ALIGN_LOOPS
ifeq ($(filter $(NATIVE_ALIGN_LOOPS),4 8 16 32),)
$(error NATIVE_ALIGN_LOOPS must be 4, 8, 16 or 32)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-alignloop$(NATIVE_ALIGN_LOOPS)
ENJ_CFLAGS += -falign-loops=$(NATIVE_ALIGN_LOOPS)
endif
# NATIVE_GAMEPLAY_OPT=<variant>: optimisation of the gameplay translation units only (code/practice.c, drive.c,
#   contact.c, road.c, path.c, visibility.c, gameplay/results.c; the Makefile applies it). The game update must stay
#   bit-identical to the arcade: every unit keeps -fno-fast-math -ffp-contract=off, under which inlining, LTO and
#   the optimisation level do not change any IEEE result. Default: -O3 like every other unit (the build's own value).
#   o2: -O2. lto: -O3 -flto (link-time optimisation among the gameplay units; fat objects, so check_build.sh and
#   the .s files still see code). inline: -O3 with larger inlining limits. noinline: -O3 -fno-inline-functions.
#   os: -Os. tools/sh4_gameplay_sim.py runs the same flags on the SH4 in the GDB simulator against the checkpoints.
ifdef NATIVE_GAMEPLAY_OPT
ifeq ($(NATIVE_GAMEPLAY_OPT),o2)
NATIVE_GAMEPLAY_OPT_FLAGS := -O2
else ifeq ($(NATIVE_GAMEPLAY_OPT),lto)
NATIVE_GAMEPLAY_OPT_FLAGS := -O3 -flto -ffat-lto-objects
# LTO internalises functions no other object calls; tests/check_build.sh looks for nm_scenery_load as a global
# symbol, so the link names it as an undefined reference first (no code change).
ENJ_LDFLAGS += -flto=4 -Wl,-u,_nm_scenery_load
else ifeq ($(NATIVE_GAMEPLAY_OPT),inline)
NATIVE_GAMEPLAY_OPT_FLAGS := -O3 --param=max-inline-insns-auto=60 --param=max-inline-insns-single=400
else ifeq ($(NATIVE_GAMEPLAY_OPT),noinline)
NATIVE_GAMEPLAY_OPT_FLAGS := -O3 -fno-inline-functions
else ifeq ($(NATIVE_GAMEPLAY_OPT),os)
NATIVE_GAMEPLAY_OPT_FLAGS := -Os
else
$(error NATIVE_GAMEPLAY_OPT must be o2, lto, inline, noinline or os)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-gp$(NATIVE_GAMEPLAY_OPT)
endif
# NATIVE_LTO (default one): link-time optimisation of the whole program in one partition (every unit -flto, fat
#   objects so check_build.sh and the .s files still see code; the link -flto-partition=one). The Diddy Kong Racing
#   port measured 1.5x the frames over its display-list budget from object order alone under default partitioning,
#   and a third of them with one partition. The compile flags travel per function (gameplay -fno-fast-math
#   -ffp-contract=off, presentation -ffast-math); the link line adds no math flag. Hardware, 15 October (stats ELF,
#   scripted fast race, runs interleaved): checkpoint differences 0, race registration mean 7.73 -> 7.56 ms, podium
#   10.19 -> 9.91 ms, late frames 4 -> 1. NATIVE_LTO=0: no LTO, ELF suffix -nolto. NATIVE_GAMEPLAY_OPT (a measurement
#   of the gameplay units alone) turns it off.
ifdef NATIVE_GAMEPLAY_OPT
NATIVE_LTO ?= 0
endif
NATIVE_LTO ?= one
ifeq ($(NATIVE_LTO),one)
ifdef NATIVE_GAMEPLAY_OPT
$(error NATIVE_LTO=one and NATIVE_GAMEPLAY_OPT are exclusive)
endif
ENJ_CFLAGS += -flto -ffat-lto-objects
# LTO internalises functions no other object calls; tests/check_build.sh looks for nm_scenery_load as a global
# symbol, so the link names it as an undefined reference first (no code change).
ENJ_LDFLAGS += -flto=1 -flto-partition=one -Wl,-u,_nm_scenery_load
else ifeq ($(NATIVE_LTO),0)
ifndef NATIVE_GAMEPLAY_OPT
ENJ_BASENAME := $(ENJ_BASENAME)-nolto
endif
else
$(error NATIVE_LTO must be one or 0)
endif
# NATIVE_LAYOUT (default 1 under NATIVE_LTO=one, 18 October 2026): the race's hot code and data at fixed places,
#   independent of every other change (tools/hot_layout.py says why and how; config/hot_layout.tsv is the hot set).
#   KOS's startup, the hot data block at a fixed residue, the registration, per-frame and updater functions in fixed
#   slots at the start of .text, the image end (heap start) at a fixed residue mod 16 KiB. The link writes the
#   ordering file from the config (build dir hot_layout.ld) and warns loudly when an entry is not where the config
#   puts it; make check fails then. 0: the linker's own order (ELF suffix -nolayout). Off with NATIVE_HOT_ORDER=1
#   (another ordering file) and by default with NATIVE_LTO=0 (other function names).
ifeq ($(NATIVE_HOT_ORDER),1)
NATIVE_LAYOUT ?= 0
endif
ifeq ($(NATIVE_LTO),one)
NATIVE_LAYOUT ?= 1
endif
NATIVE_LAYOUT ?= 0
ifeq ($(NATIVE_LAYOUT),1)
ifeq ($(NATIVE_HOT_ORDER),1)
$(error NATIVE_LAYOUT=1 and NATIVE_HOT_ORDER=1 are exclusive)
endif
NATIVE_LAYOUT_LD = $(abspath $(ENJ_BUILDDIR))/hot_layout.ld
ENJ_LDFLAGS += -Wl,--section-ordering-file=$(NATIVE_LAYOUT_LD)
else ifeq ($(NATIVE_LAYOUT),0)
ifeq ($(NATIVE_LTO),one)
ifneq ($(NATIVE_HOT_ORDER),1)
ENJ_BASENAME := $(ENJ_BASENAME)-nolayout
endif
endif
else
$(error NATIVE_LAYOUT must be 0 or 1)
endif
# NATIVE_SFX_VOICE_STREAM=1: the announcer's phrases streamed from ASSET_ROOT/sfx/voices.pak (audio/sfx.h; recipes
#   build_sfx.sh --voices stream), one at a time beside the PCM8 race music: a worker thread (8 KiB stack), the
#   pack's directory and one phrase buffer (the largest phrase, about 24 KB) on the heap. Without it, phrases play
#   only when the race bank holds them (build_sfx.sh --voices resident, beside the race music in ADPCM).
ifeq ($(NATIVE_SFX_VOICE_STREAM),1)
ENJ_BASENAME := $(ENJ_BASENAME)-voices
ENJ_CFLAGS += -DNM_SFX_VOICE_STREAM=1
endif
# File reads of the sound (the music's loads, the streamed phrases) in enDjinn's idle wait for the PVR
# (code/audio/idle_wait.c __wrap_pvr_wait_ready).
ENJ_LDFLAGS += -Wl,--wrap=pvr_wait_ready
# A scene's load in the frames of the screen before it (code/scene_loader.c, scene_loader.h): its job's file calls
# are cut into pieces that fit the idle waits (KOS fs_read, fs_open; fs_write, fs_seek, fs_close wait for room for a
# call; every other caller passes straight through).
ENJ_LDFLAGS += -Wl,--wrap=fs_read -Wl,--wrap=fs_open -Wl,--wrap=fs_write -Wl,--wrap=fs_seek -Wl,--wrap=fs_close
# NATIVE_ATTRACT_LOOPS=N (measurement; ELF suffix -attractN): the attract's N cinematics with their titles (the
# loop's courses: 0, 2, 1, ...; the next one loads in each title's frames), then the run ends (stats printed).
ifdef NATIVE_ATTRACT_LOOPS
ENJ_BASENAME := $(ENJ_BASENAME)-attract$(NATIVE_ATTRACT_LOOPS)
ENJ_CFLAGS += -DNM_ATTRACT_LOOPS=$(NATIVE_ATTRACT_LOOPS)
endif
# NATIVE_LINK_MAP=<file>: also write the linker map (no change to the image).
ifdef NATIVE_LINK_MAP
ENJ_LDFLAGS += -Wl,-Map=$(abspath $(NATIVE_LINK_MAP))
endif
# NATIVE_DIAG_NO_HUD=1: the race HUD is not submitted (and no debug text in its place).
ifeq ($(NATIVE_DIAG_NO_HUD),1)
ENJ_BASENAME := $(ENJ_BASENAME)-nohud
ENJ_CFLAGS += -DNM_DIAG_NO_HUD=1
endif
# NATIVE_DIAG_NO_SHADOWS=1: no shadow volumes submitted and no shadow-receiver bit on any header.
#   =volumes: the receiver bit stays, no volume is submitted. =receivers: volumes are submitted, no header receives.
ifdef NATIVE_DIAG_NO_SHADOWS
ifeq ($(NATIVE_DIAG_NO_SHADOWS),1)
ENJ_BASENAME := $(ENJ_BASENAME)-noshadows
ENJ_CFLAGS += -DNM_DIAG_NO_SHADOW_VOLUMES=1 -DNM_DIAG_NO_SHADOW_RECEIVERS=1
else ifeq ($(NATIVE_DIAG_NO_SHADOWS),volumes)
ENJ_BASENAME := $(ENJ_BASENAME)-noshadowvolumes
ENJ_CFLAGS += -DNM_DIAG_NO_SHADOW_VOLUMES=1
else ifeq ($(NATIVE_DIAG_NO_SHADOWS),receivers)
ENJ_BASENAME := $(ENJ_BASENAME)-noshadowreceivers
ENJ_CFLAGS += -DNM_DIAG_NO_SHADOW_RECEIVERS=1
else
$(error NATIVE_DIAG_NO_SHADOWS must be 1, volumes or receivers)
endif
endif
# NATIVE_DIAG_NO_SKY=1: no sky quads; the PVR background plane in one flat colour instead.
ifeq ($(NATIVE_DIAG_NO_SKY),1)
ENJ_BASENAME := $(ENJ_BASENAME)-nosky
ENJ_CFLAGS += -DNM_DIAG_NO_SKY=1
endif
# NATIVE_DIAG_NO_CARS=1: no car model reaches the renderer (shadow volumes stay; gameplay's detail pass is untouched).
ifeq ($(NATIVE_DIAG_NO_CARS),1)
ENJ_BASENAME := $(ENJ_BASENAME)-nocars
ENJ_CFLAGS += -DNM_DIAG_NO_CARS=1
endif
# NATIVE_DIAG_NO_WORLD=1: no world instance reaches the renderer: cars, shadow volumes, HUD and sky only.
ifeq ($(NATIVE_DIAG_NO_WORLD),1)
ENJ_BASENAME := $(ENJ_BASENAME)-noworld
ENJ_CFLAGS += -DNM_DIAG_NO_WORLD=1
endif
# NATIVE_DIAG_LISTS=3: the two translucent lists get no object pointer blocks ("No List": Empty PTR in every tile of
#   the region array) and are never opened. =2: the opaque modifier list too (needs NATIVE_DIAG_NO_SHADOWS=1 or
#   =volumes). enDjinn opens all five lists every frame, so the build wraps pvr_list_begin/pvr_list_finish at link
#   time (code/main.c); neither enDjinn nor KOS is changed. Race scenes only: nothing of them uses those lists.
ifdef NATIVE_DIAG_LISTS
ifeq ($(filter $(NATIVE_DIAG_LISTS),2 3),)
$(error NATIVE_DIAG_LISTS must be 3 (opaque, opaque modifier, punch-through) or 2 (opaque, punch-through))
endif
ifeq ($(NATIVE_DIAG_LISTS),2)
ifeq ($(filter $(NATIVE_DIAG_NO_SHADOWS),1 volumes),)
$(error NATIVE_DIAG_LISTS=2 closes the opaque modifier list: it needs NATIVE_DIAG_NO_SHADOWS=1 or volumes)
endif
endif
ENJ_BASENAME := $(ENJ_BASENAME)-lists$(NATIVE_DIAG_LISTS)
ENJ_CFLAGS += -DNM_DIAG_LISTS=$(NATIVE_DIAG_LISTS)
ENJ_LDFLAGS += -Wl,--wrap=pvr_list_begin -Wl,--wrap=pvr_list_finish
endif
# NATIVE_DIAG_FB565=1: 16-bit RGB565 frame buffer, dithering off. ONLY a measurement of what the pixel format costs in
#   render time; the product keeps the 24-bit frame buffer (a project decision).
ifeq ($(NATIVE_DIAG_FB565),1)
ENJ_BASENAME := $(ENJ_BASENAME)-fb565
ENJ_CFLAGS += -DNM_DIAG_FB565=1
endif
# NATIVE_PAL16=1555: palette RAM in ARGB1555 instead of ARGB8888 for the whole program (include/render/materials/
#   palette_ram.h): the format register set once at startup before any entry, every palette written in that format at
#   its load (cars, scenery, HUD, finish actor, result hand-off, title, name labels), 5 bits a channel (nearest under
#   the PVR's bit-repeat expansion), alpha bit = alpha >= 128. The packs are unchanged (ARGB8888 source colours).
#   KOS's pvr_set_pal_format (dc/pvr/pvr_pal.h) warns that a filtered palette texture renders at about half speed with
#   ARGB8888 entries.
#   Default 1555 since 14 October 2026 (a project decision); NATIVE_PAL16=8888 builds ARGB8888 palette RAM as before.
#   565 is refused: RGB565 entries have no alpha and the punch-through list needs transparent entries.
ifdef NATIVE_PAL16
ifeq ($(NATIVE_PAL16),1555)
ENJ_BASENAME := $(ENJ_BASENAME)-pal1555
ENJ_CFLAGS += -DNM_PAL16=1555
ifeq ($(NATIVE_DIAG_PAL1555),1)
$(error NATIVE_DIAG_PAL1555 and NATIVE_PAL16=1555 together: the measurement switch is superseded (it needs NATIVE_PAL16=8888))
endif
else ifeq ($(NATIVE_PAL16),8888)
else ifeq ($(NATIVE_PAL16),565)
$(error NATIVE_PAL16=565: RGB565 palette entries have no alpha; the HUD, the finish actor and every cutout draw on the punch-through list from transparent entries (palette format is one register for all banks): use 1555)
else
$(error NATIVE_PAL16 must be 1555 or 8888)
endif
endif
# NATIVE_DIAG_PAL1555=1 (superseded by NATIVE_PAL16=1555; its picture is wrong): converted palette RAM to ARGB1555 before
#   the first frame by writing 16-bit entries while the register still said ARGB8888 and switching the register after,
#   against the text's rule (PAL_RAM_CTRL "must be set before storing any data"); the result hand-off then set the
#   register back to ARGB8888 for its own palettes and its readback checks failed, which ended the scripted run at phase 25.
ifeq ($(NATIVE_DIAG_PAL1555),1)
ENJ_BASENAME := $(ENJ_BASENAME)-pal1555
ENJ_CFLAGS += -DNM_DIAG_PAL1555=1
endif
# NATIVE_OPB=8|16 (default 16): words of an object pointer block of every list (code/main.c NM_OPB_WORDS); 8 was the
#   value before 16 October 2026 (ELF suffix -opb8).
NATIVE_OPB ?= 16
ifeq ($(NATIVE_OPB),8)
ENJ_CFLAGS += -DNM_OPB_WORDS=PVR_BINSIZE_8
ENJ_BASENAME := $(ENJ_BASENAME)-opb8
else ifneq ($(NATIVE_OPB),16)
$(error NATIVE_OPB must be 8 or 16)
endif
# NATIVE_DIAG_OPB=16|32: words of an object pointer block of every list, overriding NATIVE_OPB (measurement).
ifdef NATIVE_DIAG_OPB
ifeq ($(filter $(NATIVE_DIAG_OPB),16 32),)
$(error NATIVE_DIAG_OPB must be 16 or 32)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-opb$(NATIVE_DIAG_OPB)
ENJ_CFLAGS += -DNM_DIAG_OPB=$(NATIVE_DIAG_OPB)
endif
# NATIVE_DIAG_ROAD_DEPTH=<factor>: depth written for the road-surface class relative to true 1/z, the same for every
#   camera (the build's own is the race view's, h/(h+0.6) of its eye height h, and 0.5 for the other cameras:
#   render/models/model_renderer.h). 1.0 draws the road at its true depth: cars are cut by it.
ifdef NATIVE_DIAG_ROAD_DEPTH
ENJ_BASENAME := $(ENJ_BASENAME)-roaddepth$(NATIVE_DIAG_ROAD_DEPTH)
ENJ_CFLAGS += -DNM_ROAD_DEPTH_SCALE=$(NATIVE_DIAG_ROAD_DEPTH)f -DNM_ROAD_DEPTH_FIXED=1
endif
# NATIVE_DIAG_FILL_PROBE=pt|tr|presort|add: not an effect, a fill probe (code/render/scene/practice.c): while the race
#   runs, two layers of a 256x256-pixel quad (131,072 pixel samples a frame, the arcade's average tyre-smoke frame) on
#   the punch-through list (pt), the translucent list with hardware auto-sort (tr), the translucent list pre-sorted
#   (presort), or pre-sorted, untextured and additive (add).
ifdef NATIVE_DIAG_FILL_PROBE
ifdef NATIVE_DIAG_LISTS
$(error NATIVE_DIAG_FILL_PROBE needs the lists NATIVE_DIAG_LISTS closes)
endif
ifeq ($(NATIVE_DIAG_FILL_PROBE),pt)
ENJ_CFLAGS += -DNM_DIAG_FILL_PROBE=1
else ifeq ($(NATIVE_DIAG_FILL_PROBE),tr)
ENJ_CFLAGS += -DNM_DIAG_FILL_PROBE=2
else ifeq ($(NATIVE_DIAG_FILL_PROBE),presort)
ENJ_CFLAGS += -DNM_DIAG_FILL_PROBE=3
else ifeq ($(NATIVE_DIAG_FILL_PROBE),add)
ENJ_CFLAGS += -DNM_DIAG_FILL_PROBE=4
else
$(error NATIVE_DIAG_FILL_PROBE must be pt, tr, presort or add)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-fillprobe-$(NATIVE_DIAG_FILL_PROBE)
endif
ifdef NATIVE_HEAP_TRACE
# Diagnostic (include/heap.h): prints every named allocation of 16 KiB or more with its address.
ENJ_BASENAME := $(ENJ_BASENAME)-heap-trace
ENJ_CFLAGS += -DNATIVE_HEAP_TRACE
endif
ifdef NATIVE_SOUND_LOG
# Diagnostic (code/audio/sound.c): a line for every music request, start and end.
ENJ_BASENAME := $(ENJ_BASENAME)-soundlog
ENJ_CFLAGS += -DNATIVE_SOUND_LOG
endif
ifdef NATIVE_STAGE_TRACE
# Diagnostic, NATIVE_STAGE_TRACE=<race tick>: from that tick until 40 updates into
# the podium scene, prints each step of the updater and of the list callbacks
# (code/practice.c), so a freeze names its place. Not a game build.
ifdef NATIVE_SAMPLE_PROFILE
$(error NATIVE_STAGE_TRACE and NATIVE_SAMPLE_PROFILE both use the frame-end callback)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-stage-trace
ENJ_CFLAGS += -DNATIVE_STAGE_TRACE=$(NATIVE_STAGE_TRACE)
endif
ifeq ($(NATIVE_TRANSMISSION),manual)
# The race's transmission (501464) until a native transmission select exists: manual by default; holding X at
# the START that begins a race picks the other one (code/practice.c). A race script header decides on its own.
ENJ_BASENAME := $(ENJ_BASENAME)-manual
ENJ_CFLAGS += -DNATIVE_TRANSMISSION_MANUAL
else ifneq ($(NATIVE_TRANSMISSION),)
ifneq ($(NATIVE_TRANSMISSION),automatic)
$(error NATIVE_TRANSMISSION is manual or automatic)
endif
endif
ifdef NATIVE_RACE_CHECKPOINTS
# Prints race state at fixed ticks after shutdown, for comparison with the host harness.
ENJ_CFLAGS += -DNATIVE_RACE_CHECKPOINTS
endif
ifdef NATIVE_RACE_SCRIPT
# Diagnostic replay of a scripted race (tests/trace_race_native.c --script-header):
# pad input is ignored and the run ends at the result handoff. Not a game build.
# Per tick: steer, pedals and edges (bit0/1 pedal rising, bit2 START rising: the
# slot machine and the off-road recovery).
ifndef NATIVE_RACE_SCRIPT_DIR
$(error NATIVE_RACE_SCRIPT requires NATIVE_RACE_SCRIPT_DIR with a generated race_script_data.h)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-race-script
ENJ_CFLAGS += -DNATIVE_RACE_SCRIPT -I$(abspath $(NATIVE_RACE_SCRIPT_DIR))
endif
# NATIVE_PAD_MONKEY=1 (test build; ELF suffix -monkey, -monkeyN with NATIVE_PAD_MONKEY_FRAMES=N): a scripted pad on
#   port A in place of the real one (tests/pad_monkey.c), for an unattended console run through several games on the
#   production paths (attract, credit, select, race, results, name entry, attract again): the accelerator held with
#   short releases (the select screens confirm on the trigger's edge), the stick swinging slowly from lock to lock
#   (walls and traffic), START for four frames every 600 frames or so with a drifting period (a credit in the
#   cinematic, the title or the V.R. demo; a slot reel stop in a race), the left trigger never (the settings chord),
#   no other button (A and X shift, B resets the session, Y and the D-pad pick views). enDjinn's pad refresh at the
#   top of every frame (enj_ctrl_map_states) steps the script, so every reader of a frame sees one state and one
#   edge; it and enj_ctrl_get_states are wrapped at link time (enDjinn and the product's sources are unchanged).
#   Both units are built outside the LTO unit: whole-program LTO inlines enj_ctrl_get_states (one load) into every
#   reader and resolves the engine's own calls, and the link's --wrap then sees no call to redirect (a wrap of a
#   symbol in the LTO unit fails silently; KOS's pvr_*/fs_* wraps above work because KOS is outside it). After
#   NATIVE_PAD_MONKEY_FRAMES frames (default 60000, 17 minutes: three to five games by the race's time-out) the pad
#   is released, a line printed and the run returns to the loader.
#   NATIVE_PAD_MONKEY_SCHEDULE=2 (suffix -schedule2): the credits pressed a fixed number of frames into the cinematic,
#   the V.R. demo and the title in turn (schedule 1's all land in the cinematic), and the select scripted: the stick's
#   excursions pick the course slot (Beginner, Advanced, Expert, Advanced in turn) and every other game is manual
#   (tests/pad_monkey.c).
ifeq ($(NATIVE_PAD_MONKEY),1)
ifdef NATIVE_RACE_SCRIPT
$(error NATIVE_PAD_MONKEY drives the pad, which a NATIVE_RACE_SCRIPT run ignores)
endif
ifdef NATIVE_DRIVING_TEST
$(error NATIVE_PAD_MONKEY drives the pad, which NATIVE_DRIVING_TEST does not read)
endif
NATIVE_PAD_MONKEY_SCHEDULE ?= 1
ENJ_BASENAME := $(ENJ_BASENAME)-monkey$(NATIVE_PAD_MONKEY_FRAMES)$(if $(filter-out 1,$(NATIVE_PAD_MONKEY_SCHEDULE)),-schedule$(NATIVE_PAD_MONKEY_SCHEDULE))
OBJS += $(ENJ_BUILDDIR)/tests/pad_monkey.o
$(ENJ_BUILDDIR)/tests/pad_monkey.o: DEFINES += -fno-lto $(if $(NATIVE_PAD_MONKEY_FRAMES),-DNATIVE_PAD_MONKEY_FRAMES=$(NATIVE_PAD_MONKEY_FRAMES)) \
  -DNATIVE_PAD_MONKEY_SCHEDULE=$(NATIVE_PAD_MONKEY_SCHEDULE)
$(ENJ_BUILDDIR)/enDjinn/code/enj_ctrl.o: DEFINES += -fno-lto
ENJ_LDFLAGS += -Wl,--wrap=enj_ctrl_get_states -Wl,--wrap=enj_ctrl_map_states
else ifneq ($(filter-out 0,$(NATIVE_PAD_MONKEY)),)
$(error NATIVE_PAD_MONKEY must be 0 or 1)
endif
ifdef NATIVE_GAMEPLAY_SH4ZAM
# Timing comparison only: approximate SH4ZAM divide/length in car state.
ENJ_CFLAGS += -DNM_GAMEPLAY_SH4ZAM
endif
# Mip D adjust (the TSP's level-of-detail multiplier, quarter steps 1..15; model_renderer.c). Only textures with a
# mip chain are affected, so only an asset set with chains (ENJ_ROMDIR) changes the picture.
# NATIVE_MIP_CLASSES=1 (default since 14 October 2026): per class, for every texture with a chain: the road-surface
#   class and the grandstand composites NATIVE_MIP_BIAS_SURFACE (default 2 = 0.5), the catch-fence stipple 4 (1.0),
#   everything else, cars and punch-through cutouts included, NATIVE_MIP_BIAS (default 4 = 1.0).
# NATIVE_MIP_CLASSES=0: the rule before: opaque scenery 1 (0.25), cars and punch-through 4 (1.0); NATIVE_MIP_BIAS
#   sets every opaque polygon, cars and scenery alike (the ELFs of build/dc-native-mipmaps.ds44Xc).
ifdef NATIVE_MIP_CLASSES
ifeq ($(NATIVE_MIP_CLASSES),0)
ENJ_BASENAME := $(ENJ_BASENAME)-nomipclasses
ENJ_CFLAGS += -DNM_MIP_CLASSES=0
ifdef NATIVE_MIP_BIAS_SURFACE
$(error NATIVE_MIP_BIAS_SURFACE needs NATIVE_MIP_CLASSES=1)
endif
else ifneq ($(NATIVE_MIP_CLASSES),1)
$(error NATIVE_MIP_CLASSES must be 0 or 1)
endif
endif
ifdef NATIVE_MIP_BIAS
ifeq ($(filter $(NATIVE_MIP_BIAS),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15),)
$(error NATIVE_MIP_BIAS must be 1..15 quarter steps)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-mipbias$(NATIVE_MIP_BIAS)
ENJ_CFLAGS += -DNM_MIP_BIAS=$(NATIVE_MIP_BIAS)
endif
ifdef NATIVE_MIP_BIAS_SURFACE
ifeq ($(filter $(NATIVE_MIP_BIAS_SURFACE),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15),)
$(error NATIVE_MIP_BIAS_SURFACE must be 1..15 quarter steps)
endif
ENJ_BASENAME := $(ENJ_BASENAME)-mipsurf$(NATIVE_MIP_BIAS_SURFACE)
ENJ_CFLAGS += -DNM_MIP_BIAS_SURFACE=$(NATIVE_MIP_BIAS_SURFACE)
endif
# NATIVE_SUPERSAMPLE_SURFACE=1 (measurement option): TSP super-sample texture bit on the road-surface class
#   (model_renderer.c): four texels averaged per pixel against grazing-angle blur and crawl, three to four times the
#   texture cost of those pixels.
ifeq ($(NATIVE_SUPERSAMPLE_SURFACE),1)
ENJ_BASENAME := $(ENJ_BASENAME)-sssurface
ENJ_CFLAGS += -DNM_SUPERSAMPLE_SURFACE=1
else ifneq ($(filter-out 0,$(NATIVE_SUPERSAMPLE_SURFACE)),)
$(error NATIVE_SUPERSAMPLE_SURFACE must be 0 or 1)
endif
ifdef NATIVE_PODIUM_TEST
# Presentation smoke test only: skips the race and finish, proves nothing about winning.
ENJ_BASENAME := $(ENJ_BASENAME)-podium-test
ENJ_CFLAGS += -DNATIVE_PODIUM_TEST
endif
ifdef NATIVE_RACE_FINISH_TEST
ifndef RACE_ASSET_DIR
$(error NATIVE_RACE_FINISH_TEST requires the 40-car race assets)
endif
ENJ_CFLAGS += -DNATIVE_RACE_FINISH_TEST
endif
ENJ_DEFAULT_OPTLEVEL := 3
# finite.h first in every unit: exact bit tests replace the libgcc unordered
# compare behind isfinite/isnan, whatever order the headers arrive in.
ENJ_CFLAGS += $(KOS_CFLAGS) -fno-fast-math -ffp-contract=off -include $(abspath include/finite.h)
ENJ_CFLAGS += -I$(KOS_PORTS)/include/zlib
# NATIVE_SH4ZAM_ROOT: the SH4ZAM checkout (its include/, and lib/libsh4zam.a built from it) used instead of the one
#   installed in KOS's addons. Default: extern/sh4zam at the pinned release (v0.10.0, 70dbb17; platform/dreamcast-native/scripts/fetch_sh4zam.sh), since KOS's copy
#   is shared and can move. SHZ_SH4_DOT_FIPR: v0.10.0 computes shz_dot6f/shz_dot8f (and the squared magnitudes) with
#   scalar FPU code unless it is defined; the renderer's bounds (geometry.c) and face classes (model_submission.h)
#   mirror the FIPR form in their own asm, so the FIPR form stays. NATIVE_SH4ZAM_ROOT= (empty) links KOS's copy.
NATIVE_SH4ZAM_ROOT ?= $(abspath ../../extern/sh4zam)
ifneq ($(strip $(NATIVE_SH4ZAM_ROOT)),)
ifeq ($(wildcard $(NATIVE_SH4ZAM_ROOT)/lib/libsh4zam.a),)
$(error $(NATIVE_SH4ZAM_ROOT)/lib/libsh4zam.a is missing: run platform/dreamcast-native/scripts/fetch_sh4zam.sh (or set NATIVE_SH4ZAM_ROOT=))
endif
ENJ_CFLAGS += -I$(abspath $(NATIVE_SH4ZAM_ROOT))/include -DSHZ_SH4_DOT_FIPR
ENJ_LDLIBS += $(abspath $(NATIVE_SH4ZAM_ROOT))/lib/libsh4zam.a -lz
else
ENJ_LDLIBS += -lsh4zam -lz
endif
# Sound: AICAflow (include/audio/sound.h). platform/dreamcast-native/scripts/fetch_aicaflow.sh puts it into extern/aicaflow (AICAFLOW_ROOT selects
#   another checkout); the Makefile builds its SH4 library there, code/audio/sound.c embeds its ARM7 firmware, and KOS's
#   own sound startup (enDjinn calls snd_init) is wrapped away so KOS's driver never loads. Music: ASSET_ROOT/music from
#   recipes/build_music.sh; without it the game is silent.
# NATIVE_SOUND_TEST=NN: recording NN plays from boot, repeating (ELF suffix -soundNN): a playback check, not the
#   game's music.
ifndef NATIVE_TRANSFER_BENCH
AICAFLOW_ROOT ?= $(abspath ../../extern/aicaflow)
ifeq ($(wildcard $(AICAFLOW_ROOT)/driver/sh4/Makefile),)
$(error No AICAflow at $(AICAFLOW_ROOT): run platform/dreamcast-native/scripts/fetch_aicaflow.sh or set AICAFLOW_ROOT)
endif
AICAFLOW_LIB := $(AICAFLOW_ROOT)/driver/sh4/libaicaflow_host.a
ENJ_CFLAGS += -I$(AICAFLOW_ROOT)/driver/include -I$(AICAFLOW_ROOT)/driver/format/include \
              -I$(AICAFLOW_ROOT)/driver/sh4/include --embed-dir=$(AICAFLOW_ROOT)/firmware
ENJ_LDLIBS += $(AICAFLOW_LIB)
ENJ_LDFLAGS += -Wl,--wrap=snd_init -Wl,--wrap=snd_shutdown
# NATIVE_SOUND=0: the same ELF without starting AICAflow (silent; suffix -nosound): an A/B of the sound work.
ifeq ($(NATIVE_SOUND),0)
ENJ_BASENAME := $(ENJ_BASENAME)-nosound
ENJ_CFLAGS += -DNM_SOUND_OFF
else ifneq ($(filter-out 1,$(NATIVE_SOUND)),)
$(error NATIVE_SOUND must be 0 or 1)
endif
# Music loads (include/audio/sound.h): read in the idle waits for the PVR, one step a frame (code/audio/idle_wait.c).
#   NATIVE_MUSIC_LOAD=sync: each load in one update as before (suffix -musicsync), for an A/B of the drop log;
#   it reaches code/audio/sound.o only (Makefile).
ifeq ($(NATIVE_MUSIC_LOAD),sync)
ENJ_BASENAME := $(ENJ_BASENAME)-musicsync
NATIVE_SOUND_DEFINES += -DNM_SOUND_LOAD_SYNC
else ifneq ($(filter-out frames,$(NATIVE_MUSIC_LOAD)),)
$(error NATIVE_MUSIC_LOAD must be frames or sync)
endif
# NATIVE_SLOT_GLASS_DIR: the slot machine's glass (render/scene/slot_glass.c), the directory of slot_glass_data.h
#   (tools/prepare_world_objects.py glass; recipes: headers/slot-glass). Without it the glass is not drawn.
ifdef NATIVE_SLOT_GLASS_DIR
ENJ_CFLAGS += -I$(abspath $(NATIVE_SLOT_GLASS_DIR))
endif
# NATIVE_ATTRACT_2D_DIR: the attract's 2D layer (render/ui/attract_2d_render.c), the directory of attract_2d_data.h
#   (tools/prepare_attract_2d.py; recipes: headers/attract-2d, assets attract-2d/). Without it nothing is drawn.
ifdef NATIVE_ATTRACT_2D_DIR
ENJ_CFLAGS += -I$(abspath $(NATIVE_ATTRACT_2D_DIR))
endif
# NATIVE_PODIUM_SCREEN_DIR: the podium's 2D screen and photo (render/ui/podium_render.c), the directory of
#   podium_screen_data.h (tools/prepare_podium_screen.py; recipes: headers/podium-screen, assets podium/).
ifdef NATIVE_PODIUM_SCREEN_DIR
ENJ_CFLAGS += -I$(abspath $(NATIVE_PODIUM_SCREEN_DIR))
endif
ifdef NATIVE_SOUND_TEST
$(if $(shell [ "$(NATIVE_SOUND_TEST)" -ge 1 ] 2>/dev/null && [ "$(NATIVE_SOUND_TEST)" -le 45 ] 2>/dev/null && echo ok),,$(error NATIVE_SOUND_TEST must be a track number 1..45))
ENJ_BASENAME := $(ENJ_BASENAME)-sound$(NATIVE_SOUND_TEST)
ENJ_CFLAGS += -DNM_SOUND_TEST=$(patsubst 0%,%,$(NATIVE_SOUND_TEST))
endif
endif
$(if $(filter-out 0 1,$(NATIVE_SH4ZAM_GFX)),$(error NATIVE_SH4ZAM_GFX must be 0 or 1),$(if $(filter 0,$(NATIVE_SH4ZAM_GFX)),$(eval ENJ_BASENAME := $(ENJ_BASENAME)-noshzgfx)$(eval ENJ_CFLAGS += -DNM_SH4ZAM_GFX=0)))
# NATIVE_SUPERSAMPLE_FAR (default 1 with NATIVE_FILTER_ALL=1, else 0): TSP super-sample bit on the far level of the world
#   pack's "supersample" level-of-detail groups (tools/lod_groups.py: header word 1 mark 0x1000, model_renderer.c
#   compile_draw_header). A pack without such groups compiles the same headers. 0 draws both levels alike (A/B).
NATIVE_SUPERSAMPLE_FAR ?= $(if $(filter 1,$(NATIVE_FILTER_ALL)),1,0)
ifeq ($(NATIVE_SUPERSAMPLE_FAR),1)
ENJ_CFLAGS += -DNM_SUPERSAMPLE_FAR=1
else ifeq ($(NATIVE_SUPERSAMPLE_FAR),0)
$(if $(filter 1,$(NATIVE_FILTER_ALL)),$(eval ENJ_BASENAME := $(ENJ_BASENAME)-nossfar))
else
$(error NATIVE_SUPERSAMPLE_FAR must be 0 or 1)
endif
# NATIVE_FENCE_SWEEP (default 0 since 15 October, evening, the plain fence chosen; the C default of
#   model_submission.h is the same): 1 draws the catch fence's level-of-detail boundary as a depth plane, panels across
#   it clipped (model_submission.h, sq_sweep_select). It acts only on the fence groups of a world pack with a fence rule
#   (assets-ssfar4cf3 .. assets-ssfar4eu, three levels); the recommended assets-ssfar4f has none (the plain fence, every
#   panel at full density at every distance), so there it would do nothing. 0 switches the older sets' fence groups
#   panel by panel like every other group. ELF suffix -fencesweep when 1.
NATIVE_FENCE_SWEEP ?= 0
ifeq ($(NATIVE_FENCE_SWEEP),1)
ENJ_BASENAME := $(ENJ_BASENAME)-fencesweep
ENJ_CFLAGS += -DNM_FENCE_SWEEP=1
else ifneq ($(NATIVE_FENCE_SWEEP),0)
$(error NATIVE_FENCE_SWEEP must be 0 or 1)
endif
# NATIVE_SMALL_CAR_PX (default 12 since 15 October, the chosen default; 0: off): render-only level of detail for distant
# opponents (code/render/scene/practice.c, practice_drawn_level, whose C default is the same): an opponent whose body is
# under N output rows tall is drawn with the level-1 body and level 1's parts; the arcade's detail level and the
# drawn-car count the opponents read stay as computed. ELF suffix -smallcarN when on, -smallcaroff when 0.
NATIVE_SMALL_CAR_PX ?= 12
$(if $(shell [ "$(NATIVE_SMALL_CAR_PX)" -ge 0 ] 2>/dev/null && echo ok),,$(error NATIVE_SMALL_CAR_PX must be a whole number of rows))
ENJ_BASENAME := $(ENJ_BASENAME)-smallcar$(if $(filter 0,$(NATIVE_SMALL_CAR_PX)),off,$(NATIVE_SMALL_CAR_PX))
ENJ_CFLAGS += -DNM_SMALL_CAR_PX=$(NATIVE_SMALL_CAR_PX)
# NATIVE_COURSE (default 0, Beginner): the course the first race starts on, as the slot 501460 (0 Beginner,
# 1 Expert, 2 Advanced; include/course_assets.h). The course is a run-time choice (code/practice.c
# nm_practice_course_select, for the front end); this is only its value at boot. Every course's packs are
# loaded by name (courseN.dnc/.dnr/-route0..7.dnp/courseN.dnv, courseN-world.dnm/.dnt, courseN-sky.dns) and
# its tables come from the one race_start_data.h (prepare_intro.py --race-start --courses), which must hold
# the course. The attract shows course 0. ELF suffix -courseN.
NATIVE_COURSE ?= 0
$(if $(filter 0 1 2,$(NATIVE_COURSE)),,$(error NATIVE_COURSE must be 0, 1 or 2))
ifneq ($(NATIVE_COURSE),0)
ENJ_BASENAME := $(ENJ_BASENAME)-course$(NATIVE_COURSE)
ENJ_CFLAGS += -DNM_COURSE=$(NATIVE_COURSE)
endif
# NATIVE_GUARD_BAND (default 2048 since 15 October, the chosen default; the C default of model_submission.h is the same):
# logical pixels by which the guard view extends the screen on every side (SQ_GUARD_BAND): a face with a corner beyond it
# is clipped on the CPU, the PVR clips the rest at the screen edges. ELF suffix -guardN when not 2048.
NATIVE_GUARD_BAND ?= 2048
$(if $(shell [ "$(NATIVE_GUARD_BAND)" -ge 1 ] 2>/dev/null && echo ok),,$(error NATIVE_GUARD_BAND must be a whole number of logical pixels))
ifneq ($(NATIVE_GUARD_BAND),2048)
ENJ_BASENAME := $(ENJ_BASENAME)-guard$(NATIVE_GUARD_BAND)
endif
ENJ_CFLAGS += -DSQ_GUARD_BAND=$(NATIVE_GUARD_BAND).0f
# The pad's analogue steering (include/steer_curve.h, code/practice.c; pad only: scripted races inject their steer
# values and the arcade decoder and race after the table stay as they are). The stick's travel outside the dead
# zone is rescaled to 0..1, raised to the curve's exponent and mapped onto the wheel's full band, so full lock is at
# both ends of the stick and there is no step at the dead-zone edge.
#   NATIVE_STEER_CURVE (default 2.0): the exponent; 1.0 is linear with full lock, higher is finer near the centre
#     and steeper on the outer travel (4 is very flat near the centre). ELF suffix -steer<value> when not 2.0.
#   NATIVE_STEER_DEADZONE (default 12): stick counts (of 127) around the centre that read as straight ahead.
#     ELF suffix -steerdz<value> when not 12.
NATIVE_STEER_CURVE ?= 2.0
NATIVE_STEER_DEADZONE ?= 12
$(if $(shell [ "$(NATIVE_STEER_DEADZONE)" -ge 0 ] 2>/dev/null && [ "$(NATIVE_STEER_DEADZONE)" -le 126 ] && echo ok),,$(error NATIVE_STEER_DEADZONE must be a whole number 0..126))
$(if $(shell awk 'BEGIN{exit !("$(NATIVE_STEER_CURVE)"+0>0)}' && echo ok),,$(error NATIVE_STEER_CURVE must be a number above 0))
ifneq ($(NATIVE_STEER_CURVE),2.0)
ENJ_BASENAME := $(ENJ_BASENAME)-steer$(NATIVE_STEER_CURVE)
endif
ifneq ($(NATIVE_STEER_DEADZONE),12)
ENJ_BASENAME := $(ENJ_BASENAME)-steerdz$(NATIVE_STEER_DEADZONE)
endif
ENJ_CFLAGS += -DNM_STEER_CURVE=$(NATIVE_STEER_CURVE) -DNM_STEER_DEADZONE=$(NATIVE_STEER_DEADZONE)
