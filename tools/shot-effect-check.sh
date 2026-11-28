#!/usr/bin/env bash
#
# shot-effect-check.sh -- prove a visual sweep's switches changed pixels.
#
# Usage:
#   shot-effect-check.sh <shotdir> [exclude-geometry ...]
#
#   exclude-geometry is ImageMagick WxH+X+Y (default: the whole bottom status
#   strip, 800x24+0+576).  Every named region is painted flat in BOTH
#   frames before comparing, so an animated widget cannot stand in for an
#   effect.  Requires ImageMagick `magick` on PATH.
#
# A screenshot diff between two BUILDS is not a correctness test for a switch
# fix: the frames are supposed to change.  This is the other half -- within one
# run, each pair below differs only by the switch named in its label, so an
# cvar that silently does nothing fails even when the build comparison passes.
#
# Pairs: FXAA, HDR, exposure, bloom, HDR over bloom, SSAO q1/q3 and both OIT
# modes.  weighted vs moment is reported but not required: r_oit 2 is allowed
# to fall back to weighted where the moment targets are unavailable.

set -uo pipefail

usage() { sed -n '2,17p' "$0"; exit 2; }
[ $# -ge 1 ] || usage
DIR=$1; shift
[ -d "$DIR" ] || { echo "shot-effect-check: no directory $DIR" >&2; exit 2; }
command -v magick >/dev/null || {
  echo "shot-effect-check: ImageMagick 'magick' not on PATH" >&2; exit 2; }

EXCLUDES=("$@")
[ ${#EXCLUDES[@]} -gt 0 ] || EXCLUDES=("800x24+0+576")
mask_args=()
for g in "${EXCLUDES[@]}"; do
  mask_args+=(-fill black -draw "rectangle $(
    printf '%s' "$g" | awk -F'[x+]' '{print $3","$4" "$3+$1-1","$4+$2-1}'
  )")
done

pairs=(
  "FXAA|01-base-farclip|02-fxaa"
  "HDR|01-base-farclip|03-hdr"
  "exposure|03-hdr|04-hdr-exposure"
  "bloom|01-base-farclip|05-bloom"
  "HDR-over-bloom|06-torch-bloom|07-torch-hdr-bloom"
  "SSAO-q1|08-ssao-off|09-ssao-low"
  "SSAO-q3|08-ssao-off|10-ssao-high"
  "OIT-weighted|11-oit-off|12-oit-weighted"
  "OIT-moment|11-oit-off|13-oit-moment"
  "OIT-mode-delta|12-oit-weighted|13-oit-moment"
)

rc=0
for row in "${pairs[@]}"; do
  IFS='|' read -r label a b <<<"$row"
  for f in "$DIR/$a.png" "$DIR/$b.png"; do
    [ -f "$f" ] || { echo "$label: MISSING $f"; rc=1; continue 2; }
  done

  x=$(mktemp --suffix=.png); y=$(mktemp --suffix=.png)
  magick "$DIR/$a.png" "${mask_args[@]}" "$x"
  magick "$DIR/$b.png" "${mask_args[@]}" "$y"
  ae=$(magick compare -metric AE "$x" "$y" null: 2>&1 | awk '{print $1}')
  rm -f "$x" "$y"

  if [[ "$ae" =~ ^[0-9]+([.][0-9]+)?$ ]] && awk -v n="$ae" 'BEGIN { exit !(n > 0) }'; then
    printf '%-16s OK    (%s differing pixels)\n' "$label" "$ae"
  elif [ "$label" = "OIT-mode-delta" ]; then
    printf '%-16s info  (no weighted/moment delta; r_oit 2 may fall back)\n' "$label"
  else
    printf '%-16s FAIL  (%s differing pixels -- switch did nothing)\n' "$label" "${ae:-unreadable}"
    rc=1
  fi
done

[ $rc -eq 0 ] && echo "shot-effect-check: every switch changed pixels" \
                || echo "shot-effect-check: SWITCHES WITH NO EFFECT"
exit $rc
