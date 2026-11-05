#!/usr/bin/env zsh
# The original's sound effects and voices as AICAflow assets: one AFB sample bank and one .dsx (index, engine tables,
# control flows) per bank of sfx-banks.txt.
#
#   zsh platform/dreamcast-native/recipes/build_sfx.sh ROMSET.zip OUTDIR [--aicaflow DIR] [--music DIR] [--voices MODE]
#
# ROMSET.zip  the daytona93 set (MAME layout) with its sound ROMs: epr-16720/16721 (the 68000 program, Revision A's,
#             as MAME's set has it) and mpr-16491..16494 (the MultiPCM samples), checked by CRC.
# OUTDIR      output directory (keep it under the ignored build/); OUTDIR/sfx is made from scratch on every run.
# --aicaflow DIR  AICAflow checkout with AICAforge (default: extern/aicaflow from platform/dreamcast-native/scripts/fetch_aicaflow.sh).
# --music DIR the music/ of build_music.sh: the race bank must fit next to its race recording t03; without it the
#             check uses t03's PCM8 size (1,561,766 bytes with its flow).
# --voices MODE  the announcer's phrases of sfx-banks.txt's "voices" line (ADPCM): stream (default) writes
#             sfx/voices.pak, one bank and flow each, for NATIVE_SFX_VOICE_STREAM=1 (audio/sfx.h), and checks that
#             the largest fits beside the race bank and t03; resident puts them into the race bank (fits only
#             beside t03 in ADPCM: build_music.sh --adpcm, --music); none leaves them out.
#
# Needs: platform/dreamcast-native/scripts/fetch_aicaflow.sh run once, a C11 and a C++20 compiler, zlib headers (AICAforge), python3.
#
# Steps: tools/extract_sound_roms.py (sound images, deleted at the end), tools/prepare_sfx.cpp render (the native
# sequencer plays every effect command from the boot state into a MultiPCM VGM; engine layers and their tables),
# AICAforge afx_vgm on the flows a bank lists (PCM8 or ADPCM), prepare_sfx park on the engine layers (controlled
# flows), afx_bank --merge per bank, prepare_sfx pack.
#
# Outputs (ROM-derived: never commit them):
#   OUTDIR/sfx/BANK.afb   the bank's samples
#   OUTDIR/sfx/BANK.dsx   its index, velocity gains, engine table and flows (code/audio/sfx.c)
#   OUTDIR/sfx/voices.pak with --voices stream: "VOX1", version, count, then per phrase (key, bank offset, bank
#                         bytes, flow offset, flow bytes) and the AFB/AFX images, each at a 32-byte boundary
#   OUTDIR/sfx/render.tsv every flow rendered (key, command, kind, track, notes, voices, length, samples)
#   OUTDIR/sfx/banks.tsv  per bank: flows, AFB bytes, AICA bytes (32-byte allocations), what is left beside t03
#   OUTDIR/outputs.sha256 sha256 of every file in sfx/; compared with recipes/sfx-expected.sha256 when it exists
emulate -L zsh
setopt err_exit pipe_fail no_unset
zmodload zsh/datetime
zmodload -F zsh/stat b:zstat

SELF=${0:A}
RC=${SELF:h}
REPO=${RC:h:h:h}
DN=$REPO/platform/dreamcast-native
# AICAflow's asset arena on hardware ("Sound: AICAflow up, ... bytes"); the race recording t03 with its flow (PCM8).
ARENA=2072768
RACE_MUSIC_DEFAULT=1561766

usage() { sed -n '2,32p' $SELF | sed 's/^# \{0,1\}//'; exit 2; }
(( $# >= 2 )) || usage
ROMSET=${1:A}; OUT=${2:A}; shift 2
AF=$REPO/extern/aicaflow; MUSIC=""; VOICES=stream
while (( $# )); do
  case $1 in
    --aicaflow) AF=${2:A}; shift;;
    --music) MUSIC=${2:A}; shift;;
    --voices) VOICES=$2; [[ $VOICES == (stream|resident|none) ]] || usage; shift;;
    *) usage;;
  esac
  shift
done
[[ -f $ROMSET ]] || { print -u2 -r -- "ROM set $ROMSET not found"; exit 2; }
[[ -f $AF/dependencies/AICAforge/Makefile ]] || { print -u2 -r -- "no AICAflow/AICAforge at $AF: run platform/dreamcast-native/scripts/fetch_aicaflow.sh"; exit 2; }

START=$EPOCHREALTIME
W=$OUT/work/sfx
rm -rf $W $OUT/sfx
mkdir -p $OUT/logs $W/bin $OUT/sfx
print -r -- "-- AICAforge tools ($AF) and prepare_sfx"
{ make -C $AF authoring
  cc -std=c11 -O2 -c -I$AF/driver/format/include $AF/driver/format/src/codec.c -o $W/bin/codec.o
  c++ -std=c++20 -O2 -DM2_DC_NATIVE_HOST -I$REPO/src -I$AF/driver/format/include $DN/tools/prepare_sfx.cpp \
      $REPO/src/runtime/native_sound_sequencer.cpp $W/bin/codec.o -o $W/bin/prepare_sfx
} > $OUT/logs/sfx-tools.log 2>&1 || { tail -20 $OUT/logs/sfx-tools.log; exit 1; }
VGM=$AF/dependencies/AICAforge/build/afx_vgm
BANK=$AF/dependencies/AICAforge/build/afx_bank
P=$W/bin/prepare_sfx

print -r -- "-- sound images and the render"
python3 -I $DN/tools/extract_sound_roms.py $ROMSET $W/sound
$P render $W/sound $W/render
rm -rf $W/sound
cp $W/render/render.tsv $OUT/sfx/

# Banks: "BANK CODING KEY..." lines; a range FIRST-LAST covers the rendered keys with FIRST's prefix in between.
typeset -A coding
banks=(); voice_keys=(); race_left=0
while read -r bank code keys; do
  [[ -z $bank || $bank == \#* ]] && continue
  [[ $code == (pcm8|adpcm) ]] || { print -u2 -r -- "sfx-banks.txt: coding $code"; exit 1; }
  [[ $bank == voices ]] || (( ${banks[(Ie)$bank]} )) || banks+=($bank)
  expanded=$(python3 -I - $W/render/render.tsv ${=keys} <<'EOF'
import sys
keys = [line.split('\t')[0] for line in open(sys.argv[1]).read().splitlines()[1:]]
for spec in sys.argv[2:]:
    if '-' in spec:
        first, last = spec.split('-')
        n = 0
        while n < min(len(first), len(last)) and first[n] == last[n]:
            n += 1
        prefix, lo, hi = first[:n], int(first[n:], 16), int(last[n:], 16)
        found = [k for k in keys if k.startswith(prefix) and len(k) == len(first) and lo <= int(k[n:], 16) <= hi]
    else:
        found = [k for k in keys if k == spec]
        if not found:
            sys.exit(f'sfx-banks.txt: {spec} was not rendered (it sounds nothing)')
    print('\n'.join(found))
EOF
  )
  for key in ${(f)expanded}; do
    [[ -n $key ]] || continue
    if [[ $bank == voices ]]; then
      [[ $code == adpcm ]] || { print -u2 -r -- "sfx-banks.txt: voices are adpcm"; exit 1; }
      voice_keys+=($key)
      [[ $VOICES == resident ]] && coding[race/$key]=adpcm
      continue
    fi
    [[ -z ${coding[$bank/$key]:-} ]] || { print -u2 -r -- "sfx-banks.txt: $key twice in $bank"; exit 1; }
    coding[$bank/$key]=$code
  done
done < $RC/sfx-banks.txt

race_music=$RACE_MUSIC_DEFAULT
if [[ -n $MUSIC ]]; then
  [[ -f $MUSIC/t03.afb && -f $MUSIC/t03.afx ]] || { print -u2 -r -- "no t03.afb/t03.afx in $MUSIC"; exit 2; }
  race_music=$(( $(zstat +size $MUSIC/t03.afb) - 32 + $(zstat +size $MUSIC/t03.afx) ))
fi
print -r -- $'bank\tflows\tafb_bytes\taica_bytes\tleft_beside_race_music' > $OUT/sfx/banks.tsv
for bank in $banks; do
  print -r -- "-- bank $bank"
  mkdir -p $W/raw/$bank $W/merged/$bank
  n=0
  for entry in ${(ok)coding}; do
    [[ ${entry%%/*} == $bank ]] || continue
    key=${entry#*/}; opts=(); [[ ${coding[$entry]} == adpcm ]] && opts=(--adpcm)
    $VGM $W/render/vgm/$key.vgm $W/raw/$bank/$key.afx $opts >> $OUT/logs/sfx-convert.log
    rm -f $W/raw/$bank/$key.afc $W/raw/$bank/$key.afv
    if [[ $key == e<0-9> ]]; then
      $P park $W/raw/$bank/$key.afx $W/raw/$bank/$key.parked
      mv $W/raw/$bank/$key.parked $W/raw/$bank/$key.afx
    fi
    (( n += 1 ))
  done
  $BANK --merge $OUT/sfx/$bank.afb $W/merged/$bank $W/raw/$bank/*.afx >> $OUT/logs/sfx-convert.log
  $P pack $W/render/render.tsv $W/render/engine.tbl $W/render/gains.bin $W/merged/$bank $OUT/sfx/$bank.dsx
  aica=$(python3 -I - $OUT/sfx/$bank.afb $OUT/sfx/$bank.dsx <<'EOF'
import struct, sys
align = lambda n: (n + 31) & ~31
afb = open(sys.argv[1], 'rb').read()
dsx = open(sys.argv[2], 'rb').read()
total = align(struct.unpack_from('<I', afb, 20)[0])
count, directory = struct.unpack_from('<II', dsx, 8)
for i in range(count):
    offset = struct.unpack_from('<I', dsx, directory + 16 * i + 8)[0]
    total += align(struct.unpack_from('<I', dsx, offset + 20)[0])  # the AFX upload image
print(total)
EOF
  )
  left=$(( ARENA - race_music - aica ))
  print -r -- "$bank"$'\t'"$n"$'\t'"$(( $(zstat +size $OUT/sfx/$bank.afb) - 32 ))"$'\t'"$aica"$'\t'"$left" >> $OUT/sfx/banks.tsv
  print -r -- "   $bank: $n flows, $aica bytes of AICA RAM; $left left beside the race music ($race_music)"
  if [[ $bank == race ]] && (( left < 4096 )); then
    print -u2 -r -- "the race bank does not fit beside the race music: $left bytes left (less than 4 KiB)"; exit 1
  fi
  [[ $bank == race ]] && race_left=$left
done

if [[ $VOICES == stream ]] && (( $#voice_keys )); then
  print -r -- "-- voices: ${#voice_keys} phrases streamed (sfx/voices.pak)"
  mkdir -p $W/voices
  for key in $voice_keys; do
    $VGM $W/render/vgm/$key.vgm $W/voices/$key.afx --adpcm >> $OUT/logs/sfx-convert.log
    rm -f $W/voices/$key.afc $W/voices/$key.afv
  done
  need=$(python3 -I - $W/voices $OUT/sfx/voices.pak $voice_keys <<'PACK'
import struct, sys
from pathlib import Path
align = lambda n: (n + 31) & ~31
root, out, keys = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
images, need = [], 0
for key in keys:
    afb, afx = (root / f'{key}.afb').read_bytes(), (root / f'{key}.afx').read_bytes()
    # AICA RAM while it plays: its samples (afb header +20) and the flow's upload image (afx +20), 32-byte blocks.
    need = max(need, align(struct.unpack_from('<I', afb, 20)[0]) + align(struct.unpack_from('<I', afx, 20)[0]))
    images.append((int(key, 16), afb, afx))
offset = align(12 + 20 * len(images))
directory, payload = [], bytearray()
for key, afb, afx in images:
    b = offset + len(payload); payload += afb + bytes(align(len(afb)) - len(afb))
    x = offset + len(payload); payload += afx + bytes(align(len(afx)) - len(afx))
    directory.append(struct.pack('<IIIII', key, b, len(afb), x, len(afx)))
head = b'VOX1' + struct.pack('<II', 1, len(images)) + b''.join(directory)
Path(out).write_bytes(head + bytes(offset - len(head)) + payload)
print(need)
PACK
  )
  print -r -- $'voices\t'"${#voice_keys}"$'\t'"$(zstat +size $OUT/sfx/voices.pak)"$'\t'"$need"$'\t'"$(( race_left - need ))" >> $OUT/sfx/banks.tsv
  print -r -- "   voices: the largest phrase needs $need bytes of AICA RAM; $(( race_left - need )) left while it plays"
  if (( race_left - need < 0 )); then
    print -u2 -r -- "the largest streamed phrase does not fit beside the race bank and the race music"; exit 1
  fi
fi
rm -rf $W

( cd $OUT
  find sfx -type f | LC_ALL=C sort | while read -r f; do print -r -- "$(shasum -a 256 $f | cut -c1-64)  $f"; done
) > $OUT/outputs.sha256
if [[ -f $RC/sfx-expected.sha256 ]]; then
  ok=0; bad=0; missing=0
  while read -r sum name; do
    [[ $sum == \#* || -z $sum ]] && continue
    got=$(grep -F "  $name" $OUT/outputs.sha256 | head -1 | cut -c1-64 || true)
    if [[ -z $got ]]; then (( missing += 1 )); print -r -- "   not built: $name"
    elif [[ $got == $sum ]]; then (( ok += 1 ))
    else (( bad += 1 )); print -r -- "   DIFFERS: $name"; fi
  done < $RC/sfx-expected.sha256
  print -r -- "-- byte identity against recipes/sfx-expected.sha256: $ok identical, $bad different, $missing not built"
fi
printf -- '-- done in %.0f s: %s/sfx\n' $(( EPOCHREALTIME - START )) $OUT
