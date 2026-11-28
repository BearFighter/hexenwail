# Rust Next Slice Survey: Hexen II `#469` after Slice F

**Worktree**: `F:\Hexen2\HEXENRUST\engine-src\.worktrees\rust-parallel` (`worktree-rust-parallel`, based on `80f1c8f17`)  
**Scope**: Read-only survey of `engine/hexen2/sv_main.c` and existing `engine/rust/src/sv_main_*.rs` ports/harnesses. No C/Rust/CMake/Cargo/script/gate files changed.  
**Excluded as Slice F / in-flight elsewhere**: `SV_Init`, `SV_Protocol_f`, `Max_Edicts_f`, `SV_Edicts`, `Sv_Edicts_f` (`engine/hexen2/sv_main.c:164-367`, declarations/commands/callback wiring).

## Current port map (from `sv_main.c` comments)

| Slice | Rust module | C original fixture | Harness/runner | Exported/ported symbols |
|---|---|---|---|---|
| A sound/particles/limits | `engine/rust/src/sv_main_sound.rs` | `sv_main/tests/sv_main_sound_h2.c` | `diff_harness.c` / `run_diff_harness.sh` | `SV_StartParticle*`, `SV_StopSound`, `SV_UpdateSoundPos`, `SV_StartSound`, `SV_MaxSounds`, `SV_MaxDatagram`, `SV_MaxReliableMessage` (`sv_main.c:379-382`) |
| B entity updates | `sv_main_ents.rs` | `sv_main_ents_h2.c` | `ents_harness.c` / `run_ents_harness.sh` | fat PVS helpers, `SV_EntityUpdateSize`, `SV_EntityPriority`, `SV_PrepareClientEntities`, `SV_CleanupEnts` (`:429-435`) |
| C datagrams/clientdata | `sv_main_datagram.rs` | `sv_main_datagram_h2.c` | `datagram_harness.c` / `run_datagram_harness.sh` | `SV_ClearDatagram`, `SV_WriteClientdataToMessage`, `SV_SendClientDatagram`, `SV_UpdateToReliableMessages`, `SV_SendNop`, `SV_SendClientMessages` (`:424,437,443,449,456,462`) |
| D connect/signon | `sv_main_connect.rs` | `sv_main_connect_h2.c` | `connect_harness.c` / `run_connect_harness.sh` | `SV_ProtocolName`, `SV_SendServerinfo`, `SV_ConnectClient`, `SV_CheckForNewClients`, `SV_ModelIndex`, signon buffer helpers, `SV_CreateBaseline`, `SV_SendReconnect` (`:402,406,411,477,481,485,489,493,498`) |
| E1 inv/sky/save helpers | `sv_main_inv.rs` | `sv_main_inv_h2.c` | `inv_harness.c` / `run_inv_harness.sh` | `SV_ParseWorldspawnSky`, `SV_ClientInventoryPage`, `INV_UpdateExItem`, `SV_GetLevelname`, `SV_SaveSpawnparms`, `INV_WritePage`, `SV_LoadInventory`, `INV_SavePages` (`:384,389,503,508,512`) |
| E2 spawn/map checklist | `sv_main_spawn.rs` | `sv_main_spawn_h2.c` | `spawn_harness.c` / `run_spawn_harness.sh` | `SV_SpawnServer`, `SV_PrintMapChecklist`, `SV_MapCheckThresh`, `SV_MapCheckTitle`; `SV_PrintMapCheck` stays C variadic in `engine/rust/sv_main_variadic.c` (`:516-522`) |

Harness pattern: each port has a C fixture copied from original, `c_ref*.c` wrapper renaming symbols to `c_*`, a differential harness comparing serialized state/logs/exit behavior, and `run_*_harness.sh` building against `RUST_LIB` + `sv_main_target.c` (+ `world_target.c` where needed), with gcov coverage. `scripts/check-rust-sv-main.sh` runs selected parts, enforces coverage/floors, and checks symbol placement in built binaries and absence from Hexen II `sv_main.c.o`.

## Remaining `sv_main.c` definitions

After excluding Slice F functions, the only remaining C definition in `engine/hexen2/sv_main.c` is:

| Candidate | Lines | Signature / behavior | Callers / visibility |
|---|---:|---|---|
| C1 `PlayerClass_Count` | 106-112 | Returns `MAX_PLAYER_CLASS` if `depraved_class.integer` or `(gameflags & GAME_PORTALS)` else `MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES`. | Declared in `engine/hexen2/server.h:367`; used widely in Hexen II client/UI code (`menu.c`, `sbar.c`) — not HexenWorld. |

No other function definitions remain; the rest of `sv_main.c` is file-scope state/cvars and comments pointing at Rust ports.

## Candidate ranking

| Rank | Candidate | Safety | Why |
|---:|---|---|---|
| 1 (only) | C1 `PlayerClass_Count` | **Medium-low for isolation, but blocked by wiring** | Tiny pure-ish query; no file I/O, no network, no edict mutation. Risk is not logic size but integration surface: it is a general engine symbol used outside server frame and currently not part of sv_main Rust port wiring. |

## C1 assessment: `PlayerClass_Count`

- **Exact function**: `int PlayerClass_Count(void)` at `engine/hexen2/sv_main.c:106-112`.
- **Callees that must stay C**: none inside function; it reads C-owned globals/cvars.
- **Mutable state that must stay C-owned**: `depraved_class` cvar object/integer (`sv_main.c:104`), global `gameflags` (`engine/h2shared/quakefs.h:64` extern; defined in filesystem/gameflags code). Constants come from headers: `MAX_PLAYER_CLASS`, `PORTALS_EXTRA_CLASSES` (`engine/hexen2/quakedef.h`), `GAME_PORTALS` (`engine/h2shared/quakefs.h`).
- **Target facts/accessors needed**: expose `depraved_class.integer`, `gameflags`, and constants (`MAX_PLAYER_CLASS`, `PORTALS_EXTRA_CLASSES`, `GAME_PORTALS`) through a target facts struct/accessors or a narrow ABI. Must confirm Hexen II-only target visibility; symbol is declared in Hexen II `server.h`.
- **Harness shape**: small differential harness comparing int return over matrix of `depraved_class.integer` in {0,nonzero} x `gameflags` with/without `GAME_PORTALS`, including unrelated bits set. Could follow existing `sv_main` harness style (C fixture + Rust export + compare logs/return), but coverage floor is trivial.
- **Specific gate/wiring files that would have to change**: at minimum `engine/rust/src/lib.rs` (new module/feature), `engine/rust/Cargo.toml` (feature), `engine/rust/sv_main_target.c/.h` (facts/accessors), `scripts/check-rust-sv-main.sh` (part/symbol checks/floors), `scripts/check-rust-archives.sh` and/or `scripts/check-rust-abi.sh` as applicable, `engine/CMakeLists.txt` (feature list / target sources / symbol expectations), and likely `engine/hexen2/sv_main.c` removal/extern adjustments plus test fixtures under `engine/rust/sv_main/tests/`. **This conflicts with the task constraint to not touch shared wiring/gate files.**

## Recommendation

**Recommended: no code-port slice now; produce this survey only.** The only post-Slice-F function left in Hexen II `sv_main.c` is `PlayerClass_Count`, and safely landing it as a Rust `#469` slice would require shared wiring/gate/target changes listed above. Per instructions, I am stopping short of edits and reporting the blocker.

If wiring changes become explicitly in-scope in a dedicated session, C1 is the only remaining `sv_main.c` candidate; it should be handled as a tiny Hexen II-only utility port with careful symbol ownership checks because it is called from client/UI translation units, not just server frame paths.

## Detailed evidence notes

### `sv_main.c` function inventory
- `PlayerClass_Count` (`sv_main.c:106-112`): only remaining substantive function definition not marked ported and not in excluded Slice F set. Reads `depraved_class.integer` and `gameflags`; returns 4 or 5 depending on mission-pack/gameflag/depraved mode.
- `SV_ProtocolName` (`sv_main.c:178` declaration, comment at 173-177): ported to Rust `sv_main_connect.rs`; C symbol remains visible for C callers including `SV_Protocol_f` and startup checks.
- `SV_Init` (`:238-315`), `SV_Protocol_f` (`:199-231`), `Max_Edicts_f` (`:166-171`), `SV_Edicts` (`:317-345`), `Sv_Edicts_f` (`:347-367`): explicitly excluded by task as Slice F/in-flight.
- No other `SV_*`/function definitions appear after scan; file tail is port comments for sound/inv/connect/datagram/ents/sign-on/spawn slices.

### Existing Rust sv_main modules and dependencies (`engine/rust/Cargo.toml`)
- `sv_main_sound = ["world_hull","msg_io","sizebuf"]`
- `sv_main_ents = ["sv_main_sound"]`
- `sv_main_datagram = ["sv_main_ents"]`
- `sv_main_connect = ["sv_main_sound"]`
- `sv_main_inv = ["sv_main_sound"]`
- `sv_main_spawn = ["world_hull"]`
All are Hexen II-only ports; gate script `scripts/check-rust-sv-main.sh` enumerates parts `sound ents datagram connect inv spawn`.

### Wiring/gate footprint observed in existing slices
- `engine/rust/src/lib.rs`: feature-gated `pub mod sv_main_*`.
- `engine/rust/sv_main_target.c/.h`: facts/accessors/globals for slices A-E2; compiled into targets linking `sv_main_sound` feature (`engine/CMakeLists.txt` finds `sv_main_sound` arg) and also has spawn variadic C file when `sv_main_spawn` selected.
- `engine/CMakeLists.txt`: `RUST_ENGINE_H2_SERVER_FEATURES` contains all six sv_main features; client/h2ded feature lists append them; hwsv has own server code.
- `scripts/check-rust-sv-main.sh`: builds per-part crate with feature, runs harness script, checks coverage/floors, verifies symbol definitions in `hexenwail`, `h2ded`, `hwsv`, and confirms Hexen II `sv_main.c.o` does not define ported symbols.
- Harness scripts live in `engine/rust/sv_main/tests/run_*_harness.sh`; fixtures include `*_h2.c`, `c_ref*.c`, harness C, mutants scripts for some slices.

### Why C1 is not a safe "just write report then port later in same constraints" item
`PlayerClass_Count` is exported in `engine/hexen2/server.h` and called from Hexen II UI/client code (`menu.c`, `sbar.c`), so moving ownership to Rust would touch symbol/export/wiring expectations beyond `sv_main.c`. Even a tiny differential harness would require adding module/feature/gate entries and target facts/accessors. That crosses the explicit "no shared wiring files (`Cargo.toml`, `lib.rs`, gate scripts, `sv_main_target.*`, CMake)" constraint.

## Candidate details

### C1. `PlayerClass_Count` (only remaining candidate)
- **Lines/signature**: `engine/hexen2/sv_main.c:106-112`, `int PlayerClass_Count(void)`.
- **Behavior**: returns 5 when mission pack/portals active (`gameflags & GAME_PORTALS`) or `depraved_class.integer` set; otherwise 4. Depends on Hexen II class constants.
- **Callees that must stay C**: none called by function; reads C globals/cvar state.
- **Mutable C-owned state**: `depraved_class` cvar storage and `gameflags` global; both engine-owned mutable state outside function.
- **Needed target facts/accessors**: `MAX_PLAYER_CLASS`, `PORTALS_EXTRA_CLASSES`, `GAME_PORTALS`, `depraved_class.integer`, `gameflags` value; possibly a narrow accessor `SvMain_PlayerClassCountInputs()` to avoid exposing unrelated globals.
- **Harness shape**: table-driven differential cases over gameflags bit patterns and depraved cvar values; assert return equals C. Can run in host test binary with stubbed globals; no filesystem/network needed.
- **Gate/wiring files required if implemented**: `engine/rust/Cargo.toml`, `engine/rust/src/lib.rs`, `engine/rust/sv_main_target.c`, `engine/rust/sv_main_target.h`, `engine/rust/sv_main/tests/*` new harness/fixture/runner, `scripts/check-rust-sv-main.sh`, possibly `scripts/check-rust-archives.sh`/ABI tests, `engine/CMakeLists.txt`, and `engine/hexen2/sv_main.c`/headers as needed.
- **Risk**: medium-low logic risk, **high process/integration risk** under current constraints because required files are shared wiring/gates.

### Rejected / excluded
- **Slice F functions** (`SV_Init`, `SV_Protocol_f`, `Max_Edicts_f`, `SV_Edicts`, `Sv_Edicts_f`, `sv_main.c:164-367`): excluded by user; also init/console/callback-heavy and touch command/cvar registration, file I/O (`SV_Edicts` writes file), and startup protocol selection.
- **Already-ported slices A-E2**: not candidates; `sv_main.c` contains only comments/markers for them.
- **HexenWorld sv_main.c**: out of scope; task specified Hexen II `engine/hexen2/sv_main.c`.

## Final recommendation

**No safe next Hexen II `#469` `sv_main.c` code slice is available under the stated constraints.** After excluding Slice F, only `PlayerClass_Count` remains in `sv_main.c`, and porting it would require shared wiring/gate/CMake/target changes. The safest action was this read-only survey; any follow-up should be a dedicated wiring-in-scope micro-slice for `PlayerClass_Count` with explicit symbol-ownership checks across Hexen II client and server targets.
