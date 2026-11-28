// SPDX-License-Identifier: GPL-2.0-or-later
//
// sv_main_class.rs -- Hexen II's PlayerClass_Count, moved out of
// engine/hexen2/sv_main.c (F / slice G of the #469 server oxidation).  The C
// original is engine/rust/sv_main/tests/sv_main_class_h2.c and the differential
// harness beside it runs this module against it.
//
// Copyright (C) 1996-1997  Id Software, Inc.
// Copyright (C) 1997-1998  Raven Software Corp.
// Copyright (C) 2026 Hexenwail contributors.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or (at
// your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
//
// Both inputs stay C-owned: `depraved_class` is a cvar in sv_main.c and
// `gameflags` belongs to quakefs.  engine/rust/sv_main_target.c reads them for
// this module, so no Rust-exported static is needed on wasm.  The header
// constants come through the same target table; the harness checks them
// against the engine headers before comparing any results.

#![allow(non_snake_case)]

use core::ffi::{c_int, c_uint};
use core::mem::{offset_of, size_of};

#[repr(C)]
struct SvMainClassFacts {
    max_player_class: c_int,
    portals_extra_classes: c_int,
    game_portals: c_uint,
}

extern "C" {
    static SvMainClass_TargetFacts: SvMainClassFacts;

    fn SvMainClass_DepravedClass() -> c_int;
    fn SvMainClass_GameFlags() -> c_uint;
}

#[inline]
unsafe fn facts() -> &'static SvMainClassFacts {
    &SvMainClass_TargetFacts
}

/// The extra mission-pack class is available when a mod opts in with
/// `depraved_class`, or when the Portals game data is detected.  Otherwise the
/// original four classes are offered.  Hexen II's targets only; HexenWorld's
/// server has its own class handling.
#[no_mangle]
pub extern "C" fn PlayerClass_Count() -> c_int {
    let f = unsafe { facts() };
    let depraved = unsafe { SvMainClass_DepravedClass() };
    let flags = unsafe { SvMainClass_GameFlags() };

    if depraved != 0 || (flags & f.game_portals) != 0 {
        f.max_player_class
    } else {
        f.max_player_class - f.portals_extra_classes
    }
}

// Layout probes for the differential harness and the host/wasm ABI gate.
#[no_mangle]
pub extern "C" fn SvMainClassFacts_sizeof() -> usize {
    size_of::<SvMainClassFacts>()
}

#[no_mangle]
pub extern "C" fn SvMainClassFacts_offsetof_game_portals() -> usize {
    offset_of!(SvMainClassFacts, game_portals)
}
