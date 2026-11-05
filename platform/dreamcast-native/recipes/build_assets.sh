#!/usr/bin/env zsh
# Rebuild every ROM-derived input of the recommended Dreamcast build from your own ROM set.
#
#   zsh platform/dreamcast-native/recipes/build_assets.sh ROM OUTDIR (--music ARCHIVE | --no-music) [--elf] [--check] [--variants LIST]
#                                                [--host-build DIR] [--keep] [--jobs N] [--face-group N]
#
# ROM      daytona93 ROM set (.zip or .7z, MAME layout; the sound ROMs are not needed), or a directory that already
#          holds the imported images (program.bin, main_data.bin, ... as written by m2import_silent or m2import).
# OUTDIR   new or partly built output directory (each finished step leaves OUTDIR/stamps/STEP; a rerun skips those).
# --music ARCHIVE  the soundtrack: vgmrips' Daytona USA pack (not in the ROM set; a browser download, see build_music.sh,
#          which this runs as the step "music", about 15 s; it needs platform/dreamcast-native/scripts/fetch_aicaflow.sh run once). Every asset
#          set gets music/tNN.afb and tNN.afx of the 45 tracks, the files the ELF reads. One of --music and --no-music
#          is required, unless an earlier run of OUTDIR finished the step "music".
# --no-music  asset sets without music/ (the game runs silent); their music rows are reported as one line.
#          The sound effects (the step "sfx", build_sfx.sh, about 30 s) come from the ROM set's sound ROMs: every
#          set gets sfx/race.afb and race.dsx when ROM is a .zip; from a directory of images they are left out.
# --elf    also build the default stage 6 ELFs (scripted fast race with render statistics and drop log; the same for
#          the manual race; controller).
# --check  with --elf: run the Dreamcast `check` target as well (host tests and the heap budget of the asset set).
# --variants LIST  comma-separated candidate sets not chosen yet: ssfar4d (ssfar4c + Sonic rock and
#          rock moss baked into opaque faces), ssfar4bf3 (ssfar4b + the three-level catch fence, with mip chains on
#          the forest floor and glazing; its world packs are always built, for ssfar4cf3); ssfar4cf0 is a benchmark,
#          not a candidate: ssfar4cf3 with no fence rule (every stipple panel at full density at every distance);
#          ssfar4cf3r, ssfar4eu and ssfar4f (the recommended set since 15 October, evening) are built always and
#          accepted here for older command lines.
# --host-build DIR  use an existing CMake build of this checkout, configured with -DM2_DREAMCAST_NATIVE=ON, for m2segment
#          and the pack checkers instead of configuring and building OUTDIR/host. Its m2segment must know --dump-video.
# --keep   keep the three large traces (HUD, race and manual race reference, about 520 MB) after their consumers ran.
# --jobs N parallel jobs for CMake and make (default: number of CPUs).
# --face-group N  faces per face group of the world packs (prepare_models.py --face-group-size; default 8 since
#          15 October: hardware registration -0.40 ms against 32, 16 and 4 measured slower). A measurement option:
#          other sizes do not match expected.sha256.
#
# Needs: this repository, CMake and a C++20 compiler (the host tools), python3 with numpy and Pillow, the KOS/enDjinn
# Dreamcast toolchain (pvrtex for the sky, title and finish textures; sh-elf and make for --elf), found through
# ENDJINN_ROOT (default: extern/endjinn, fetched by platform/dreamcast-native/scripts/fetch_endjinn.sh; its environ.sh is sourced); for the music,
# AICAflow and AICAforge at their pins in extern/aicaflow (platform/dreamcast-native/scripts/fetch_aicaflow.sh), zlib headers and unzip.
# Nothing here reads any build/ directory of the repository: everything is made from the ROM (and the music archive)
# below OUTDIR.
#
# Outputs (ROM-derived: keep OUTDIR out of git; build/ is ignored):
#   OUTDIR/assets-ssfar4f/   the recommended asset set (ENJ_ROMDIR at build time, -m at upload; chosen 15
#                            October, evening): ssfar4eu with the plain catch fence as the arcade defines it (no fence
#                            rule, specs/ssfar4f.json: every stipple panel at full density at every distance); since 16
#                            October the infield grass 6924 and the aprons 6664 are not super-sampled (rule "verges":
#                            the edge strip 6686 only); since 17 October the slot machine's spinning reels are (rule
#                            "reels")
#   OUTDIR/assets-ssfar4eu/  the set before it: ssfar4e + every sign of the sign sheet super-sampled
#                            (specs/ssfar4eu.json) and no mip chain on any super-sampled sign word; three-level fence
#   OUTDIR/assets-ssfar4e/   the set before that: ssfar4cf3r with the rubber streaks kept on or above the road and lifted
#                            2 layers instead of 240 (the cars' shadows fall on them) and the readable signs
#                            super-sampled beyond 3x their texel distance
#   OUTDIR/assets-ssfar4cf3r/ the set before it: ssfar4cf3 with opaque cores and trimmed punch-through fringes on Sonic
#                            and the rock ivy (chosen 15 October)
#   OUTDIR/assets-ssfar4cf3/ the set before it: ssfar4c with the three-level catch fence (dense, quarter stipple,
#                            nothing; chosen 14 October)
#   OUTDIR/assets-ssfar4c/   ssfar4b without mip chains on the forest floor and the stand glazing
#   OUTDIR/assets-ssfar4b/   the set before that; OUTDIR/assets-VARIANT/ for --variants
#   OUTDIR/assets-*/music/   in every set (not with --no-music): tNN.afb, tNN.afx of build_music.sh's defaults
#   OUTDIR/assets-*/sfx/     in every set (ROM a .zip): race.afb, race.dsx, voices.pak of build_sfx.sh (work/sfx-out: all)
#   OUTDIR/headers/*         the generated header directories the ELF build needs; OUTDIR/make-vars.txt lists them
#   OUTDIR/elf/{stats,stats-manual,controller}/  with --elf: bin/*.elf, loaded.sha256 (every loadable section)
#   OUTDIR/outputs.sha256    sha256 of every output; compared with recipes/expected.sha256 at the end
# See platform/dreamcast-native/recipes/README.md for the dependency graph.
emulate -L zsh
setopt err_exit pipe_fail no_unset
zmodload zsh/datetime

RC=${0:A:h}
REPO=${RC:h:h:h}
DN=$REPO/platform/dreamcast-native
T=$DN/tools

usage() { sed -n '2,58p' $RC/build_assets.sh | sed 's/^# \{0,1\}//'; exit 2; }
(( $# >= 2 )) || usage
ROM=${1:A}; OUT=${2:A}; shift 2
ELF=0; CHECK=0; KEEP=0; HOSTBUILD=""; MUSIC=""; NOMUSIC=0; VARIANTS=(); FACE_GROUP=8; JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
while (( $# )); do
  case $1 in
    --elf) ELF=1;;
    --music) MUSIC=${2:A}; shift;;
    --no-music) NOMUSIC=1;;
    --check) CHECK=1;;
    --keep) KEEP=1;;
    --host-build) HOSTBUILD=${2:A}; shift;;
    --jobs) JOBS=$2; shift;;
    --face-group) FACE_GROUP=$2; shift;;
    --variants) VARIANTS=(${(s:,:)2}); shift;;
    *) usage;;
  esac
  shift
done
for v in $VARIANTS; do [[ $v == (ssfar4d|ssfar4bf3|ssfar4cf0|ssfar4cf3r|ssfar4eu|ssfar4f) ]] || { print -u2 -r -- "unknown variant $v (ssfar4d, ssfar4bf3, ssfar4cf0, ssfar4cf3r, ssfar4eu, ssfar4f)"; exit 2; }; done
[[ -e $ROM ]] || { print -u2 -r -- "ROM $ROM not found"; exit 2; }
if (( NOMUSIC )); then
  [[ -z $MUSIC ]] || { print -u2 -r -- "--music and --no-music exclude each other"; exit 2; }
elif [[ -n $MUSIC ]]; then
  [[ -f $MUSIC ]] || { print -u2 -r -- "music archive $MUSIC not found (vgmrips' Daytona USA pack: see build_music.sh)"; exit 2; }
  [[ -f $REPO/extern/aicaflow/dependencies/AICAforge/Makefile ]] || { print -u2 -r -- "no AICAflow/AICAforge in extern/aicaflow: run platform/dreamcast-native/scripts/fetch_aicaflow.sh"; exit 2; }
elif [[ ! -e $OUT/stamps/music ]]; then
  print -u2 -r -- "the soundtrack is not in the ROM set: pass --music ARCHIVE (vgmrips' Daytona USA pack, a browser download:"
  print -u2 -r -- "see platform/dreamcast-native/recipes/build_music.sh) or --no-music for asset sets without music (the game runs silent)"
  exit 2
fi

ENDJINN_ROOT=${ENDJINN_ROOT:-$REPO/extern/endjinn}
if ! command -v pvrtex >/dev/null || (( ELF )); then
  [[ -r $ENDJINN_ROOT/environ.sh ]] || { print -u2 -r -- "need pvrtex/KOS: run platform/dreamcast-native/scripts/fetch_endjinn.sh or set ENDJINN_ROOT to an enDjinn checkout (environ.sh)"; exit 2; }
  set +u; source $ENDJINN_ROOT/environ.sh >/dev/null; set -u
fi
command -v pvrtex >/dev/null || { print -u2 -r -- "pvrtex not on PATH after sourcing $ENDJINN_ROOT/environ.sh"; exit 2; }
python3 -c 'import numpy, PIL' 2>/dev/null || { print -u2 -r -- "python3 needs numpy and Pillow"; exit 2; }

mkdir -p $OUT/stamps $OUT/logs $OUT/work $OUT/headers
X=$OUT/work/export; W=$OUT/work; HD=$OUT/headers
mkdir -p $X
START=$EPOCHREALTIME
INPUTS=$RC/inputs/inputs-finish-fast.txt
INPUTS_MANUAL=$RC/inputs/inputs-manual.txt
INPUTS2=$RC/inputs/inputs-advanced-timeout.txt   # Advanced (course slot 2): menu, then a run to the time-out
INPUTS1=$RC/inputs/inputs-expert-timeout.txt     # Expert (course slot 1): menu, then a run to the time-out
INPUTS2F=$RC/inputs/inputs-advanced-finish.txt   # Advanced: menu, 4 laps to 3rd place, podium, results
INPUTS1F=$RC/inputs/inputs-expert-finish.txt     # Expert: menu, 2 laps to 3rd place, podium, results

# step NAME FUNCTION: run FUNCTION once (stamp OUTDIR/stamps/NAME), output in OUTDIR/logs/NAME.log.
step() {
  local name=$1 fn=$2 t0=$EPOCHREALTIME
  if [[ -e $OUT/stamps/$name ]]; then print -r -- "-- $name: done earlier"; return 0; fi
  print -r -- "-- $name: ${(P)${:-DESC_$fn}:-$fn}"
  setopt local_options no_err_exit
  ( setopt err_exit; $fn ) > $OUT/logs/$name.log 2>&1
  local st=$?
  if (( st )); then
    print -u2 -r -- "!! $name failed; last lines of $OUT/logs/$name.log:"; tail -25 $OUT/logs/$name.log >&2; return $st
  fi
  : > $OUT/stamps/$name
  printf '   %s: %.0f s\n' $name $(( EPOCHREALTIME - t0 ))
}
typeset -A BG
# bg_step NAME FUNCTION: step in the background; bg_wait NAME... waits for its status file.
bg_step() {
  rm -f $OUT/logs/$1.status
  { local st=1; step $1 $2 && st=0; print $st > $OUT/logs/$1.status.tmp; mv $OUT/logs/$1.status.tmp $OUT/logs/$1.status } &
  BG[$1]=$!
}
bg_wait() {
  local n
  for n in "$@"; do
    while [[ ! -s $OUT/logs/$n.status ]]; do sleep 2; done
    [[ $(<$OUT/logs/$n.status) == 0 ]] || { print -u2 -r -- "!! $n failed"; exit 1; }
    rm -f $OUT/logs/$n.status; unset "BG[$n]"
  done
}
fresh() { rm -rf "$@"; }
# A failure stops the background traces too.
TRAPEXIT() { local p; for p in ${(v)BG}; do pkill -P $p 2>/dev/null; kill $p 2>/dev/null; done; return 0; }

# ---------------------------------------------------------------------------------------------------- music
# The soundtrack first: it needs no host tool, and a bad archive or a missing AICAflow fails in seconds.
MU=$W/music
DESC_music="soundtrack: build_music.sh on vgmrips' pack (AICAforge afx_vgm, PCM8) -> OUTDIR/work/music/music"
music() { fresh $MU; zsh $RC/build_music.sh $MUSIC $MU; }
if (( ! NOMUSIC )); then
  if [[ -n $MUSIC ]]; then step music music; else print -r -- "-- music: done earlier"; fi
  [[ -f $MU/music/t45.afx ]] || { print -u2 -r -- "no $MU/music: delete $OUT/stamps/music and pass --music ARCHIVE"; exit 2; }
fi
# The race pieces of Advanced (t04, Let's Go Away) and Expert (t05, Sky High) do not fit the AICA RAM in PCM8 beside the
# effects' race bank (1,616,201 and 1,818,799 bytes of bank and flow against 1,586,784 left of 2,072,768): an ADPCM
# copy of each in music/adpcm/, which code/audio/sound.c loads when the PCM8 one does not.
MA=$W/music-adpcm
DESC_music_adpcm="race music of Advanced and Expert in ADPCM (t04, t05; afx_vgm --adpcm) -> OUTDIR/work/music-adpcm/music"
music_adpcm() {
  local vgm=$REPO/extern/aicaflow/dependencies/AICAforge/build/afx_vgm validate=$REPO/extern/aicaflow/driver/build/afx_validate
  fresh $MA; mkdir -p $MA/vgz $MA/music
  unzip -q -j $MUSIC '04 *.vgz' '05 *.vgz' -d $MA/vgz
  local src
  for src in $MA/vgz/*.vgz; do
    local num=${${src:t:r}%% *}
    $vgm $src $MA/music/t$num.afx --adpcm
    $validate $MA/music/t$num.afx
  done
  rm -rf $MA/vgz $MA/music/*.afc(N) $MA/music/*.afv(N)
  ls -l $MA/music
}
if (( ! NOMUSIC )); then
  if [[ -n $MUSIC || -e $OUT/stamps/music_adpcm ]]; then step music_adpcm music_adpcm
  else print -r -- "-- music_adpcm: needs --music ARCHIVE (the asset sets get no music/adpcm/)"; fi
fi

# ---------------------------------------------------------------------------------------------------- sound effects
# From the ROM set's sound ROMs (a .zip; the imported images have none), beside the race music it must fit with.
SX=$W/sfx-out
DESC_sfx="sound effects: build_sfx.sh on the ROM set (native sequencer, AICAforge afx_vgm, ADPCM/PCM8) -> OUTDIR/work/sfx-out/sfx"
sfx() { local music=(); (( NOMUSIC )) || music=(--music $MU/music); fresh $SX; zsh $RC/build_sfx.sh $ROM $SX $music; }
SFX=0
if [[ $ROM == *.zip ]]; then
  [[ -f $REPO/extern/aicaflow/dependencies/AICAforge/Makefile ]] || { print -u2 -r -- "no AICAflow/AICAforge in extern/aicaflow: run platform/dreamcast-native/scripts/fetch_aicaflow.sh"; exit 2; }
  step sfx sfx; SFX=1
elif [[ -e $OUT/stamps/sfx ]]; then SFX=1
else print -r -- "-- sfx: left out (ROM is not a .zip: no sound ROMs); the sets get no sfx/ and the game plays no effects"
fi

# ---------------------------------------------------------------------------------------------------- host tools
if [[ -n $HOSTBUILD ]]; then H=$HOSTBUILD; else H=$OUT/host; fi
if [[ -d $ROM ]]; then ROMDIR=$ROM; else ROMDIR=$OUT/rom/daytona93; fi
M=$H/m2segment

DESC_host_configure="CMake configure of the host tools (OUTDIR/host)"
host_configure() {
  local gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
  cmake -S $REPO -B $H $gen -DCMAKE_BUILD_TYPE=Release -DM2_ROMSET=daytona93 -DM2_DREAMCAST_NATIVE=ON
  cmake --build $H --parallel $JOBS --target m2import_silent m2recomp m2tgprecomp
}
DESC_rom_import="m2import_silent ROM -> OUTDIR/rom/daytona93 (silent images)"
rom_import() { fresh $ROMDIR; $H/m2import_silent $ROM $ROMDIR; }
DESC_host_recompile="m2recomp/m2tgprecomp: the game's i960 and TGP code to C++ (OUTDIR/host/gen)"
host_recompile() {
  fresh $H/gen/daytona93 $H/gen/daytona93_tgp; mkdir -p $H/gen/daytona93 $H/gen/daytona93_tgp
  local hooks=(); [[ -f $REPO/seeds/daytona93_hooks.txt ]] && hooks=(--hooks seeds/daytona93_hooks.txt)
  ( cd $REPO && $H/m2recomp $ROMDIR/program.bin $H/gen/daytona93 --seeds seeds/daytona93.txt $hooks )
  $H/m2tgprecomp $ROMDIR/tgp_program.bin $H/gen/daytona93_tgp/tgp_gen.cpp
}
DESC_host_build="build m2segment, check_native_meshpack, check_native_material_pack"
host_build() { cmake -S $REPO -B $H; cmake --build $H --parallel $JOBS --target m2segment check_native_meshpack check_native_material_pack; }

if [[ -z $HOSTBUILD ]]; then step host_configure host_configure; fi
if [[ ! -d $ROM ]]; then step rom_import rom_import; fi
if [[ -z $HOSTBUILD ]]; then step host_recompile host_recompile; step host_build host_build; fi
for f in program main_data copro_data copro_tables polygons textures tgp_program; do
  [[ -f $ROMDIR/$f.bin ]] || { print -u2 -r -- "missing $ROMDIR/$f.bin"; exit 2; }
done
[[ "$($M 2>&1 || true)" == *--dump-video* ]] || { print -u2 -r -- "$M has no --dump-video: rebuild it from this checkout"; exit 2; }
R=$ROMDIR

# ---------------------------------------------------------------------------------------------------- m2segment exports
# The three long traces run in the background while everything else is made.
DESC_trace_hud="HUD reference trace: 13300 frames of the scripted fast race (--trace-hud)"
trace_hud() { fresh $X/hud-trace; $M $R 13300 --inputs $INPUTS --trace-hud $X/hud-trace 250; }
DESC_trace_race="race reference trace: 17500 frames of the scripted fast race (--trace-race, --trace-sound)"
trace_race() { rm -f $X/race.trace $X/race.sound; $M $R 17500 --inputs $INPUTS --trace-race $X/race.trace --trace-sound $X/race.sound; }
DESC_trace_manual="manual race reference trace: 17500 frames of the scripted manual race (--trace-race, --trace-sound)"
trace_manual() { rm -f $X/reference-manual.trace $X/reference-manual.sound; $M $R 17500 --inputs $INPUTS_MANUAL --trace-race $X/reference-manual.trace --trace-sound $X/reference-manual.sound; }
DESC_trace_race2="Advanced reference trace: 8000 frames of the time-out run (--trace-race, --trace-clock)"
trace_race2() { rm -f $X/race-advanced.trace $X/clock-advanced.txt
  $M $R 8000 --inputs $INPUTS2 --trace-race $X/race-advanced.trace --trace-clock $X/clock-advanced.txt; }
bg_step trace_hud trace_hud
bg_step trace_race trace_race
bg_step trace_manual trace_manual
bg_step trace_race2 trace_race2
DESC_trace_hud2="Advanced HUD trace: 8000 frames of the time-out run (--trace-hud), for its course map"
trace_hud2() { fresh $X/hud-trace2; $M $R 8000 --inputs $INPUTS2 --trace-hud $X/hud-trace2 250; }
bg_step trace_hud2 trace_hud2
DESC_trace_race1="Expert reference trace: 9100 frames of the time-out run (--trace-race, --trace-clock with Expert's actors)"
trace_race1() { rm -f $X/race-expert.trace $X/clock-expert.txt
  $M $R 9100 --inputs $INPUTS1 --trace-race $X/race-expert.trace --trace-clock $X/clock-expert.txt --trace-clock-expert; }
bg_step trace_race1 trace_race1
DESC_trace_hud1="Expert HUD trace: 9100 frames of the time-out run (--trace-hud), for its course map"
trace_hud1() { fresh $X/hud-trace1; $M $R 9100 --inputs $INPUTS1 --trace-hud $X/hud-trace1 250; }
bg_step trace_hud1 trace_hud1
DESC_trace_hud_lead="Expert HUD trace: 13000 frames of the finishing race, which leads from frame 11483 (--trace-hud), for the map's marker of the second car"
trace_hud_lead() { fresh $X/hud-trace-lead; $M $R 13000 --inputs $INPUTS1F --trace-hud $X/hud-trace-lead 250; }
bg_step trace_hud_lead trace_hud_lead
DESC_trace_race2f="Advanced finishing reference trace: 17800 frames of the 3rd-place race, podium and results (--trace-race, --trace-clock)"
trace_race2f() { rm -f $X/race-advanced-finish.trace $X/clock-advanced-finish.txt
  $M $R 17800 --inputs $INPUTS2F --trace-race $X/race-advanced-finish.trace --trace-clock $X/clock-advanced-finish.txt; }
bg_step trace_race2f trace_race2f
DESC_trace_race1f="Expert finishing reference trace: 19100 frames of the 3rd-place race, podium and results (--trace-race, --trace-clock with Expert's actors)"
trace_race1f() { rm -f $X/race-expert-finish.trace $X/clock-expert-finish.txt
  $M $R 19100 --inputs $INPUTS1F --trace-race $X/race-expert-finish.trace --trace-clock $X/clock-expert-finish.txt --trace-clock-expert; }
bg_step trace_race1f trace_race1f
DESC_trace_attract="attract reference trace: 13000 frames without input, the loop over courses 0, 2, 1, 0 with their V.R. demos (--trace-race --trace-attract, --trace-clock)"
trace_attract() { rm -f $X/attract.trace $X/clock-attract.txt
  $M $R 13000 --trace-race $X/attract.trace --trace-attract --trace-clock $X/clock-attract.txt --trace-clock-expert; }
bg_step trace_attract trace_attract
DESC_trace_attract_2d="attract 2D reference: the tile RAM of every 4th frame 180..12300 without input (--dump-2d: layer A, the line table; the pens of frame 180)"
trace_attract_2d() { fresh $X/attract-2d; mkdir -p $X/attract-2d; $M $R 12300 --dump-2d $X/attract-2d 180 12300 4 > /dev/null; }
bg_step trace_attract_2d trace_attract_2d

DESC_export_cells="scenery cells at frames 240 and 300 with the normal-race light (--export-native-cells --race-scenery-lights)"
export_cells() {
  fresh $X/cells240 $X/cells300
  $M $R 240 --export-native-cells $X/cells240 --race-scenery-lights
  $M $R 300 --export-native-cells $X/cells300 --race-scenery-lights
}
DESC_export_cars="40-car catalogue (528 frames, --export-native-race-cars)"
export_cars() { fresh $X/race-cars; $M $R 528 --export-native-race-cars $X/race-cars; }
DESC_export_player="player windows at frame 4080 of the fast race (--export-native-player-car)"
export_player() { fresh $X/player4080; $M $R 4080 --inputs $INPUTS --export-native-player-car $X/player4080; }
DESC_export_models="race frame 4080 models (--export-native-models) and the video tables (--dump-video)"
export_models() { fresh $X/models4080 $X/video; $M $R 4080 --inputs $INPUTS --export-native-models $X/models4080 --dump-video $X/video; }
DESC_export_ui="name, result and finish models, label colours, sky (frame 240), title (frame 3120)"
export_ui() {
  fresh $X/name-models $X/result-models $X/finish-models $X/result-colours.txt $X/name-colours.txt $X/sky $X/title
  $M $R 240 --export-native-name-models $X/name-models
  $M $R 240 --export-native-result-models $X/result-models
  $M $R 240 --export-native-finish-models $X/finish-models
  $M $R 240 --export-result-label-colors $X/result-colours.txt
  $M $R 240 --export-name-label-colors $X/name-colours.txt
  $M $R 240 --export-native-sky $X/sky
  $M $R 3120 --export-native-2d $X/title
}
DESC_export_select="course/transmission select and GENTLEMEN screens (--export-native-select: race_to_end, course_advanced, course_expert)"
export_select() {
  fresh $X/select-rte $X/select-adv $X/select-exp
  $M $R 2600 --inputs $REPO/scripts/inputs/race_to_end.txt --export-native-select $X/select-rte
  $M $R 1700 --inputs $RC/inputs/course_advanced.txt --export-native-select $X/select-adv
  $M $R 1740 --inputs $RC/inputs/course_expert.txt --export-native-select $X/select-exp
}
step export_cells export_cells
step export_cars export_cars
step export_player export_player
step export_models export_models
step export_ui export_ui
DESC_export_video2="textures of the Advanced race: video dump at frame 4000 of the time-out run (the world's, the car windows' tunnel texture)"
export_video2() { fresh $X/video2; $M $R 4000 --inputs $INPUTS2 --dump-video $X/video2; }
step export_video2 export_video2
DESC_export_video1="textures of the Expert race: video dump at frame 4000 of the time-out run"
export_video1() { fresh $X/video1; $M $R 4000 --inputs $INPUTS1 --dump-video $X/video1; }
step export_video1 export_video1
step export_select export_select
V=$X/video

# ---------------------------------------------------------------------------------------------------- ROM-only packs, headers
P=$W/packs
DESC_rom_packs="drive.dnd, course0.dnc/.dnr, eight routes, scenery.dnv (prepare_drive/contact/road/path/visibility)"
rom_packs() {
  fresh $P; mkdir -p $P
  python3 $T/prepare_drive.py $R $P/drive.dnd
  python3 $T/prepare_contact.py $R 0 $P/course0.dnc
  python3 $T/prepare_road.py $R 0 $P/course0.dnr
  for r in 0 1 2 3 4 5 6 7; do python3 $T/prepare_path.py $R 0 $r $P/course0-route$r.dnp; done
  python3 $T/prepare_visibility.py $R 0 $X/cells240 $P/scenery.dnv
}
DESC_rom_headers="intro (40 cars), podium, race start, records, name entry, intents, result actor headers"
rom_headers() {
  fresh $W/intro $HD/podium-data $HD/race-start-data $HD/record-data $HD/intent-data $HD/result-data $HD/name/name_entry_data.h
  mkdir -p $W/intro $HD/podium-data $HD/race-start-data $HD/record-data $HD/intent-data $HD/result-data $HD/name
  python3 $T/prepare_intro.py $R $W/intro/intro_drive_data.h --race-cars
  python3 $T/prepare_intro.py $R $HD/podium-data/podium_data.h --podium --courses 0,1,2
  # Every course's start camera and pit tables in one header (nm_race_course_starts): Beginner, Expert, Advanced.
  python3 $T/prepare_intro.py $R $HD/race-start-data/race_start_data.h --race-start --courses 0,1,2
  python3 $T/prepare_race_records.py $R/program.bin $HD/record-data/race_record_data.h --name-output $HD/name/name_entry_data.h
  python3 $T/prepare_race_intents.py $R/program.bin $HD/intent-data/race_intent_data.h
  python3 $T/prepare_result_actor.py $R/program.bin $HD/result-data/result_actor_data.h
}
step rom_packs rom_packs
step rom_headers rom_headers

# ---------------------------------------------------------------------------------------------------- Advanced (slot 2)
# The course byte 501460 is the ROM table slot: 0 Beginner, 1 Expert, 2 Advanced. Every file of another course
# carries its slot (include/course_assets.h); course 0 keeps its names.
P2=$W/packs-course2
DESC_course2_packs="Advanced (slot 2): course2.dnc/.dnr, eight routes, course2.dnv (attract cells, frame 3422)"
course2_packs() {
  fresh $P2 $X/cells2-3422; mkdir -p $P2
  python3 $T/prepare_contact.py $R 2 $P2/course2.dnc
  python3 $T/prepare_road.py $R 2 $P2/course2.dnr
  for r in 0 1 2 3 4 5 6 7; do python3 $T/prepare_path.py $R 2 $r $P2/course2-route$r.dnp; done
  # The attract loop shows slot 0 from frame 180, slot 2 from 3362, slot 1 from 6542: slot 2's cells 60 frames in.
  $M $R 3422 --export-native-cells $X/cells2-3422
  python3 $T/prepare_visibility.py $R 2 $X/cells2-3422 $P2/course2.dnv
  rm -rf $X/cells2-3422
}
step course2_packs course2_packs

# ---------------------------------------------------------------------------------------------------- Expert (slot 1)
P1=$W/packs-course1
DESC_course1_packs="Expert (slot 1): course1.dnc/.dnr, eight routes, course1.dnv (attract cells, frame 6602)"
course1_packs() {
  fresh $P1 $X/cells1-6602; mkdir -p $P1
  python3 $T/prepare_contact.py $R 1 $P1/course1.dnc
  python3 $T/prepare_road.py $R 1 $P1/course1.dnr
  for r in 0 1 2 3 4 5 6 7; do python3 $T/prepare_path.py $R 1 $r $P1/course1-route$r.dnp; done
  # The attract shows slot 1 from frame 6542: its cells 60 frames in.
  $M $R 6602 --export-native-cells $X/cells1-6602
  python3 $T/prepare_visibility.py $R 1 $X/cells1-6602 $P1/course1.dnv
  rm -rf $X/cells1-6602
}
step course1_packs course1_packs

# ---------------------------------------------------------------------------------------------------- cars
C=$W/car
DESC_car_packs="car pack: models, binding keys, player parts, car effects, pit crew, DNT6 palettes, face order, strips, damaged bodies"
car_packs() {
  fresh $C $HD/race-header; mkdir -p $C/in
  python3 $T/prepare_models.py $X/race-cars $C/in/car.dnm --race-cars $R --share-positions --align-tables
  python3 $T/prepare_indexed_materials.py $X/race-cars $C/car-indexed.dnt
  python3 $RC/key_pack.py dnt5 $C/car-indexed.dnt $C/in/car.dnt
  python3 $T/append_player_parts.py $C/in $X/player4080 $X/models4080 $R $V $C/out $W/intro/intro_drive_data.h $HD/race-header \
      --effects $X/race-cars --crew $X/race-cars --hoods $X/race-cars
  # The windows' tunnel texture of Advanced (tools/tunnel_windows.py): its bindings, and course 0's dump with its block.
  python3 $T/tunnel_windows.py bindings $C/out/car.dnt $C/car-keys.dnt
  python3 $T/tunnel_windows.py video $V $X/video2 $C/video
  python3 $T/prepare_palette_materials.py $C/car-keys.dnt $C/video $C/car.dnt --report $C/car-dnt.json > /dev/null
  python3 $T/prepare_face_order.py $C/out/car.dnm $C/car-sorted.dnm --intro-header $HD/race-header/intro_drive_data.h \
      --materials $C/car.dnt --order-map $C/car-order.json --match-decal-diagonals --strip-scope material > $C/car-sorted.json
  python3 $T/prepare_restrip.py $C/car-sorted.dnm $C/car.dnt $C/car.dnm --car-levels $HD/race-header/intro_drive_data.h \
      --keep-sources 0x8d9e42 0x8d7ab7 --report $C/car-strips.json --order-map $C/car-strips-order.json > /dev/null
  $H/check_native_material_pack $C/car.dnt --cpu-backed | tail -1
  $H/check_native_meshpack $C/car.dnm | tail -1
  python3 $T/prepare_damage_bodies.py $X/race-cars $R $C/car.dnm $C/car-damage.dnb > $C/car-damage.json
  python3 $T/prepare_damage_bodies.py $X/race-cars $R $C/car.dnm $C/car-hoods.dnb --hoods > $C/car-hoods.json
}
step car_packs car_packs

# ---------------------------------------------------------------------------------------------------- world
G=$W/world
DESC_world_base="world cells pack, binding keys, opaque-core specs (trees th2 near, far1 far)"
world_base() {
  fresh $G; mkdir -p $G/specs
  python3 $T/prepare_models.py $X/cells240 $G/world-cells.dnm --world-cells --face-group-size $FACE_GROUP --align-tables \
      --share-positions --collapse-flat-cutouts
  python3 $RC/key_pack.py cells $X/cells240 $RC/world-dnt4-resources.txt $G/world-keys.dnt
  cp $RC/specs/*.json $G/specs/
  python3 $T/opaque_cores.py $RC/opaque-cores/th2.json $V $G/world-cells.dnm $G/specs/near-th2.json > $G/design-th2.txt &
  python3 $T/opaque_cores.py $RC/opaque-cores/far1.json $V $G/world-cells.dnm $G/specs/far-far1.json > $G/design-far1.txt
  wait $!
}
DESC_world_objects="world objects: object lists, frame-300 export, sources, world-src.dnm, the slot machine glass header"
world_objects() {
  fresh $G/lists.txt $X/objects300 $G/sources $G/world-src.dnm $HD/slot-glass
  python3 $T/prepare_world_objects.py lists $R $G/lists.txt --extra-models $RC/world-object-models.txt
  $M $R 300 --export-native-objects $G/lists.txt $X/objects300
  python3 $T/prepare_world_objects.py sources $R $X/objects300 $X/cells300 $G/sources
  python3 $T/prepare_world_objects.py glass $R $X/objects300 $HD/slot-glass/slot_glass_data.h
  python3 $T/prepare_models.py $G/sources $G/world-src.dnm --world-cells --world-objects --face-group-size $FACE_GROUP --align-tables \
      --share-positions --collapse-flat-cutouts
}
# pack_world NAME SPEC MIPSKIP RESTRIP [face-order options for both meshes...]: packs/world-NAME{-ref.dnm,.dnm,-mip.dnt,-strips.dnm}
#   MIPSKIP: texture words without mip chains ("" none); RESTRIP: the DNT the strips are made with, plain (world-NAME.dnt,
#   no mip chains, also written) or mip.
pack_world() {
  local name=$1 lod=$2 mipskip=$3 restrip=$4; shift 4
  local common=(--materials $G/world-keys.dnt --strip-scope material --static-light -0.45 -0.89 -0.45
      --scenery-classes $DN/code/render/scene/track_surface.c --match-decal-diagonals --cross-mesh-decals --stipple-panels
      --flatten-stacks --flatten-limit 2 "$@")
  local p=$G/packs/world-$name q=$G/reports/world-$name
  rm -f $p.*(N) $p-*.*(N) $q.*(N) $q-*.*(N)
  python3 $T/prepare_face_order.py $G/world-src.dnm $p-ref.dnm $common --order-map $q-ref-order.json --composites $q-ref-composites.json > $q-ref.json
  python3 $T/prepare_face_order.py $G/world-src.dnm $p.dnm $common --order-map $q-order.json --composites $q-composites.json \
      --split-video $V --lod-groups $lod > $q.json
  local tx=(--mesh $p-ref.dnm --split-mesh $p.dnm --share $C/car.dnt --palette-banks 58 --vram-limit 3571712
      --composites $q-ref-composites.json --texture-fill $lod --add-mesh-keys)
  local mip=(--mipmaps-for all --mip-filter colour --mip-skip-surface --mip-skip-composites)
  [[ -n $mipskip ]] && mip+=(--mip-skip-textures $mipskip)
  [[ $restrip == plain ]] && python3 $T/prepare_palette_materials.py $G/world-keys.dnt $V $p.dnt $tx --report $q-dnt.json > /dev/null
  python3 $T/prepare_palette_materials.py $G/world-keys.dnt $V $p-mip.dnt $tx --report $q-mip-dnt.json $mip > /dev/null
  local sd=$p-mip.dnt; [[ $restrip == plain ]] && sd=$p.dnt
  python3 $T/prepare_restrip.py $p.dnm $sd $p-strips.dnm --rules exact --report $q-strips.json \
      --order-map $q-strips-order.json --pixel-faces $q-strips-pixels.txt > /dev/null
  for f in $p.dnm $p-ref.dnm $p-strips.dnm; do $H/check_native_meshpack $f | tail -1; done
  $H/check_native_material_pack $p-mip.dnt --cpu-backed | tail -1
}
DESC_world_ssfar4b="world pack ssfar4b: face order (reference and level of detail), DNT6 with mip chains, strips"
world_ssfar4b() { pack_world ssfar4b $G/specs/ssfar4b.json "" plain; }
# mip_skip_world NAME FROM SPEC [WORDS]: world-NAME-mip.dnt = FROM's packs with mip chains, none on the forest floor (6428)
#   and stand glazing (3858, 7064) nor on the texture words WORDS (comma-separated); world-NAME-strips.dnm = FROM's strips.
mip_skip_world() {
  local name=$1 lod=$3 skip=6428,3858,7064${4:+,$4} p=$G/packs/world-$2 q=$G/reports/world-$2
  python3 $T/prepare_palette_materials.py $G/world-keys.dnt $V $G/packs/world-$name-mip.dnt --mesh $p-ref.dnm \
      --split-mesh $p.dnm --share $C/car.dnt --palette-banks 58 --vram-limit 3571712 --composites $q-ref-composites.json \
      --texture-fill $lod --add-mesh-keys --report $G/reports/world-$name-mip-dnt.json --mipmaps-for all --mip-filter colour \
      --mip-skip-surface --mip-skip-composites --mip-skip-textures $skip > /dev/null
  cp $p-strips.dnm $G/packs/world-$name-strips.dnm
  $H/check_native_material_pack $G/packs/world-$name-mip.dnt --cpu-backed | tail -1
}
DESC_world_ssfar4c="world DNT6 ssfar4c: ssfar4b without mip chains on the forest floor (6428) and stand glazing (3858, 7064)"
world_ssfar4c() { mip_skip_world ssfar4c ssfar4b $G/specs/ssfar4b.json; }
DESC_world_ssfar4bf3="world pack ssfar4bf3: ssfar4b with the three-level catch fence (specs/ssfar4bf3.json)"
world_ssfar4bf3() { pack_world ssfar4bf3 $G/specs/ssfar4bf3.json "" plain; }
DESC_world_ssfar4cf3="world DNT6 ssfar4cf3: ssfar4bf3 without mip chains on the forest floor and stand glazing (the chosen set)"
world_ssfar4cf3() { mip_skip_world ssfar4cf3 ssfar4bf3 $G/specs/ssfar4bf3.json; }
DESC_world_ssfar4cf0="world packs ssfar4cf0 (benchmark): ssfar4cf3 with no fence rule, every stipple panel drawn at every distance"
world_ssfar4cf0() {
  cp $RC/specs/ssfar4b-fence-plain.json $G/specs/
  pack_world ssfar4bf0 $G/specs/ssfar4b-fence-plain.json "" plain
  mip_skip_world ssfar4cf0 ssfar4bf0 $G/specs/ssfar4b-fence-plain.json
}
DESC_world_ssfar4cf3r="world packs ssfar4cf3r: ssfar4cf3 + opaque cores and trimmed punch-through fringes on Sonic (2056) and the rock ivy (6424)"
world_ssfar4cf3r() {
  cp $RC/specs/ssfar4bf3-rock.json $G/specs/
  rm -f $G/specs/near-rock.json
  python3 $T/opaque_cores.py $RC/opaque-cores/rock-decals.json $V $G/world-cells.dnm $G/specs/near-rock.json > $G/design-rock.txt
  pack_world ssfar4bf3r $G/specs/ssfar4bf3-rock.json "" plain
  mip_skip_world ssfar4cf3r ssfar4bf3r $G/specs/ssfar4bf3-rock.json
}
DESC_world_ssfar4e="world packs ssfar4e: ssfar4cf3r + road decals on or above the road (--drape-road-decals) + super-sampled signs (specs/ssfar4e.json)"
world_ssfar4e() {
  cp $RC/specs/ssfar4e.json $G/specs/
  pack_world ssfar4bf3e $G/specs/ssfar4e.json "" plain --drape-road-decals
  mip_skip_world ssfar4e ssfar4bf3e $G/specs/ssfar4e.json
}
DESC_world_ssfar4eu="world packs ssfar4eu: ssfar4e + more super-sampled signs (specs/ssfar4eu.json), no mip chain on a super-sampled word"
# The sign words of the spec's rules board, signs and picks lose their mip chains (a super-sampled sign has none),
# except the video screens 2584, 2588, 2840 of ssfar4e's signs rule, which keep theirs as in ssfar4e.
world_ssfar4eu() {
  cp $RC/specs/ssfar4eu.json $G/specs/
  pack_world ssfar4bf3eu $G/specs/ssfar4eu.json "" plain --drape-road-decals
  local words=$(python3 -c 'import json,sys; print(",".join(str(w) for r in json.load(open(sys.argv[1]))["rules"] if r["name"] in ("board", "signs", "picks") for w in r["textures"] if w not in (2584, 2588, 2840)))' $G/specs/ssfar4eu.json)
  mip_skip_world ssfar4eu ssfar4bf3eu $G/specs/ssfar4eu.json $words
}
DESC_world_ssfar4f="world packs ssfar4f (recommended): ssfar4eu with the plain catch fence (no fence rule, specs/ssfar4f.json)"
# specs/ssfar4f.json = specs/ssfar4eu.json without the rule "fence" (its only fence-specific rule): the 277 stipple
# panels are drawn at full checker density at every distance, as the arcade defines the fence (no three-level groups,
# no quarter-stipple texture variant). Since 16 October (project decision: the grass is not super-sampled) its rule "verges"
# holds only the white track-edge strip 6686: the infield grass 6924 and the grey concrete aprons 6664 stay at their plain
# level at every distance (bilinear; road-surface words have no mip chain, --mip-skip-surface). Since 17 October its
# rule "reels" super-samples the slot machine's three reel strips 2592, 2720, 2848 (64 meshes x 3 faces, world objects)
# with the signs' switch, by instance ("group": "instance": the switch depth in their three materials, the renderer sets
# the bit per reel; no face, group or binding added); they are 256x64 and carry no mip chain anyway. Every other rule,
# the cores and the mip skips as ssfar4eu.
world_ssfar4f() {
  cp $RC/specs/ssfar4f.json $G/specs/
  pack_world ssfar4bf $G/specs/ssfar4f.json "" plain --drape-road-decals
  local words=$(python3 -c 'import json,sys; print(",".join(str(w) for r in json.load(open(sys.argv[1]))["rules"] if r["name"] in ("board", "signs", "picks") for w in r["textures"] if w not in (2584, 2588, 2840)))' $G/specs/ssfar4f.json)
  mip_skip_world ssfar4f ssfar4bf $G/specs/ssfar4f.json $words
}
DESC_world_ssfar4d="world pack ssfar4d: ssfar4c + Sonic rock and rock moss baked into opaque faces (decals/rock-shared.json)"
world_ssfar4d() { pack_world ssfar4d $G/specs/ssfar4b.json 6428,3858,7064 mip --bake-decals $RC/decals/rock-shared.json; }
step world_base world_base
step world_objects world_objects
mkdir -p $G/packs $G/reports
step world_ssfar4b world_ssfar4b
step world_ssfar4c world_ssfar4c
step world_ssfar4bf3 world_ssfar4bf3
step world_ssfar4cf3 world_ssfar4cf3
step world_ssfar4cf3r world_ssfar4cf3r
step world_ssfar4e world_ssfar4e
step world_ssfar4eu world_ssfar4eu
step world_ssfar4f world_ssfar4f
for v in $VARIANTS; do [[ $v == (ssfar4bf3|ssfar4cf3r|ssfar4eu|ssfar4f) ]] || step world_$v world_$v; done

# ---------------------------------------------------------------------------------------------------- Advanced world and sky
# The course-0 pipeline without its course-specific rules: the cells of the attract's slot-2 stretch (frame 3422)
# under the normal-race light, the textures of the Advanced race (video dump at frame 4000 of the time-out run), the
# world objects of 236940[2] that tools/prepare_world_objects.py knows (the pit crews; the props as cone instances;
# the eight frame-counter animations as one base mesh a group, their frames in course2-actors.dna: every texture
# header and luma a frame shows gets a binding, --extra-keys; the colour-cycling ramps of lumabases 2..65 baked into
# 16 texture copies with one palette, --chase-variants 16), resource numbers by texture word (key_pack.py auto), no
# level-of-detail rules, road surface by --surface-ground (course2-surface.txt), mip chains as course 0's shipped
# DNT; strips. tests/test_clock.c checks every animation frame the file and the renderer's decoding give against the
# export of the original's lists. Sky: the panorama of 3a48[2] (43 tile rows).
G2=$W/world-course2
DESC_course2_world="Advanced world and sky: course2-world.dnm/.dnt (cells, objects, face order, DNT6 with mip chains, strips), course2-actors.dna, course2-sky.dns"
course2_world() {
  fresh $G2 $X/cells2-race $X/objects2 $X/sky2; mkdir -p $G2/packs $G2/reports $G2/specs
  $M $R 3422 --export-native-cells $X/cells2-race --race-scenery-lights
  python3 $RC/key_pack.py cells $X/cells2-race auto $G2/world-keys.dnt
  python3 $T/prepare_world_objects.py lists $R $G2/lists.txt --course 2
  $M $R 3482 --export-native-objects $G2/lists.txt $X/objects2
  python3 $T/prepare_world_objects.py sources $R $X/objects2 $X/cells2-race $G2/sources --course 2 > $G2/reports/objects.json
  python3 $T/prepare_models.py $G2/sources $G2/world-src.dnm --world-cells --world-objects --face-group-size $FACE_GROUP \
      --align-tables --share-positions --collapse-flat-cutouts > /dev/null
  local common=(--materials $G2/world-keys.dnt --strip-scope material --static-light -0.45 -0.89 -0.45
      --scenery-classes $RC/course2-surface.txt --surface-ground --match-decal-diagonals --cross-mesh-decals --stipple-panels
      --flatten-stacks --flatten-limit 2 --drape-road-decals)
  local p=$G2/packs/course2-world q=$G2/reports/course2-world
  python3 $T/prepare_face_order.py $G2/world-src.dnm $p-ref.dnm $common --order-map $q-ref-order.json --composites $q-ref-composites.json > $q-ref.json
  python3 $T/prepare_face_order.py $G2/world-src.dnm $p.dnm $common --order-map $q-order.json --composites $q-composites.json \
      --split-video $X/video2 > $q.json
  python3 $T/prepare_palette_materials.py $G2/world-keys.dnt $X/video2 $p-mip.dnt --mesh $p-ref.dnm --split-mesh $p.dnm \
      --share $C/car.dnt --palette-banks 58 --vram-limit 3571712 --composites $q-ref-composites.json --add-mesh-keys \
      --report $q-mip-dnt.json --mipmaps-for all --mip-filter colour --mip-skip-surface --mip-skip-composites \
      --extra-keys $G2/sources/clock_keys.txt --chase-variants 16 > /dev/null
  python3 $T/prepare_restrip.py $p.dnm $p-mip.dnt $p-strips.dnm --rules exact --report $q-strips.json \
      --order-map $q-strips-order.json --pixel-faces $q-strips-pixels.txt > /dev/null
  for f in $p.dnm $p-ref.dnm $p-strips.dnm; do $H/check_native_meshpack $f | tail -1; done
  $H/check_native_material_pack $p-mip.dnt --cpu-backed | tail -1
  python3 $T/prepare_world_objects.py clock $R $X/objects2 $G2/sources $p-strips.dnm $G2/packs/course2-actors.dna --course 2 \
      --expected $G2/clock-expected.ckx > $G2/reports/clock.json
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include $DN/tests/test_clock.c $DN/code/meshpack.c -lm -o $G2/test_clock
  $G2/test_clock $p-strips.dnm $G2/packs/course2-actors.dna $G2/clock-expected.ckx | tail -1
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include $DN/tests/test_clock_light.c $DN/code/meshpack.c -lm -o $G2/test_clock_light
  $G2/test_clock_light $p-strips.dnm $G2/packs/course2-actors.dna | tail -1
  rm -f $G2/clock-expected.ckx $G2/test_clock $G2/test_clock_light
  $M $R 3422 --export-native-sky $X/sky2
  python3 $T/prepare_sky.py $X/sky2/panorama.tga $G2/sky > /dev/null
  if (( ! KEEP )); then rm -rf $X/cells2-race $X/objects2; fi
}
step course2_world course2_world

# ---------------------------------------------------------------------------------------------------- Expert world and sky
# As Advanced's, with the cells of the attract's slot-1 stretch (frame 6602) and the Expert race's textures (frame 4000
# of its time-out run). Its glass and metal have a specular term that follows the camera's turn: the export takes
# every luma it can reach and --live-specular leaves it to the renderer (NM_PACK_LIVE_SPECULAR). Expert draws no world
# objects: the props as cone instances and Expert's world actors (include/expert_actors.h; their frames and placements in
# course1-actors.dna, every luma a placed actor's face can show and the colour-cycling signs' ramps as for Advanced;
# tests/test_clock.c checks every frame against the export). Sky: 3a48[1] (54 tile rows).
G1=$W/world-course1
DESC_course1_world="Expert world and sky: course1-world.dnm/.dnt (cells, objects, face order with live specular, DNT6 with mip chains, strips), course1-actors.dna, course1-sky.dns"
course1_world() {
  fresh $G1 $X/cells1-race $X/objects1 $X/sky1; mkdir -p $G1/packs $G1/reports $G1/specs
  $M $R 6602 --export-native-cells $X/cells1-race --race-scenery-lights
  python3 $RC/key_pack.py cells $X/cells1-race auto $G1/world-keys.dnt
  python3 $T/prepare_world_objects.py lists $R $G1/lists.txt --course 1
  $M $R 6662 --export-native-objects $G1/lists.txt $X/objects1
  python3 $T/prepare_world_objects.py sources $R $X/objects1 $X/cells1-race $G1/sources --course 1 > $G1/reports/objects.json
  python3 $T/prepare_models.py $G1/sources $G1/world-src.dnm --world-cells --world-objects --face-group-size $FACE_GROUP \
      --align-tables --share-positions --collapse-flat-cutouts > /dev/null
  local common=(--materials $G1/world-keys.dnt --strip-scope material --static-light -0.45 -0.89 -0.45 --live-specular
      --scenery-classes $RC/course1-surface.txt --surface-ground --match-decal-diagonals --cross-mesh-decals --stipple-panels
      --flatten-stacks --flatten-limit 2 --drape-road-decals)
  local p=$G1/packs/course1-world q=$G1/reports/course1-world
  python3 $T/prepare_face_order.py $G1/world-src.dnm $p-ref.dnm $common --order-map $q-ref-order.json --composites $q-ref-composites.json > $q-ref.json
  python3 $T/prepare_face_order.py $G1/world-src.dnm $p.dnm $common --order-map $q-order.json --composites $q-composites.json \
      --split-video $X/video1 > $q.json
  python3 $T/prepare_palette_materials.py $G1/world-keys.dnt $X/video1 $p-mip.dnt --mesh $p-ref.dnm --split-mesh $p.dnm \
      --share $C/car.dnt --palette-banks 58 --vram-limit 3571712 --composites $q-ref-composites.json --add-mesh-keys \
      --report $q-mip-dnt.json --mipmaps-for all --mip-filter colour --mip-skip-surface --mip-skip-composites \
      --extra-keys $G1/sources/clock_keys.txt --chase-variants 16 > /dev/null
  python3 $T/prepare_restrip.py $p.dnm $p-mip.dnt $p-strips.dnm --rules exact --report $q-strips.json \
      --order-map $q-strips-order.json --pixel-faces $q-strips-pixels.txt > /dev/null
  for f in $p.dnm $p-ref.dnm $p-strips.dnm; do $H/check_native_meshpack $f | tail -1; done
  $H/check_native_material_pack $p-mip.dnt --cpu-backed | tail -1
  python3 $T/prepare_world_objects.py clock $R $X/objects1 $G1/sources $p-strips.dnm $G1/packs/course1-actors.dna --course 1 \
      --expected $G1/clock-expected.ckx > $G1/reports/clock.json
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include $DN/tests/test_clock.c $DN/code/meshpack.c -lm -o $G1/test_clock
  $G1/test_clock $p-strips.dnm $G1/packs/course1-actors.dna $G1/clock-expected.ckx | tail -1
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include $DN/tests/test_clock_light.c $DN/code/meshpack.c -lm -o $G1/test_clock_light
  $G1/test_clock_light $p-strips.dnm $G1/packs/course1-actors.dna | tail -1
  rm -f $G1/clock-expected.ckx $G1/test_clock $G1/test_clock_light
  $M $R 6602 --export-native-sky $X/sky1
  python3 $T/prepare_sky.py $X/sky1/panorama.tga $G1/sky > /dev/null
  if (( ! KEEP )); then rm -rf $X/cells1-race $X/objects1; fi
}
step course1_world course1_world
DESC_course1_actors="Expert's world actors against their original ticks on the i960 and TGP (m2segment --check-native-expert-actors after 3000 frames of the Expert time-out run)"
course1_actors() { $M $R 3000 --inputs $INPUTS1 --check-native-expert-actors $P/drive.dnd $P1/course1.dnc | grep -E '^(expert_actor|native_expert_actors)'; }
step course1_actors course1_actors

# ---------------------------------------------------------------------------------------------------- name, result, finish, sky, title
DESC_ui_packs="name, result and finish packs and labels, sky, title scene"
ui_packs() {
  fresh $W/ui $HD/name/name_data.h $HD/name/labels $HD/result-labels $HD/result-pal4 $HD/title; mkdir -p $W/ui
  python3 $T/prepare_models.py $X/name-models $W/ui/name.dnm --name-models $R --name-header $HD/name/name_data.h --share-positions
  python3 $T/prepare_result_labels.py $R $HD/name/labels --name-scene --colors $X/name-colours.txt
  python3 $T/prepare_result_labels.py $R $HD/result-labels --colors $X/result-colours.txt
  python3 $T/prepare_result_pal4.py $X/result-models $HD/result-pal4
  python3 $T/prepare_models.py $X/result-models $W/ui/result.dnm --result-models $R
  python3 $T/prepare_models.py $X/finish-models $W/ui/finish.dnm --finish-models $R
  python3 $T/prepare_dreamcast_textures.py $X/finish-models/materials $W/ui/finish --format pal4
  python3 $T/prepare_sky.py $X/sky/panorama.tga $W/ui/sky
  python3 $T/prepare_scene.py $X/title $HD/title --title-program $R/program.bin
}
step ui_packs ui_packs
DESC_select_pack="select screens: PAL4 textures and select.dsl (prepare_select.py), checked against the original's screens"
select_pack() {
  fresh $W/select
  python3 $T/prepare_select.py $X/select-rte $X/select-adv $X/select-exp $W/select
  python3 $DN/tests/test_select_render.py $W/select $X/select-rte
}
step select_pack select_pack
DESC_podium_screen="podium screen: the photo model (--export-native-objects 288cb94), banner, logos, text, background"
podium_screen() {
  fresh $X/podium-photo $W/podium $W/podium-photo-lists.txt $HD/podium-screen
  print -r -- 288cb94 > $W/podium-photo-lists.txt
  $M $R 300 --export-native-objects $W/podium-photo-lists.txt $X/podium-photo
  python3 $T/prepare_podium_screen.py $R $X/name-colours.txt $X/podium-photo $W/podium $HD/podium-screen/podium_screen_data.h
}
step podium_screen podium_screen

# ---------------------------------------------------------------------------------------------------- HUD, race script
bg_wait trace_hud
DESC_hud="HUD pack (prepare_hud.py on the reference trace; the Advanced map from its HUD trace)"
bg_wait trace_hud2
bg_wait trace_hud1
bg_wait trace_hud_lead
# The HUD takes banks 60..63 (palette_banks.h): the Advanced map's colour and the lap message's font fill no free entry.
hud() {
  fresh $HD/hud
  python3 $T/prepare_hud.py $R $X/hud-trace $HD/hud --course-map 2 $X/hud-trace2 --course-map 1 $X/hud-trace1 --second-marker 1 $X/hud-trace-lead --first-bank 60 --max-banks 4
}
step hud hud

bg_wait trace_race
DESC_race_script="race script header of the fast race (trace_race_native production replay of the reference trace), the race and podium against the original's, its sound commands"
race_script() {
  fresh $W/script $HD/script-fast; mkdir -p $W/script $HD/script-fast
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_race_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $W/script/trace_race_native
  rm -f $W/script/native-finish-fast.sound
  $W/script/trace_race_native $P $X/race.trace $W/script/native-finish-fast.trace 5 14000 production \
     --script-header $HD/script-fast/race_script_data.h --checkpoints $W/script/expected-checkpoints-finish-fast.txt \
     --sound $W/script/native-finish-fast.sound
  # The race, the podium and the results against the original, every car on every row (row 0: the last loading row;
  # rows the reference sampled inside a logic pass or in a frame without one are accepted on their evidence only).
  python3 $T/compare_race_trace.py $X/race.trace $W/script/native-finish-fast.trace --first 5 --require-equal --podium \
     --skip-rows 0 > $W/script/compare-finish-fast.txt
  grep -E '^(mid-pass|caught-up|podium:|require-equal:)' $W/script/compare-finish-fast.txt
  rm -f $W/script/native-finish-fast.trace
  # The race's sound commands as the original sends them (include/race_sound.h and the event tail).
  python3 -I $T/compare_race_sound.py $X/race.sound $W/script/native-finish-fast.sound --all
}
step race_script race_script

bg_wait trace_manual
DESC_race_script_manual="race script header of the manual race (production --transmission manual; tests/test_manual_lockstep.py)"
race_script_manual() {
  fresh $HD/script-manual $W/script/expected-checkpoints-manual.txt $W/script/native-manual.sound; mkdir -p $HD/script-manual
  python3 $DN/tests/test_manual_lockstep.py $W/script/trace_race_native $P $X
  $W/script/trace_race_native $P $X/reference-manual.trace $W/script/native-manual.trace 5 14000 production \
     --transmission manual --script-header $HD/script-manual/race_script_data.h \
     --checkpoints $W/script/expected-checkpoints-manual.txt --sound $W/script/native-manual.sound
  rm -f $W/script/native-manual.trace
  # The manual race's sound commands as the original sends them (no command of the original's depends on the
  # transmission; this slower race also reaches 35b0's time calls, ae2125 at frame 12186).
  python3 -I $T/compare_race_sound.py $X/reference-manual.sound $W/script/native-manual.sound --all
}
step race_script_manual race_script_manual

bg_wait trace_race2
DESC_course2_lockstep="Advanced lockstep: trace_race_native --course 2 against the reference, every car on every row; race script"
# FIRST_ROW is the reference's first row with active phase 20 (the rows before it are the loading frames of the
# phase-19 entry); native rows 0 and 1 pair with the last loading row and a row the reference samples mid-pass.
course2_lockstep() {
  fresh $W/script-advanced $HD/script-advanced; mkdir -p $W/script-advanced $HD/script-advanced
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_race_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $W/script-advanced/trace_race_native
  local first=$(awk '$1=="T" { for(i=2;i<=NF;i++) if($i=="active=20") { sub("n=","",$2); print $2; exit } }' $X/race-advanced.trace)
  [[ -n $first ]] || { print -r -- "no phase-20 row in the Advanced reference"; return 1; }
  mkdir -p $W/script-advanced/assets
  ln -s $P/drive.dnd $P2/course2.dn[crv] $P2/course2-route*.dnp $G2/packs/course2-actors.dna $W/script-advanced/assets/
  $W/script-advanced/trace_race_native $W/script-advanced/assets $X/race-advanced.trace $W/script-advanced/native.trace $first 8000 \
     production --course 2 --counter-base auto --script-header $HD/script-advanced/race_script_data.h \
     --checkpoints $W/script-advanced/expected-checkpoints-advanced.txt --clock $W/script-advanced/clock-native.txt
  python3 $T/compare_race_trace.py $X/race-advanced.trace $W/script-advanced/native.trace --first $first --require-equal \
     --skip-rows 0,1 --view-limit 0.001 > $W/script-advanced/compare.txt
  tail -1 $W/script-advanced/compare.txt
  # The frame-counter animations as the native draws them against the original's draws, every tick.
  python3 -I $DN/tests/test_clock_lockstep.py $G2/sources/clock.json $W/script-advanced/clock-native.txt $X/clock-advanced.txt
  rm -f $W/script-advanced/native.trace $W/script-advanced/clock-native.txt
}
step course2_lockstep course2_lockstep

bg_wait trace_race1
DESC_course1_lockstep="Expert lockstep: trace_race_native --course 1 against the reference, every car on every row; race script"
course1_lockstep() {
  fresh $W/script-expert $HD/script-expert; mkdir -p $W/script-expert $HD/script-expert
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_race_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $W/script-expert/trace_race_native
  local first=$(awk '$1=="T" { for(i=2;i<=NF;i++) if($i=="active=20") { sub("n=","",$2); print $2; exit } }' $X/race-expert.trace)
  [[ -n $first ]] || { print -r -- "no phase-20 row in the Expert reference"; return 1; }
  mkdir -p $W/script-expert/assets
  ln -s $P/drive.dnd $P1/course1.dn[crv] $P1/course1-route*.dnp $G1/packs/course1-actors.dna $W/script-expert/assets/
  $W/script-expert/trace_race_native $W/script-expert/assets $X/race-expert.trace $W/script-expert/native.trace $first 9100 \
     production --course 1 --counter-base auto --script-header $HD/script-expert/race_script_data.h \
     --checkpoints $W/script-expert/expected-checkpoints-expert.txt --clock $W/script-expert/clock-native.txt
  python3 $T/compare_race_trace.py $X/race-expert.trace $W/script-expert/native.trace --first $first --require-equal \
     --skip-rows 0,1 --view-limit 0.001 > $W/script-expert/compare.txt
  tail -1 $W/script-expert/compare.txt
  # Expert's world actors as the native draws them against the original's draws and matrices, every tick.
  python3 -I $DN/tests/test_expert_lockstep.py $G1/sources/clock.json $W/script-expert/clock-native.txt $X/clock-expert.txt
  rm -f $W/script-expert/native.trace $W/script-expert/clock-native.txt
}
step course1_lockstep course1_lockstep

# course_finish SLOT NAME TRACE PACKS: a race that ends on the podium, against the original through the podium and the
# results (compare_race_trace.py --podium); its race script header and checkpoint rows (with the podium's).
# course_finish SLOT NAME TRACE PACKS [CLOCK_JSON ORIGINAL_DRAWS]: the frame-counter animations' check where given.
course_finish() {
  local slot=$1 name=$2 trace=$3 packs=$4 clocks=${5:-} original=${6:-} d=$W/script-$2
  fresh $d $HD/script-$name; mkdir -p $d $HD/script-$name
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_race_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $d/trace_race_native
  local first=$(awk '$1=="T" { for(i=2;i<=NF;i++) if($i=="active=20") { sub("n=","",$2); print $2; exit } }' $trace)
  [[ -n $first ]] || { print -r -- "no phase-20 row in $trace"; return 1; }
  mkdir -p $d/assets
  ln -s $P/drive.dnd $packs/course$slot.dn[crv] $packs/course$slot-route*.dnp $d/assets/
  local clock=(); [[ -n $clocks ]] && { ln -s $clocks:h:h/packs/course$slot-actors.dna $d/assets/; clock=(--clock $d/clock-native.txt); }
  $d/trace_race_native $d/assets $trace $d/native.trace $first 18000 production --course $slot --counter-base auto \
     --script-header $HD/script-$name/race_script_data.h --checkpoints $d/expected-checkpoints-$name.txt $clock
  python3 $T/compare_race_trace.py $trace $d/native.trace --first $first --require-equal --podium --skip-rows 0,1 --view-limit 0.001 > $d/compare.txt
  grep -E '^(podium|require-equal):' $d/compare.txt
  # The frame-counter animations (Advanced) or Expert's world actors against the original's draws, race, podium
  # and results.
  local check=test_clock_lockstep.py; (( slot == 1 )) && check=test_expert_lockstep.py
  [[ -z $clocks ]] || python3 -I $DN/tests/$check $clocks $d/clock-native.txt $original
  rm -f $d/clock-native.txt
  awk '$1=="CHECKPOINT" && $3==28 { n++ } END { if(!n) { print "no podium checkpoint rows"; exit 1 } print n " podium checkpoint rows" }' \
     $d/expected-checkpoints-$name.txt
  rm -f $d/native.trace
}
bg_wait trace_race2f
DESC_course2_finish="Advanced finish lockstep: 3rd place, every car on every race row, the podium's cars, the timers through the results; race script"
course2_finish() { course_finish 2 advanced-finish $X/race-advanced-finish.trace $P2 $G2/sources/clock.json $X/clock-advanced-finish.txt; }
step course2_finish course2_finish
bg_wait trace_race1f
DESC_course1_finish="Expert finish lockstep: 3rd place, every car on every race row, the podium's cars, the timers through the results; race script"
course1_finish() { course_finish 1 expert-finish $X/race-expert-finish.trace $P1 $G1/sources/clock.json $X/clock-expert-finish.txt; }
step course1_finish course1_finish
bg_wait trace_attract
DESC_attract_lockstep="attract lockstep: trace_attract_native over four loops (courses 0, 2, 1, 0) against the original, every car, the camera, cells and scene light of every cinematic tick, every car and the camera of every V.R. demo tick; Advanced's frame-counter animations"
attract_lockstep() {
  local d=$W/attract
  fresh $d; mkdir -p $d/assets
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_attract_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $d/trace_attract_native
  ln -s $P/drive.dnd $P/course0.dn[cr] $P/course0-route*.dnp $P/scenery.dnv $P2/course2.dn[crv] $P2/course2-route*.dnp \
     $P1/course1.dn[crv] $P1/course1-route*.dnp $G2/packs/course2-actors.dna $d/assets/
  $d/trace_attract_native $d/assets $d/native.trace 4 --clock $d/clock-native.txt $X/attract.trace --actors 2
  # Expert's actors (and Advanced's animations above), cinematic and demo, against the original's draws.
  ln -s $G1/packs/course1-actors.dna $d/assets/
  $d/trace_attract_native $d/assets $d/native-expert.trace 4 --clock $d/clock-expert.txt $X/attract.trace --actors 1
  python3 -I $DN/tests/test_expert_lockstep.py $G1/sources/clock.json $d/clock-expert.txt $X/clock-attract.txt
  rm -f $d/native-expert.trace $d/clock-expert.txt
  # Fails on any difference (the accepted rows are counted in the last lines).
  python3 -I $DN/tests/test_attract_lockstep.py $X/attract.trace $d/native.trace > $d/compare.txt || { tail -20 $d/compare.txt; return 1; }
  tail -2 $d/compare.txt
  python3 -I $DN/tests/test_clock_lockstep.py $G2/sources/clock.json $d/clock-native.txt $X/clock-attract.txt
  rm -f $d/native.trace $d/clock-native.txt
}
step attract_lockstep attract_lockstep
bg_wait trace_attract_2d
DESC_attract_2d="attract 2D layer: the atlas and the blocks (prepare_attract_2d.py), checked against the original's layer A and line table through four loops (test_attract_2d)"
attract_2d() {
  local d=$W/attract-2d-check
  fresh $W/attract-2d $HD/attract-2d $d; mkdir -p $d
  python3 $T/prepare_attract_2d.py $R $X/attract-2d/pens_00180.bin $W/attract-2d $HD/attract-2d/attract_2d_data.h \
    --check-tiles $X/attract-2d/tiles_0{0180,1900,2200,5300,8400}.bin
  cc -std=c11 -O1 -Wall -Wextra -Werror -I$DN/include -I$HD/attract-2d -I$HD/record-data $DN/tests/test_attract_2d.c \
     -o $d/test_attract_2d
  $d/test_attract_2d $X/attract.trace $X/attract-2d 4
  rm -rf $d
}
step attract_2d attract_2d
DESC_games_lockstep="games lockstep: the original's runs of two games and of games from the demo and the title (inputs/games-A..J), each game through trace_attract_native and trace_race_native chained, every car on every row, tracking words included"
games_lockstep() {
  local d=$W/games x n i=0
  fresh $d; mkdir -p $d/assets $d/traces
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_race_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $d/trace_race_native
  cc -std=c11 -O2 -fno-fast-math -ffp-contract=off -I$DN/include -I$W/intro -I$HD/intent-data -I$HD/race-start-data \
     -I$HD/record-data -I$HD/podium-data $DN/tests/trace_attract_native.c $DN/code/{drive,contact,road,path,visibility,props,props_scene,expert_actors}.c -lm \
     -o $d/trace_attract_native
  ln -s $P/drive.dnd $P/course0.dn[cr] $P/course0-route*.dnp $P/scenery.dnv $P2/course2.dn[crv] $P2/course2-route*.dnp \
     $P1/course1.dn[crv] $P1/course1-route*.dnp $d/assets/
  for x in A B C D E F G H I J; do
    n=$(awk '$1=="frames" { print $2; exit }' $RC/inputs/games-$x.txt)
    $M $R $n --inputs $RC/inputs/games-$x.txt --trace-frontend $d/traces/frontend-$x.trace --trace-race $d/traces/race-$x.trace \
       --trace-attract > $d/traces/$x.log 2>&1 &
    (( (++i)%JOBS )) || wait
  done
  wait
  for x in A B C D E F G H I J; do [[ -s $d/traces/race-$x.trace ]] || { print -r -- "no reference of games-$x"; return 1; }; done
  python3 -I $DN/tests/test_games_lockstep.py $d/trace_race_native $d/trace_attract_native $d/assets $d/traces > $d/compare.txt || { tail -20 $d/compare.txt; return 1; }
  tail -19 $d/compare.txt
  fresh $d/traces
}
step games_lockstep games_lockstep
DESC_props_slow="2230ac's slow knock: runs that drive into a prop of Advanced and of Expert at speed index 54 (m2segment --trace-props), tests/test_props.c on every tick, draw and hit sound"
props_slow() {
  local d=$W/props-slow
  fresh $d; mkdir -p $d/assets
  $M $R 4030 --inputs $RC/inputs/inputs-prop-advanced.txt --trace-race $d/advanced.trace --trace-props $d/advanced.bin > $d/advanced.log 2>&1
  $M $R 9370 --inputs $RC/inputs/inputs-prop-expert.txt --trace-race $d/expert.trace --trace-props $d/expert.bin > $d/expert.log 2>&1
  ln -s $P/drive.dnd $P/course0.dnc $P1/course1.dnc $P2/course2.dnc $d/assets/
  cc -std=c11 -O1 -fno-fast-math -ffp-contract=off -I$DN/include -I$HD/race-start-data -I$W/intro $DN/tests/test_props.c \
     $DN/code/props.c $DN/code/drive.c $DN/code/contact.c -lm -o $d/test_props
  $d/test_props $d/assets --course 2 $d/advanced.bin --course 1 $d/expert.bin | tee $d/compare.txt | tail -2
  grep -q 'knock starts.*2230ac 2/' $d/compare.txt || { print -r -- "no slow knock of 2230ac in both runs"; return 1; }
  rm -f $d/advanced.trace $d/expert.trace $d/advanced.bin $d/expert.bin
}
step props_slow props_slow
if (( ! KEEP )); then rm -rf $X/attract-2d; fi
if (( ! KEEP )); then rm -rf $X/hud-trace $X/hud-trace2 $X/hud-trace1 $X/hud-trace-lead $X/race.trace $X/race.sound $X/reference-manual.trace $X/reference-manual.sound $X/race-advanced.trace $X/race-expert.trace $X/race-advanced-finish.trace $X/race-expert-finish.trace $X/clock-advanced.txt $X/clock-advanced-finish.txt $X/clock-expert.txt $X/clock-expert-finish.txt $X/attract.trace $X/clock-attract.txt; fi

# ---------------------------------------------------------------------------------------------------- asset sets
DESC_assets="asset sets assets-ssfar4f, assets-ssfar4eu, assets-ssfar4e, assets-ssfar4cf3r, assets-ssfar4cf3, assets-ssfar4c, assets-ssfar4b and the variants asked for"
assets() {
  local set
  for set in ssfar4b ssfar4c ssfar4cf3 ssfar4cf3r ssfar4e ssfar4eu ssfar4f ${${${VARIANTS:#ssfar4cf3r}:#ssfar4eu}:#ssfar4f}; do
    local d=$OUT/assets-$set
    fresh $d
    mkdir -p $d/finish $d/hud $d/labels $d/name-labels $d/result
    cp $P/*.dnd $P/*.dnc $P/*.dnr $P/*.dnp $P/scenery.dnv $d/
    cp $C/car.dnm $C/car.dnt $C/car-damage.dnb $C/car-hoods.dnb $d/
    cp $P2/course2.dnc $P2/course2.dnr $P2/course2-route*.dnp $P2/course2.dnv $d/
    cp $G2/packs/course2-world-strips.dnm $d/course2-world.dnm
    cp $G2/packs/course2-world-mip.dnt $d/course2-world.dnt
    cp $G2/packs/course2-actors.dna $d/course2-actors.dna
    cp $G2/sky/sky.dns $d/course2-sky.dns
    cp $P1/course1.dnc $P1/course1.dnr $P1/course1-route*.dnp $P1/course1.dnv $d/
    cp $G1/packs/course1-world-strips.dnm $d/course1-world.dnm
    cp $G1/packs/course1-world-mip.dnt $d/course1-world.dnt
    cp $G1/packs/course1-actors.dna $d/course1-actors.dna
    cp $G1/sky/sky.dns $d/course1-sky.dns
    cp $W/ui/name.dnm $W/ui/result.dnm $W/ui/finish.dnm $W/ui/sky/sky.dns $d/
    cp $W/ui/finish/*.dt $W/ui/finish/*.dt.pal $d/finish/
    cp $HD/hud/hud/* $d/hud/
    mkdir -p $d/select; cp $W/select/select/* $d/select/
    cp $HD/result-labels/label-*.dt $HD/result-labels/labels.dp $d/labels/
    cp $HD/name/labels/label-*.dt $HD/name/labels/labels.dp $d/name-labels/
    cp $HD/result-pal4/texture_*.dt $HD/result-pal4/palettes.dpal $d/result/
    mkdir -p $d/podium; cp $W/podium/podium-*.dt $W/podium/cinematic.dp $W/podium/static.dp $d/podium/
    mkdir -p $d/attract-2d; cp $W/attract-2d/attract-2d.dt $d/attract-2d/
    cp $G/packs/world-$set-strips.dnm $d/world.dnm
    cp $G/packs/world-$set-mip.dnt $d/world.dnt
    if (( ! NOMUSIC )); then mkdir -p $d/music; cp $MU/music/t<01-45>.afb $MU/music/t<01-45>.afx $d/music/; fi
    if (( ! NOMUSIC )) && [[ -f $MA/music/t05.afx ]]; then mkdir -p $d/music/adpcm; cp $MA/music/t0[45].af[bx] $d/music/adpcm/; fi
    if (( SFX )); then mkdir -p $d/sfx; cp $SX/sfx/race.afb $SX/sfx/race.dsx $d/sfx/; [[ ! -f $SX/sfx/voices.pak ]] || cp $SX/sfx/voices.pak $d/sfx/; fi
  done
  {
    print -r -- "# make variables for the ELF build (platform/dreamcast-native/Makefile) against these outputs"
    print -r -- "RACE_ASSET_DIR=$HD/race-header"
    print -r -- "NATIVE_PODIUM_DATA_DIR=$HD/podium-data"
    print -r -- "NATIVE_RACE_START_DATA_DIR=$HD/race-start-data"
    print -r -- "NATIVE_HUD_DIR=$HD/hud"
    print -r -- "NAME_ASSET_DIR=$HD/name"
    print -r -- "NATIVE_RESULT_PAL4_DIR=$HD/result-pal4"
    print -r -- "NATIVE_RESULT_DATA_DIR=$HD/result-data"
    print -r -- "NATIVE_RESULT_LABEL_DIR=$HD/result-labels"
    print -r -- "NATIVE_RECORD_DATA_DIR=$HD/record-data"
    print -r -- "NATIVE_INTENT_DATA_DIR=$HD/intent-data"
    print -r -- "TITLE_SCENE_DIR=$HD/title"
    print -r -- "NATIVE_SLOT_GLASS_DIR=$HD/slot-glass"
    print -r -- "NATIVE_PODIUM_SCREEN_DIR=$HD/podium-screen"
    print -r -- "NATIVE_ATTRACT_2D_DIR=$HD/attract-2d"
    print -r -- "ENJ_ROMDIR=$OUT/assets-ssfar4f"
    print -r -- "# scripted fast race (NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-fast"
    print -r -- "# scripted manual race (NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-manual"
    print -r -- "# scripted Advanced time-out run (NATIVE_COURSE=2 NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-advanced"
    print -r -- "# scripted Expert time-out run (NATIVE_COURSE=1 NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-expert"
    print -r -- "# scripted Advanced 3rd place with podium (NATIVE_COURSE=2 NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-advanced-finish"
    print -r -- "# scripted Expert 3rd place with podium (NATIVE_COURSE=1 NATIVE_RACE_SCRIPT=1): NATIVE_RACE_SCRIPT_DIR=$HD/script-expert-finish"
  } > $OUT/make-vars.txt
}
rm -f $OUT/stamps/assets
step assets assets

# ---------------------------------------------------------------------------------------------------- ELFs
DESC_elfs="default stage 6 ELFs: stats (scripted fast race, render statistics, drop log), stats-manual, controller, controller-advanced, play (from the attract), play-heap-trace, stats-attract (the attract loop, render statistics, drop log), stats-advanced, stats-expert, controller-expert, stats-advanced-finish, stats-expert-finish"
elf_one() { # NAME make-variables...
  local name=$1; shift
  local d=$OUT/elf/$name; fresh $d; mkdir -p $d
  local vars=(${(f)"$(grep -v '^#' $OUT/make-vars.txt)"})
  local goal=(); (( CHECK )) && goal=(check)
  make -C $DN ENDJINN_ROOT=$ENDJINN_ROOT $vars NATIVE_RESIDENT_CAR=1 NATIVE_FRAME_PROFILE=1 NATIVE_HANDOPT=1 \
      RACE_TICK_LIMIT=0 "$@" ENJ_BUILDDIR=$d/obj ENJ_BINDIR=$d/bin -j$JOBS $goal > $d/make.log 2>&1 \
      || { tail -30 $d/make.log; return 1; }
  local elf=$(print $d/bin/*.elf)
  sh-elf-objcopy -O binary $elf $d/loaded.bin
  shasum -a 256 $d/loaded.bin | cut -c1-64 > $d/loaded.sha256; rm -f $d/loaded.bin
  shasum -a 256 $elf | cut -c1-64 > $d/elf.sha256
  rm -rf $d/obj
  print -r -- "$name: ${elf:t} loaded $(cat $d/loaded.sha256)"
}
elfs() {
  elf_one stats AUTO_START=1 NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-fast NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  elf_one stats-manual AUTO_START=1 NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-manual NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  elf_one controller AUTO_START=1
  # Advanced (course slot 2): the scripted time-out run with checkpoints, and a controller ELF that boots into an
  # Advanced race (NATIVE_COURSE: the course of the first race; the front end chooses it at run time).
  elf_one controller-advanced AUTO_START=1 NATIVE_COURSE=2
  # The game from the attract: credit (START), the course and transmission select (every course raced), the race,
  # back to the attract. play-heap-trace: the same with NATIVE_HEAP_TRACE=1 (every named allocation and the heap
  # after each scene load, to compare with tools/heap_budget.py --session).
  elf_one play AUTO_START=attract
  elf_one play-heap-trace AUTO_START=attract NATIVE_HEAP_TRACE=1
  # The attract loop with render statistics and the drop log: the cinematics of courses 0, 2 and 1 with their titles
  # (the next course loads in each title's frames: the scene loader's and the title's lines), then the run ends.
  elf_one stats-attract AUTO_START=attract NATIVE_ATTRACT_LOOPS=3 NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  elf_one stats-advanced AUTO_START=1 NATIVE_COURSE=2 \
      NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-advanced NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  # Expert (course slot 1): the scripted time-out run with checkpoints, and a controller ELF that boots into it.
  elf_one stats-expert AUTO_START=1 NATIVE_COURSE=1 \
      NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-expert NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  elf_one controller-expert AUTO_START=1 NATIVE_COURSE=1
  elf_one stats-advanced-finish AUTO_START=1 NATIVE_COURSE=2 \
      NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-advanced-finish NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
  elf_one stats-expert-finish AUTO_START=1 NATIVE_COURSE=1 \
      NATIVE_RACE_SCRIPT=1 NATIVE_RACE_SCRIPT_DIR=$HD/script-expert-finish NATIVE_RACE_CHECKPOINTS=1 \
      NATIVE_DIAG_RENDER_STATS=1 NATIVE_DIAG_DROP_LOG=1
}
if (( ELF )); then rm -f $OUT/stamps/elfs; step elfs elfs; cat $OUT/logs/elfs.log; fi

# ---------------------------------------------------------------------------------------------------- hashes
# Every output file, relative to OUTDIR. native_scene.inc names its textures by absolute path: hashed without it.
( cd $OUT
  { find assets-* -type f 2>/dev/null || true
    print -l headers/race-header/intro_drive_data.h headers/podium-data/podium_data.h headers/race-start-data/race_start_data.h \
      headers/script-fast/race_script_data.h headers/script-manual/race_script_data.h headers/hud/hud_data.h headers/name/name_data.h headers/name/name_entry_data.h \
      headers/name/labels/name_label_data.h headers/result-pal4/result_pal4_data.h headers/result-data/result_actor_data.h \
      headers/result-labels/result_label_data.h headers/record-data/race_record_data.h headers/intent-data/race_intent_data.h \
      headers/slot-glass/slot_glass_data.h headers/podium-screen/podium_screen_data.h headers/attract-2d/attract_2d_data.h \
      headers/script-advanced/race_script_data.h headers/script-expert/race_script_data.h \
      headers/script-advanced-finish/race_script_data.h headers/script-expert-finish/race_script_data.h
    find headers/title -type f -name '*.dt' || true
  } | LC_ALL=C sort | while read -r f; do print -r -- "$(shasum -a 256 $f | cut -c1-64)  $f"; done
  print -r -- "$(sed 's|^#embed ".*/|#embed "|' headers/title/native_scene.inc | shasum -a 256 | cut -c1-64)  headers/title/native_scene.inc (paths stripped)"
  for e in elf/*/loaded.sha256(N); do print -r -- "$(cat $e)  $e"; done
) > $OUT/outputs.sha256
if [[ -f $RC/expected.sha256 ]]; then
  ok=0; bad=0; missing=0; silent=0
  while read -r sum name; do
    [[ $sum == \#* || -z $sum ]] && continue
    got=$(grep -F "  $name" $OUT/outputs.sha256 | head -1 | cut -c1-64 || true)
    if [[ -z $got ]] && (( NOMUSIC )) && [[ $name == assets-*/music/* ]]; then (( silent += 1 ))
    elif [[ -z $got ]]; then (( missing += 1 )); [[ $name == assets-ssfar4d/* || $name == assets-ssfar4bf3/* ]] || print -r -- "   not built: $name"
    elif [[ $got == $sum ]]; then (( ok += 1 ))
    else (( bad += 1 )); print -r -- "   DIFFERS: $name"; fi
  done < $RC/expected.sha256
  print -r -- "-- byte identity against recipes/expected.sha256: $ok identical, $bad different, $missing not built"
  (( ! silent )) || print -r -- "   --no-music: the $silent music rows not built (asset sets without music/: the game runs silent)"
fi
printf -- '-- done in %.0f s: %s\n' $(( EPOCHREALTIME - START )) $OUT
