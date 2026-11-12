#!/usr/bin/env zsh
# Everything the PC build needs from your own ROM set, in one directory.
#
#   zsh platform/pc-native/recipes/build_pc_assets.sh ROMSET.zip OUTDIR [build_assets.sh options...]
#
# ROMSET.zip  MAME's daytona93 set (either layout of its sound program: epr-16720/16721 or epr-16489/16490).
# OUTDIR      output directory (keep it under the ignored build/).
#
# DAYTONA_COUNTRY=usa|jpn|export: the arcade's region (its COUNTRY setting) for the select screens and the HUD
# (usa, the default: English texts and the speed in mph; jpn: the factory board's Japanese screens).
#
# Steps:
#   1. platform/dreamcast-native/recipes/build_assets.sh ROMSET.zip OUTDIR --no-music: the asset set
#      (OUTDIR/assets-ssfar4f), the generated headers (OUTDIR/headers) and their checks, the gameplay
#      lockstep against the recompiled arcade game included. The PC build plays the music from the
#      sound program itself, so vgmrips' pack is not used.
#   2. tools/extract_sound_roms.py: OUTDIR/assets-ssfar4f/sound/{sound_program,pcm1,pcm2}.bin, which
#      platform/pc-native/audio/pc_sound.cpp hands to the arcade sound engine.
#   3. tools/prepare_horse.py: the Saturn version's secret horse, the arcade's own Expert horse (extracted from
#      the ROM into Expert's world pack by step 1) copied into the car pack: car.dnm, car.dnt, car-horse.dnb.
#   4. With DAYTONA_SATURN_FILES=DIR (the files of your Saturn Daytona USA disc: SOUNDS.BIN, GAMED.BIN):
#      tools/extract_saturn_horse.py, the horse's hoof sounds (OUTDIR/assets-ssfar4f/sound/horse-hoof*.wav), and
#      tools/prepare_saturn_menus.py, the Saturn game's front-end art (OUTDIR/assets-ssfar4f/saturn-menu/: the menus).
#   5. With DAYTONA_CCE_DIR=DIR (the DAYTONA folder of your Daytona USA Circuit Edition disc: COURSEn.MDL/.TEX,
#      CSn_COL.BIN, CSn_BLK.BIN) and DAYTONA_CCE_REPO=DIR (a checkout of AJBats/daytona-cce95, for its
#      tools/col_generate.py): tools/prepare_cce_course.py, the Circuit Edition's National Park Speedway (slot 3) and
#      Desert City (slot 4): their HUD maps (made before step 1, which draws them into the HUD pack: --hud-map) and
#      their world, contact, road, route and visibility packs, and tools/prepare_select_cce.py their course select
#      panels and maps (the select screen then shows all five courses).
#   6. With DAYTONA_REVA_ROM=FILE (MAME's daytona set, Revision A, as a zip): tools/prepare_reva_start.py, Revision
#      A's race-start camera (OUTDIR/headers/pc/reva_start_camera.h, compiled in when there); without it the race
#      start keeps Deluxe '93's camera.
#   7. capture_flags.sh: OUTDIR/pc-flags.cmake, the game's build options for CMake.
#
# Then: cmake -S platform/pc-native -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAYTONA_ASSETS=OUTDIR
#       cmake --build build/pc && build/pc/daytona OUTDIR/assets-ssfar4f
emulate -L zsh
setopt err_exit pipe_fail no_unset
SELF=${0:A}
PC=${SELF:h:h}
DN=${PC:h}/dreamcast-native
(( $# >= 2 )) || { sed -n '2,36p' $SELF | sed 's/^# \{0,1\}//'; exit 2; }
ROM=${1:A}; OUT=${2:A}; shift 2
# The Circuit Edition courses: CCE's course number, the PC slot, its laps.
CCE_COURSES=(3 3 6  4 4 4)
HUDMAPS=()
if [[ -n ${DAYTONA_CCE_DIR:-} && -n ${DAYTONA_CCE_REPO:-} ]]; then
  CCE=${DAYTONA_CCE_DIR:A}; CCE_REPO=${DAYTONA_CCE_REPO:A}
  mkdir -p $OUT/work/cce
  for course slot laps in $CCE_COURSES; do
    python3 -I $PC/tools/prepare_cce_course.py $CCE $course $slot --contact $CCE_REPO --map $OUT/work/cce/course$slot-map.json
    HUDMAPS+=(--hud-map $slot $OUT/work/cce/course$slot-map.json)
  done
else
  print -r -- "No DAYTONA_CCE_DIR and DAYTONA_CCE_REPO: the Circuit Edition courses are left out"
fi
zsh $DN/recipes/build_assets.sh $ROM $OUT --no-music --country ${DAYTONA_COUNTRY:-usa} $HUDMAPS "$@"
SET=$OUT/assets-ssfar4f
[[ -f $SET/drive.dnd ]] || { print -u2 -r -- "no asset set at $SET"; exit 1; }
python3 $DN/tools/extract_sound_roms.py $ROM $SET/sound
python3 $PC/tools/prepare_horse.py $SET
if [[ -n ${DAYTONA_SATURN_FILES:-} ]]; then
  python3 $PC/tools/extract_saturn_horse.py ${DAYTONA_SATURN_FILES:A} $SET
  python3 -I $PC/tools/prepare_saturn_menus.py ${DAYTONA_SATURN_FILES:A} $SET
else
  print -r -- "No DAYTONA_SATURN_FILES: the horse gallops without its Saturn hoof sounds"
fi
if (( $#HUDMAPS )); then
  for course slot laps in $CCE_COURSES; do
    python3 -I $PC/tools/prepare_cce_course.py $CCE $course $slot $SET --contact $CCE_REPO --road --laps $laps
    cp $SET/course2-sky.dns $SET/course$slot-sky.dns   # CCE's sky (CSn.BG) is not converted yet: Advanced's
  done
  python3 -I $PC/tools/prepare_select_cce.py $OUT/work/export $CCE $OUT/work/cce $SET
fi
if [[ -n ${DAYTONA_REVA_ROM:-} ]]; then
  mkdir -p $OUT/headers/pc
  python3 -I $PC/tools/prepare_reva_start.py ${DAYTONA_REVA_ROM:A} $OUT/headers/pc/reva_start_camera.h
else
  rm -f $OUT/headers/pc/reva_start_camera.h
  print -r -- "No DAYTONA_REVA_ROM: the race start camera is Deluxe '93's (Revision A's needs MAME's daytona set)"
fi
sh $PC/recipes/capture_flags.sh $OUT $OUT/pc-flags.cmake
print -r -- "PC assets ready: $SET"
print -r -- "  cmake -S platform/pc-native -B build/pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAYTONA_ASSETS=$OUT"
print -r -- "  cmake --build build/pc && build/pc/daytona $SET"
