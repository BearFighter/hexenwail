#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the player-class differential harness (slice G of the #469 server
# oxidation) and run PlayerClass_Count, Rust against the C original.
#
#   VARIANT=h2  sv_main_class_h2.c (sv_main.c's), against an archive built with
#               sv_main_class.  Hexen II only: hwsv's server has no such helper.
#
# The C goes in with PlayerClass_Count renamed to c_PlayerClass_Count, and
# engine/rust/sv_main_target.c goes in with -DSV_FIXTURE so its file shim stays
# out of the harness.  Both link against the one Rust archive; --gc-sections
# drops the other slices' accessors.  The plain build is -O2; the coverage build
# repeats it with --coverage and requires every executable line of the fixture
# to have run.  Every run is under a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_class}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root" -I"$here")
DEFS=(-DSERVERONLY)
fixture=sv_main_class_h2.c
WARN=(-Wall -Wno-unused-function -Wno-comment)
SECT=(-ffunction-sections -fdata-sections)
LIMIT=${HARNESS_TIMEOUT:-900}
CASES=${CASES:-4096}

build() { # dir cflags...
	local dir=$1; shift
	mkdir -p "$OUT/$dir"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" \
		-DPlayerClass_Count=c_PlayerClass_Count \
		-c "$here/c_ref_class.c" -o "$OUT/$dir/c_ref.o"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" -DSV_FIXTURE=1 \
		-c "$rust_root/sv_main_target.c" -o "$OUT/$dir/sv_main_target.o"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" \
		-c "$here/class_harness.c" -o "$OUT/$dir/class_harness.o"
	cc "$@" -Wl,--gc-sections -o "$OUT/$dir/class_harness" \
		"$OUT/$dir/class_harness.o" "$OUT/$dir/c_ref.o" \
		"$OUT/$dir/sv_main_target.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$OUT/$1" && CASES="$CASES" timeout -k 10 "$LIMIT" ./class_harness) || status=$?
	if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
		echo "FAIL: the harness hung (timeout ${LIMIT}s)" >&2
	fi
	return "$status"
}

echo "== $VARIANT/h2: the C original against the Rust archive =="
build h2 -O2 -g
log="$OUT/h2.log"
status=0
run h2 | tee "$log" || status=$?
if [ "$status" -ne 0 ] || ! grep -q '^sv_main_class differential harness (h2): PASS (' "$log"; then
	echo "FAIL: h2 differential harness (exit $status)" >&2
	grep -E 'MISMATCH|FAIL|error|harness bug' "$log" >&2 || tail -40 "$log" >&2
	exit 1
fi

count=$(sed -n 's/^sv_main_class differential harness (h2): PASS (\([0-9]*\) cases)$/\1/p' "$log")
[ -n "$count" ] && [ "$count" -ge 1000 ] || {
	echo "FAIL: harness ran ${count:-no} cases (floor 1000)" >&2
	exit 1
}
line=$(grep -m1 '^outcomes:' "$log" || true)
for key in cvar portals neither both; do
	v=$(sed -n "s/.*$key=\([0-9]*\).*/\1/p" <<<"$line")
	[ -n "$v" ] && [ "$v" -ge 1 ] || {
		echo "FAIL: outcome '$key' is ${v:-missing}; the generator must reach it" >&2
		exit 1
	}
done
arms=$(grep -m1 '^arms (taken/not):' "$log" | sed 's/^arms (taken\/not)://' || true)
[ -n "$arms" ] || { echo "FAIL: harness printed no arm counts" >&2; exit 1; }
# shellcheck disable=SC2086  # word pairs: name taken/not
set -- $arms
while [ $# -ge 2 ]; do
	name=$1; t=${2%/*}; f=${2#*/}; shift 2
	if [ "$t" -lt 1 ] || [ "$f" -lt 1 ]; then
		echo "FAIL: branch '$name' ran $t taken / $f not taken; both must run" >&2
		exit 1
	fi
done
echo "floors: h2 -- $count cases, every operation and both arms of every counted branch"

if [ "${HARNESS_COVERAGE:-1}" = 0 ]; then
	echo "coverage: not run (HARNESS_COVERAGE=0)"
	echo "RESULT: PASS"
	exit 0
fi

echo "== $VARIANT/h2: the same run with C line coverage =="
build cov -O0 -g --coverage
(cd "$OUT/cov" && CASES="$CASES" timeout -k 10 "$LIMIT" ./class_harness >/dev/null 2>&1) || {
	echo "FAIL: the coverage run did not pass" >&2
	exit 1
}
(cd "$OUT/cov" && gcov -o . c_ref.o >/dev/null 2>&1) || true
gcov_file="$OUT/cov/$fixture.gcov"
[ -f "$gcov_file" ] || { echo "FAIL: gcov produced no $fixture.gcov" >&2; ls "$OUT/cov" >&2; exit 1; }
unrun=$(grep -E '^ *(#####|=====):' "$gcov_file" || true)
if [ -n "$unrun" ]; then
	echo "FAIL: lines of $fixture the harness never ran:" >&2
	printf '%s\n' "$unrun" >&2
	exit 1
fi
lines=$(grep -cE '^ *[0-9]+\*?:' "$gcov_file" || true)
echo "coverage: every executable line of $fixture ran ($lines)"

echo "RESULT: PASS"
