// SPDX-License-Identifier: GPL-2.0-or-later
//
// sv_main_init.rs -- Hexen II's SV_Init, SV_Protocol_f, Max_Edicts_f,
// SV_Edicts and Sv_Edicts_f, moved out of engine/hexen2/sv_main.c (F / slice
// F of #469).  The C original is
// engine/rust/sv_main/tests/sv_main_init_h2.c and the differential harness in
// the same directory runs this module against it.
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
// All mutable engine state stays in C-owned storage.  cvars, sv, the three
// scalars and localmodels are named by engine/rust/sv_main_target.c's
// accessors; header constants and layouts come from SvMainInit_TargetFacts.
// The engine's own C entry points are called in the original order, so the
// harness can record every side effect without this module owning a global.

#![allow(non_snake_case)]

use core::ffi::{c_char, c_double, c_int, c_uint, c_void};
use core::mem::{offset_of, size_of};
use core::ptr;

#[repr(C)]
struct SvMainInitFacts {
    protocol_raven_111: c_int,
    protocol_raven_112: c_int,
    protocol_uqe_113: c_int,
    protocol_uh2_114: c_int,
    protocol_hexenwail_1: c_int,
    print_normal: c_int,
    print_safe: c_int,
    print_termonly: c_int,
    max_models: c_int,
    localmodel_size: usize,
    sv_active: usize,
    sv_time: usize,
    sv_num_edicts: usize,
    edict_v: usize,
    v_nextthink: usize,
    v_classname: usize,
    v_model: usize,
    v_think: usize,
    v_touch: usize,
    v_use: usize,
    function_size: usize,
    function_s_name: usize,
    hw_version_num: c_double,
}

#[repr(i32)]
#[derive(Clone, Copy)]
enum Cvar {
    MaxVelocity = 0,
    Gravity,
    Friction,
    EdgeFriction,
    StopSpeed,
    MaxSpeed,
    Accelerate,
    AltNoclip,
    IdealPitchScale,
    IdealRollScale,
    Aim,
    NoStep,
    WalkPitch,
    GameplayFixElevators,
    FreezeNonClients,
    MaxEdicts,
    SvMaxDatagram,
    Cheats,
    GameplayFixRandom,
    PrCheckExtension,
    HexenWailVersion,
    FlyPitch,
    DebugMoveStep,
    SoundDistance,
    UpdatePlayer,
    UpdateMonsters,
    UpdateMissiles,
    UpdateMisc,
    NetSort,
    CeScale,
    CeMaxSize,
}

type CvarCallback = unsafe extern "C" fn(*mut c_void);
type Command = unsafe extern "C" fn();

extern "C" {
    static SvMainInit_TargetFacts: SvMainInitFacts;

    fn SvMainInit_Cvar(index: c_int) -> *mut c_void;
    fn SvMainInit_Argc() -> c_int;
    fn SvMainInit_Argv(i: c_int) -> *const c_char;
    fn SvMainInit_LaunchArgc() -> c_int;
    fn SvMainInit_LaunchArgv(i: c_int) -> *const c_char;
    fn SvMainInit_Server() -> *mut u8;
    fn SvMainInit_Protocol() -> *mut c_int;
    fn SvMainInit_SetProtocolUserset(v: c_int);
    fn SvMainInit_KingOfHill() -> *mut c_int;
    fn SvMainInit_LocalModels() -> *mut c_char;
    fn SvMainInit_Functions() -> *mut u8;
    fn SvMainInit_Edicts() -> *mut u8;
    fn SvMainInit_EdictSize() -> c_int;
    fn SvMainInit_OpenFile(name: *const c_char) -> *mut c_void;
    fn SvMainInit_PrintFile(file: *mut c_void, fmt: *const c_char, ...) -> c_int;
    fn SvMainInit_CloseFile(file: *mut c_void);

    fn Cvar_RegisterVariable(variable: *mut c_void);
    fn Cvar_SetCallback(variable: *mut c_void, callback: Option<CvarCallback>);
    fn Cvar_SetValueROM(name: *const c_char, value: f32);
    fn Cmd_AddCommand(name: *const c_char, function: Option<Command>);
    fn COM_CheckParm(parm: *const c_char) -> c_int;
    fn CON_Printf(flags: c_uint, fmt: *const c_char, ...) -> c_int;
    fn Sys_Error(fmt: *const c_char, ...) -> !;
    fn SV_ProtocolName(protocol: c_int) -> *const c_char;
    fn PR_GetString(num: c_int) -> *const c_char;
    fn Host_Callback_Notify(var: *mut c_void);
    fn SV_DebugMoveStep_Changed(var: *mut c_void);
    fn SV_UserInit();
    fn atoi(s: *const c_char) -> c_int;
    fn snprintf(buf: *mut c_char, size: usize, fmt: *const c_char, ...) -> c_int;
}

#[inline]
unsafe fn facts() -> &'static SvMainInitFacts {
    &SvMainInit_TargetFacts
}

#[inline]
unsafe fn cvar(index: Cvar) -> *mut c_void {
    SvMainInit_Cvar(index as c_int)
}

#[inline]
unsafe fn read_i32(base: *mut u8, offset: usize) -> c_int {
    ptr::read_unaligned(base.add(offset) as *const c_int)
}

#[inline]
unsafe fn read_f32(base: *mut u8, offset: usize) -> f32 {
    ptr::read_unaligned(base.add(offset) as *const f32)
}

#[inline]
unsafe fn read_f64(base: *mut u8, offset: usize) -> c_double {
    ptr::read_unaligned(base.add(offset) as *const c_double)
}

#[inline]
unsafe fn server_active() -> bool {
    read_i32(SvMainInit_Server(), facts().sv_active) != 0
}

#[inline]
unsafe fn print(flags: c_int, fmt: &'static core::ffi::CStr, value: c_int, extra: *const c_char) {
    // Two shapes cover every line in this slice: a plain literal and a
    // literal with one protocol name.  Variadic C does the formatting, so the
    // bytes are exactly the original's.
    if extra.is_null() {
        CON_Printf(flags as c_uint, fmt.as_ptr(), value);
    } else {
        CON_Printf(flags as c_uint, fmt.as_ptr(), value, extra);
    }
}

#[no_mangle]
pub unsafe extern "C" fn Max_Edicts_f(var: *mut c_void) {
    let _ = var;
    if server_active() {
        CON_Printf(
            facts().print_normal as c_uint,
            c"max_edicts will not take effect until the next map load.\n".as_ptr(),
        );
    }
}

#[no_mangle]
pub unsafe extern "C" fn SV_Protocol_f() {
    let f = facts();
    let protocol = *SvMainInit_Protocol();
    match SvMainInit_Argc() {
        1 => {
            CON_Printf(
                f.print_normal as c_uint,
                c"\"sv_protocol\" is \"%i\" (%s)\n".as_ptr(),
                protocol,
                SV_ProtocolName(protocol),
            );
        }
        2 => {
            let i = atoi(SvMainInit_Argv(1));
            if SV_ProtocolName(i).is_null() {
                CON_Printf(
                    f.print_normal as c_uint,
                    c"sv_protocol must be %i, %i, %i, %i or %i\n".as_ptr(),
                    f.protocol_raven_111,
                    f.protocol_raven_112,
                    f.protocol_uqe_113,
                    f.protocol_uh2_114,
                    f.protocol_hexenwail_1,
                );
                return;
            }
            *SvMainInit_Protocol() = i;
            SvMainInit_SetProtocolUserset(1);
            CON_Printf(
                f.print_normal as c_uint,
                c"sv_protocol set to %i (%s)\n".as_ptr(),
                i,
                SV_ProtocolName(i),
            );
            if server_active() {
                CON_Printf(
                    f.print_normal as c_uint,
                    c"changes will not take effect until the next map load.\n".as_ptr(),
                );
            }
        }
        _ => {
            CON_Printf(
                f.print_safe as c_uint,
                c"usage: sv_protocol <protocol>\n".as_ptr(),
            );
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn SV_Init() {
    let f = facts();

    Cvar_RegisterVariable(cvar(Cvar::MaxVelocity));
    Cvar_RegisterVariable(cvar(Cvar::Gravity));
    Cvar_RegisterVariable(cvar(Cvar::Friction));
    Cvar_SetCallback(cvar(Cvar::Gravity), Some(Host_Callback_Notify));
    Cvar_SetCallback(cvar(Cvar::Friction), Some(Host_Callback_Notify));
    Cvar_RegisterVariable(cvar(Cvar::EdgeFriction));
    Cvar_RegisterVariable(cvar(Cvar::StopSpeed));
    Cvar_RegisterVariable(cvar(Cvar::MaxSpeed));
    Cvar_SetCallback(cvar(Cvar::MaxSpeed), Some(Host_Callback_Notify));
    Cvar_RegisterVariable(cvar(Cvar::Accelerate));
    Cvar_RegisterVariable(cvar(Cvar::AltNoclip));
    Cvar_RegisterVariable(cvar(Cvar::IdealPitchScale));
    Cvar_RegisterVariable(cvar(Cvar::IdealRollScale));
    Cvar_RegisterVariable(cvar(Cvar::Aim));
    Cvar_RegisterVariable(cvar(Cvar::NoStep));
    Cvar_RegisterVariable(cvar(Cvar::WalkPitch));
    Cvar_RegisterVariable(cvar(Cvar::GameplayFixElevators));
    Cvar_RegisterVariable(cvar(Cvar::FreezeNonClients));
    Cvar_RegisterVariable(cvar(Cvar::MaxEdicts));
    Cvar_RegisterVariable(cvar(Cvar::SvMaxDatagram));
    Cvar_SetCallback(cvar(Cvar::MaxEdicts), Some(Max_Edicts_f));
    Cvar_RegisterVariable(cvar(Cvar::Cheats));
    Cvar_RegisterVariable(cvar(Cvar::GameplayFixRandom));
    Cvar_RegisterVariable(cvar(Cvar::PrCheckExtension));
    Cvar_RegisterVariable(cvar(Cvar::HexenWailVersion));
    Cvar_SetValueROM(c"hexenwail".as_ptr(), f.hw_version_num as f32);
    Cvar_RegisterVariable(cvar(Cvar::FlyPitch));
    Cvar_RegisterVariable(cvar(Cvar::DebugMoveStep));
    Cvar_SetCallback(cvar(Cvar::DebugMoveStep), Some(SV_DebugMoveStep_Changed));
    Cvar_RegisterVariable(cvar(Cvar::SoundDistance));
    Cvar_RegisterVariable(cvar(Cvar::UpdatePlayer));
    Cvar_RegisterVariable(cvar(Cvar::UpdateMonsters));
    Cvar_RegisterVariable(cvar(Cvar::UpdateMissiles));
    Cvar_RegisterVariable(cvar(Cvar::UpdateMisc));
    Cvar_RegisterVariable(cvar(Cvar::NetSort));
    Cvar_RegisterVariable(cvar(Cvar::CeScale));
    Cvar_RegisterVariable(cvar(Cvar::CeMaxSize));

    SV_UserInit();

    Cmd_AddCommand(c"sv_edicts".as_ptr(), Some(Sv_Edicts_f));
    Cmd_AddCommand(c"sv_protocol".as_ptr(), Some(SV_Protocol_f));

    let models = SvMainInit_LocalModels();
    for i in 0..f.max_models {
        let row = models.add((i as usize) * f.localmodel_size);
        snprintf(
            row,
            f.localmodel_size,
            c"*%i".as_ptr(),
            i,
        );
    }

    *SvMainInit_KingOfHill() = 0;

    let i = COM_CheckParm(c"-protocol".as_ptr());
    if i != 0 && i < SvMainInit_LaunchArgc() - 1 {
        *SvMainInit_Protocol() = atoi(SvMainInit_LaunchArgv(i + 1));
        SvMainInit_SetProtocolUserset(1);
    }
    let protocol = *SvMainInit_Protocol();
    let p = SV_ProtocolName(protocol);
    if p.is_null() {
        Sys_Error(
            c"Bad protocol version request %i. Accepted values: %i, %i, %i, %i, %i.".as_ptr(),
            protocol,
            f.protocol_raven_111,
            f.protocol_raven_112,
            f.protocol_uqe_113,
            f.protocol_uh2_114,
            f.protocol_hexenwail_1,
        );
    }
    CON_Printf(
        f.print_termonly as c_uint,
        c"Server using protocol %i (%s)\n".as_ptr(),
        protocol,
        p,
    );
}

#[no_mangle]
pub unsafe extern "C" fn SV_Edicts(name: *const c_char) {
    let f = facts();
    let file = SvMainInit_OpenFile(name);
    if file.is_null() {
        CON_Printf(
            f.print_normal as c_uint,
            c"Could not open %s\n".as_ptr(),
            name,
        );
        return;
    }

    let sv = SvMainInit_Server();
    let num_edicts = read_i32(sv, f.sv_num_edicts);
    SvMainInit_PrintFile(
        file,
        c"Number of Edicts: %d\n".as_ptr(),
        num_edicts,
    );
    SvMainInit_PrintFile(
        file,
        c"Server Time: %f\n".as_ptr(),
        read_f64(sv, f.sv_time),
    );
    SvMainInit_PrintFile(file, c"\n".as_ptr());
    SvMainInit_PrintFile(file, c"Num.     Time Class Name                     Model                          Think                                    Touch                                    Use\n".as_ptr());
    SvMainInit_PrintFile(file, c"---- -------- ------------------------------ ------------------------------ ---------------------------------------- ---------------------------------------- ----------------------------------------\n".as_ptr());

    let edicts = SvMainInit_Edicts();
    let edict_size = SvMainInit_EdictSize() as usize;
    let functions = SvMainInit_Functions();
    let mut i: c_int = 1;
    while i < num_edicts {
        let e = edicts.add((i as usize) * edict_size);
        let nextthink = read_f32(e, f.edict_v + f.v_nextthink) as c_double;
        let classname = PR_GetString(read_i32(e, f.edict_v + f.v_classname));
        let model = PR_GetString(read_i32(e, f.edict_v + f.v_model));

        let think = function_name(functions, f, read_i32(e, f.edict_v + f.v_think));
        let touch = function_name(functions, f, read_i32(e, f.edict_v + f.v_touch));
        let use_function = function_name(functions, f, read_i32(e, f.edict_v + f.v_use));

        SvMainInit_PrintFile(
            file,
            c"%3d. %8.2f %-30s %-30s %-40s %-40s %-40s\n".as_ptr(),
            i,
            nextthink,
            classname,
            model,
            think,
            touch,
            use_function,
        );
        i += 1;
    }
    SvMainInit_CloseFile(file);
}

#[inline]
unsafe fn function_name(functions: *mut u8, f: &SvMainInitFacts, index: c_int) -> *const c_char {
    let function = functions.add((index as usize) * f.function_size);
    PR_GetString(read_i32(function, f.function_s_name))
}

#[no_mangle]
pub unsafe extern "C" fn Sv_Edicts_f() {
    if !server_active() {
        CON_Printf(
            facts().print_normal as c_uint,
            c"This command can only be executed on a server running a map\n".as_ptr(),
        );
        return;
    }
    let name = if SvMainInit_Argc() < 2 {
        c"edicts.txt".as_ptr()
    } else {
        SvMainInit_Argv(1)
    };
    SV_Edicts(name);
}

// Layout probes for the differential harness, as in the other sv_main ports.
#[no_mangle]
pub extern "C" fn SvMainInitFacts_sizeof() -> usize {
    size_of::<SvMainInitFacts>()
}

#[no_mangle]
pub extern "C" fn SvMainInitFacts_offsetof_hw_version_num() -> usize {
    offset_of!(SvMainInitFacts, hw_version_num)
}

// Kept so the unused `print` helper cannot drift into a different signature;
// the harness does not call it.  Every console line above spells its flags out
// at the call site, where the original's Con_Printf/Con_SafePrintf macro is
// visible.
#[allow(dead_code)]
unsafe fn _print_helper_is_checked(
    flags: c_int,
    fmt: &'static core::ffi::CStr,
    value: c_int,
    extra: *const c_char,
) {
    print(flags, fmt, value, extra);
}
