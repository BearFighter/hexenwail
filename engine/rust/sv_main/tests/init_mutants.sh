#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Proves the sv_main init differential harness can fail.  Applies each mutant
# to engine/rust/src/sv_main_init.rs in turn, rebuilds the archive with
# sv_main_init, runs run_init_harness.sh without the coverage pass, and
# requires a MISMATCH -- a difference between Rust and the C.  The CONTROL
# only edits a comment and must survive.  The file is restored on exit.
#
#   bash engine/rust/sv_main/tests/init_mutants.sh
#   bash engine/rust/sv_main/tests/init_mutants.sh 3 8
#
# Not part of check-rust-sv-main.sh (it rewrites a tracked source); run it
# when sv_main_init.rs or the harness changes.  CASES sets the random case
# count per run (default 9000).  Needs perl, cargo, cc -- run in `nix develop`.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
crate="$(cd "$here/../.." && pwd)"
src="$crate/src/sv_main_init.rs"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
orig="$work/sv_main_init.rs.orig"
cp "$src" "$orig"
trap 'cp "$orig" "$src"' EXIT
export CASES="${CASES:-9000}"

# kind|name|perl -0 substitution.  CONTROL must survive; every KILL must be
# caught by the harness's serialized call/console/file/state comparison.
# shellcheck disable=SC2016  # perl code, not shell
mutants=(
	'CONTROL|a comment changes|s/sv_main_init.rs -- Hexen II/sv_main_init.rs -- Hexen II (control)/'
	# Max_Edicts_f / SV_Protocol_f
	'KILL|max_edicts warns only when idle|s/if server_active\(\) \{/if !server_active() {/'
	'KILL|protocol query reads one higher|s/let protocol = \*SvMainInit_Protocol\(\);\n    match/let protocol = *SvMainInit_Protocol() + 1;\n    match/'
	'KILL|invalid protocol is accepted|s/if SV_ProtocolName\(i\)\.is_null\(\) \{\n                CON_Printf\(/if false {\n                CON_Printf(/'
	'KILL|userset flag is not raised|s/SvMainInit_SetProtocolUserset\(1\);\n            CON_Printf\(\n                f.print_normal as c_uint,\n                c"sv_protocol set to/SvMainInit_SetProtocolUserset(0);\n            CON_Printf(\n                f.print_normal as c_uint,\n                c"sv_protocol set to/'
	'KILL|usage line is printed normally|s/f.print_safe as c_uint,\n                c"usage: sv_protocol/f.print_normal as c_uint,\n                c"usage: sv_protocol/'
	# SV_Init
	'KILL|gravity callback not installed|s/Cvar_SetCallback\(cvar\(Cvar::Gravity\), Some\(Host_Callback_Notify\)\);/let _ = cvar(Cvar::Gravity);/'
	'KILL|edict callback not installed|s/Cvar_SetCallback\(cvar\(Cvar::MaxEdicts\), Some\(Max_Edicts_f\)\);/let _ = cvar(Cvar::MaxEdicts);/'
	'KILL|sv_edicts command not added|s/Cmd_AddCommand\(c"sv_edicts".as_ptr\(\), Some\(Sv_Edicts_f\)\);/let _ = Sv_Edicts_f as *const ();/'
	'KILL|localmodels start at one|s/for i in 0..f.max_models \{/for i in 1..f.max_models {/'
	'KILL|king of the hill is not reset|s/\*SvMainInit_KingOfHill\(\) = 0;/let _ = SvMainInit_KingOfHill();/'
	'KILL|-protocol argument is ignored|s/if i != 0 && i < SvMainInit_LaunchArgc\(\) - 1 \{/if false { /'
	'KILL|startup banner prints a null protocol name|s/        protocol,\n        p,\n    \);/        protocol,\n        ptr::null::<c_char>(),\n    );/'
	# SV_Edicts / Sv_Edicts_f
	'KILL|file open failure is not reported|s/if file.is_null\(\) \{\n        CON_Printf\(/if false {\n        CON_Printf(/'
	'KILL|edict listing skips the first edict|s/let mut i: c_int = 1;/let mut i: c_int = 0;/'
	'KILL|think function read from touch|s/read_i32\(e, f.edict_v \+ f.v_think\)/read_i32(e, f.edict_v + f.v_touch)/'
	'KILL|default dump name is wrong|s/c"edicts.txt".as_ptr\(\)/c"edicts-old.txt".as_ptr()/'
	'KILL|idle edicts command is allowed|s/if !server_active\(\) \{\n        CON_Printf\(\n            facts\(\).print_normal as c_uint,\n            c"This command/if server_active() {\n        CON_Printf(\n            facts().print_normal as c_uint,\n            c"This command/'
)

# select by 1-based index, or all
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
		--target-dir "$work/target" --features sv_main_init >"$work/build.log" 2>&1; then
		cat "$work/build.log" >&2
		echo "BROKEN MUTANT #$n (does not compile): $name" >&2
		exit 2
	fi
	rc=0
	HARNESS_COVERAGE=0 RUST_LIB="$lib" OUT="$work/run" \
		bash "$here/run_init_harness.sh" >"$work/run.log" 2>&1 || rc=$?
	# The printf-with-null mutant puts NUL bytes in the log; grep -a keeps
	# those runs comparable instead of reporting "binary file matches".
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
