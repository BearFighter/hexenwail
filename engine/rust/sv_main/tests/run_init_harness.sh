#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the sv_main init differential harness (F / slice F of #469) and run:
# SV_Init, SV_Protocol_f, Max_Edicts_f, SV_Edicts and Sv_Edicts_f, Rust against
# the C original.
#
#   VARIANT=h2  sv_main_init_h2.c (hexen2/sv_main.c's), against an archive
#               built with sv_main_init.  Hexen II only: hwsv's sv_main.c is
#               HexenWorld's own and has none of these functions except its
#               own SV_Init.
#
# The C goes in with every ported function renamed to c_*, and
# engine/rust/sv_main_target.c goes in with -DSV_FIXTURE so its file-output
# shim reaches the harness's recorder.  Both link against the one Rust
# archive.  --gc-sections drops the other slices' target accessors, whose
# globals this harness does not define.  The plain build is at -O2; the glquake
# build is repeated with --coverage, and every executable line of the fixture
# must run.  Every run is under a timeout.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust_root="$(cd "$here/../.." && pwd)"
engine="$(cd "$rust_root/.." && pwd)"
root="$(cd "$engine/.." && pwd)"

VARIANT="${VARIANT:-h2}"
RUST_LIB="${RUST_LIB:?set RUST_LIB to the archive built with sv_main_init}"
OUT="${OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -f "$RUST_LIB" ] || { echo "error: Rust staticlib not found: $RUST_LIB" >&2; exit 2; }
[ "$VARIANT" = h2 ] || { echo "error: VARIANT must be h2 (this slice is Hexen II's alone)" >&2; exit 2; }

INCS=(-I"$engine/hexen2" -I"$engine/h2shared" -I"$root/common" -I"$rust_root")
# The engine compiles sv_main.c's server half with SERVERONLY; without it
# quakeinc.h pulls in the client renderer headers, whose hull_t/ptype_t types
# this server-only harness deliberately does not provide.
DEFS=(-DSERVERONLY)
fixture=sv_main_init_h2.c
WARN=(-Wall -Wno-unused-function -Wno-comment)
SECT=(-ffunction-sections -fdata-sections)
LIMIT=${HARNESS_TIMEOUT:-900}
CASES=${CASES:-6000}

build() { # dir cflags...
	local dir=$1; shift
	mkdir -p "$OUT/$dir"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" \
		-DSV_FIXTURE="\"$here/$fixture\"" \
		-DSV_Init=c_SV_Init -DSV_Edicts=c_SV_Edicts \
		-DSV_Protocol_f=c_SV_Protocol_f -DSv_Edicts_f=c_Sv_Edicts_f \
		-DMax_Edicts_f=c_Max_Edicts_f \
		-c "$here/c_ref_init.c" -o "$OUT/$dir/c_ref.o"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" -DSV_FIXTURE=1 \
		-c "$rust_root/sv_main_target.c" -o "$OUT/$dir/sv_main_target.o"
	cc "${INCS[@]}" "${DEFS[@]}" "${WARN[@]}" "${SECT[@]}" "$@" \
		-c "$here/init_harness.c" -o "$OUT/$dir/init_harness.o"
	cc "$@" -Wl,--gc-sections -o "$OUT/$dir/init_harness" \
		"$OUT/$dir/init_harness.o" "$OUT/$dir/c_ref.o" \
		"$OUT/$dir/sv_main_target.o" "$RUST_LIB" -lm
}

run() { # dir
	local status=0
	(cd "$OUT/$1" && CASES="$CASES" timeout -k 10 "$LIMIT" ./init_harness) || status=$?
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
if [ "$status" -ne 0 ] || ! grep -q '^sv_main_init differential harness (h2): PASS (' "$log"; then
	echo "FAIL: h2 differential harness (exit $status)" >&2
	grep -E 'MISMATCH|FAIL|error|harness bug' "$log" >&2 || tail -40 "$log" >&2
	exit 1
fi

count=$(sed -n 's/^sv_main_init differential harness (h2): PASS (\([0-9]*\) cases)$/\1/p' "$log")
[ -n "$count" ] && [ "$count" -ge 1000 ] || {
	echo "FAIL: harness ran ${count:-no} cases (floor 1000)" >&2
	exit 1
}
line=$(grep -m1 '^outcomes:' "$log" || true)
for key in init protocol_f edicts_f max_edicts edicts fatal file_fail; do
	v=$(sed -n "s/.*$key \([0-9]*\).*/\1/p" <<<"$line")
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
(cd "$OUT/cov" && CASES="$CASES" timeout -k 10 "$LIMIT" ./init_harness >/dev/null 2>&1) || {
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
