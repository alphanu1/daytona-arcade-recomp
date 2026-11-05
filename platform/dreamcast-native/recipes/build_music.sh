#!/usr/bin/env zsh
# Convert the Daytona USA soundtrack into AICAflow assets: one AFB sample bank and one AFX control flow per track.
#
#   zsh platform/dreamcast-native/recipes/build_music.sh ARCHIVE OUTDIR [--adpcm] [--gain-db DB] [--aicaflow DIR]
#
# ARCHIVE  vgmrips' "Daytona USA (Sega Model 2)" pack: the 45 VGZ logs of the Model 1 sound board's MultiPCMs.
#          Download it in a browser from https://vgmrips.net/packs/pack/daytona-usa-sega-model-2 (the file is
#          https://vgmrips.net/files/Arcade/SegaSys/Daytona_USA_%28Sega_Model_2%29.zip); vgmrips puts a proof-of-work
#          bot check in front of its files, so this script does not fetch it. Only that archive is accepted
#          (sha256 ac086fd5a246a9cfc125374dd43084d35e0d79dd7ef837b0bdf04784035234a2, 13,639,027 bytes).
# OUTDIR   output directory (keep it under the ignored build/); OUTDIR/music is made from scratch on every run.
# --adpcm  AICA ADPCM samples instead of AICAforge's default PCM8: half the bank (Sky High 1,576,173 -> 788,647
#          bytes). A budget option: its outputs do not match music-expected.sha256.
# --gain-db DB  sample gain (AICAforge's default -6.4 dB); other values do not match music-expected.sha256.
# --aicaflow DIR  AICAflow checkout with AICAforge initialised (default: extern/aicaflow from platform/dreamcast-native/scripts/fetch_aicaflow.sh).
#
# Needs: platform/dreamcast-native/scripts/fetch_aicaflow.sh run once, a C11 compiler and zlib headers (AICAforge's native tools), unzip.
# No Dreamcast toolchain: nothing here runs on the target.
#
# Outputs (game-derived: never commit them):
#   OUTDIR/music/tNN.afx     AICA control flow of track NN (01..45, the archive's numbering), bound to tNN.afb
#   OUTDIR/music/tNN.afb     its sample bank
#   OUTDIR/music/tNN.afc     seek checkpoints (SH4 RAM only); tNN.afv: visualisation data for tools, not the game
#   OUTDIR/music/tracks.tsv  track, stem, title, sample coding, AFX and AFB bytes
#   OUTDIR/outputs.sha256    sha256 of every file in music/; compared with recipes/music-expected.sha256 at the end
emulate -L zsh
setopt err_exit pipe_fail no_unset
zmodload zsh/datetime
zmodload -F zsh/stat b:zstat

SELF=${0:A}
RC=${SELF:h}
REPO=${RC:h:h:h}
ARCHIVE_SHA256=ac086fd5a246a9cfc125374dd43084d35e0d79dd7ef837b0bdf04784035234a2
TRACKS=45

usage() { sed -n '2,25p' $SELF | sed 's/^# \{0,1\}//'; exit 2; }
(( $# >= 2 )) || usage
ARCHIVE=${1:A}; OUT=${2:A}; shift 2
AF=$REPO/extern/aicaflow; ADPCM=0; GAIN=""
while (( $# )); do
  case $1 in
    --adpcm) ADPCM=1;;
    --gain-db) GAIN=$2; shift;;
    --aicaflow) AF=${2:A}; shift;;
    *) usage;;
  esac
  shift
done
[[ -f $ARCHIVE ]] || { print -u2 -r -- "archive $ARCHIVE not found (download it in a browser: see the header of this script)"; exit 2; }
sum=$(shasum -a 256 $ARCHIVE | cut -c1-64)
[[ $sum == $ARCHIVE_SHA256 ]] || { print -u2 -r -- "archive sha256 $sum is not vgmrips' Daytona USA pack ($ARCHIVE_SHA256)"; exit 2; }
[[ -f $AF/dependencies/AICAforge/Makefile ]] || { print -u2 -r -- "no AICAflow/AICAforge at $AF: run platform/dreamcast-native/scripts/fetch_aicaflow.sh"; exit 2; }
command -v unzip >/dev/null || { print -u2 -r -- "unzip not found"; exit 2; }

START=$EPOCHREALTIME
mkdir -p $OUT/logs
print -r -- "-- AICAforge tools and the AICAflow validator ($AF)"
{ make -C $AF authoring && make -C $AF/driver build/afx_validate; } > $OUT/logs/tools.log 2>&1 \
  || { tail -20 $OUT/logs/tools.log; exit 1; }
VGM=$AF/dependencies/AICAforge/build/afx_vgm
VALIDATE=$AF/driver/build/afx_validate

rm -rf $OUT/work/vgz $OUT/music
mkdir -p $OUT/work/vgz $OUT/music
unzip -q -j $ARCHIVE '*.vgz' -d $OUT/work/vgz
sources=($OUT/work/vgz/*.vgz(N))
(( ${#sources} == TRACKS )) || { print -u2 -r -- "expected $TRACKS VGZ files in the archive, found ${#sources}"; exit 1; }

opts=(); (( ADPCM )) && opts+=(--adpcm); [[ -n $GAIN ]] && opts+=(--gain-db $GAIN)
coding=pcm8; (( ADPCM )) && coding=adpcm
print -r -- "-- converting $TRACKS tracks ($coding${GAIN:+, gain $GAIN dB})"
print -r -- $'track\tstem\ttitle\tcoding\tafx_bytes\tafb_bytes' > $OUT/music/tracks.tsv
for src in $sources; do
  name=${src:t:r}; num=${name%% *}; title=${name#* }
  [[ $num == <01-99> && ${#num} == 2 ]] || { print -u2 -r -- "unexpected archive member ${src:t}"; exit 1; }
  stem=t$num
  [[ ! -e $OUT/music/$stem.afx ]] || { print -u2 -r -- "two archive members numbered $num"; exit 1; }
  $VGM $src $OUT/music/$stem.afx $opts >> $OUT/logs/convert.log
  $VALIDATE $OUT/music/$stem.afx >> $OUT/logs/convert.log
  print -r -- "$num"$'\t'"$stem"$'\t'"$title"$'\t'"$coding"$'\t'"$(zstat +size $OUT/music/$stem.afx)"$'\t'"$(zstat +size $OUT/music/$stem.afb)" >> $OUT/music/tracks.tsv
done
rm -rf $OUT/work

( cd $OUT
  find music -type f | LC_ALL=C sort | while read -r f; do print -r -- "$(shasum -a 256 $f | cut -c1-64)  $f"; done
) > $OUT/outputs.sha256
if (( ! ADPCM )) && [[ -z $GAIN ]] && [[ -f $RC/music-expected.sha256 ]]; then
  ok=0; bad=0; missing=0
  while read -r sum name; do
    [[ $sum == \#* || -z $sum ]] && continue
    got=$(grep -F "  $name" $OUT/outputs.sha256 | head -1 | cut -c1-64 || true)
    if [[ -z $got ]]; then (( missing += 1 )); print -r -- "   not built: $name"
    elif [[ $got == $sum ]]; then (( ok += 1 ))
    else (( bad += 1 )); print -r -- "   DIFFERS: $name"; fi
  done < $RC/music-expected.sha256
  print -r -- "-- byte identity against recipes/music-expected.sha256: $ok identical, $bad different, $missing not built"
fi
printf -- '-- done in %.0f s: %s/music (%s bytes of AFB, %s of AFX)\n' $(( EPOCHREALTIME - START )) $OUT \
  "$(awk -F'\t' 'NR>1{s+=$6}END{print s}' $OUT/music/tracks.tsv)" "$(awk -F'\t' 'NR>1{s+=$5}END{print s}' $OUT/music/tracks.tsv)"
