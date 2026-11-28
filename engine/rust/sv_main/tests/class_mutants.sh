#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the player-class differential harness can fail.  Applies each mutant to
# engine/rust/src/sv_main_class.rs in turn, rebuilds the archive with
# sv_main_class, runs run_class_harness.sh without the coverage pass, and
# requires a MISMATCH.  The CONTROL only edits a comment and must survive.  The
# file is restored on exit.
#
#   bash engine/rust/sv_main/tests/class_mutants.sh
#   bash engine/rust/sv_main/tests/class_mutants.sh 2 4
#
# Not part of check-rust-sv-main.sh (it rewrites a tracked source); run it when
# sv_main_class.rs or its harness changes.  CASES sets the case count per run
# (default 4096).  Needs perl, cargo, cc -- run in `nix develop`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/../.." && pwd)"
src="$crate/src/sv_main_class.rs"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
orig="$work/sv_main_class.rs.orig"
cp "$src" "$orig"
trap 'cp "$orig" "$src"' EXIT
export CASES="${CASES:-4096}"

# kind|name|perl -0 substitution.  CONTROL must survive; every KILL must be
# caught by the return-value comparison.
# shellcheck disable=SC2016  # perl code, not shell
mutants=(
	'CONTROL|a comment changes|s/class handling/class handling (control)/'
	'KILL|depraved_class is ignored|s/if depraved != 0 \|\|/if false ||/'
	'KILL|GAME_PORTALS is ignored|s/game_portals\) != 0/game_portals) == 0/'
	'KILL|mission pack class is always offered|s/f\.max_player_class - f\.portals_extra_classes/f.max_player_class/'
	'KILL|extra class is offered when enabled too|s/f\.max_player_class\n    \} else/f.max_player_class - f.portals_extra_classes\n    } else/'
)

if [ $# -gt 0 ]; then
	sel=("$@")
else
	sel=()
	for i in "${!mutants[@]}"; do sel+=("$((i + 1))"); done
fi

lib="$work/target/release/libengine_rs.a"
killed=0 control=0 bad=0
for n in "${sel[@]}"; do
	m="${mutants[$((n - 1))]}"
	kind="${m%%|*}"; rest="${m#*|}"
	name="${rest%%|*}"; sub="${rest#*|}"
	perl -0pe "$sub" "$orig" >"$src"
	if cmp -s "$src" "$orig"; then
		echo "BROKEN MUTANT #$n (no change): $name" >&2
		exit 2
	fi
	if ! cargo build --release --offline --manifest-path "$crate/Cargo.toml" \
		--target-dir "$work/target" --features sv_main_class >"$work/build.log" 2>&1; then
		cat "$work/build.log" >&2
		echo "BROKEN MUTANT #$n (does not compile): $name" >&2
		exit 2
	fi
	rc=0
	HARNESS_COVERAGE=0 RUST_LIB="$lib" OUT="$work/run" \
		bash "$here/run_class_harness.sh" >"$work/run.log" 2>&1 || rc=$?
	if grep -aq '^MISMATCH' "$work/run.log"; then
		verdict=killed
		why=$(grep -am1 '^MISMATCH' "$work/run.log")
	elif [ "$rc" -ne 0 ]; then
		verdict=other
		why=$(grep -am1 -E 'FAIL|error' "$work/run.log" || tail -1 "$work/run.log")
	else
		verdict=survived
		why=""
	fi
	case "$kind:$verdict" in
	KILL:killed) killed=$((killed + 1)); echo "killed     #$n: $name -- $why" ;;
	CONTROL:survived) control=$((control + 1)); echo "control    #$n: $name (survives)" ;;
	*) bad=$((bad + 1)); echo "UNEXPECTED #$n [$kind]: $name -- $verdict ${why}" ;;
	esac
done
cp "$orig" "$src"
echo "mutants: ${#sel[@]} run, $killed killed, $control control, $bad unexpected"
[ "$bad" -eq 0 ] || { echo "FAIL: $bad mutant(s) did not behave as declared" >&2; exit 1; }
echo "PASS"
