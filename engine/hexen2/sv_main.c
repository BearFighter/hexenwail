/*
 * sv_main.c -- server main program
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "ctl.h"	/* crawler-fork: engine-patches/ctl */

server_t	sv;
server_static_t	svs;

/* not static: engine/rust/sv_main_target.c reads it for SV_SpawnServer (E2 / slice E) */
char	localmodels[MAX_MODELS][8];	// inline model names for precache

/* not static: engine/rust/sv_main_target.c names it in SvMainInit_Cvar, which
 * is how slice F reaches C-owned cvar storage without assuming cvar_t's ABI. */
cvar_t	sv_sound_distance	= {"sv_sound_distance", "800", CVAR_NONE};
						/* doesn't seem functional, but the hcode calls it */

/* not static: engine/rust/sv_main_target.c reads them, and sv_netsort below,
 * for sv_main_ents.rs */
cvar_t	sv_update_player	= {"sv_update_player", "1", CVAR_ARCHIVE};
cvar_t	sv_update_monsters	= {"sv_update_monsters", "1", CVAR_ARCHIVE};
cvar_t	sv_update_missiles	= {"sv_update_missiles", "1", CVAR_ARCHIVE};
cvar_t	sv_update_misc		= {"sv_update_misc", "1", CVAR_ARCHIVE};

/* Ironwail's sv_netsort.  Decides WHICH entity updates survive when a frame's
 * deltas do not all fit in the datagram: nearest-and-largest first rather than
 * whatever happened to come last in edict order.  0 restores the edict-order
 * truncation, for comparison.  Not archived, as upstream. */
cvar_t	sv_netsort		= {"sv_netsort", "1", CVAR_NONE};

/* Engine identity for HexenC mods -- issue #279, asked for by Mathuzzz.
 *
 * A mod that wants to light up engine-specific content has no portable way to
 * ask which engine it is on.  checkextension() cannot do it: that builtin is
 * #130 here and #99 in DP/FTE, so the number a mod writes against means two
 * different things.  cvar() is #45 everywhere, including FTE's Hexen II
 * support, so a numerically-valued cvar is the one probe that compiles and
 * behaves identically on every engine -- and reads 0 on all of them but this
 * one, because an unknown cvar reads as 0.
 *
 * CVAR_ROM so a player cannot set it and fake the answer, and deliberately NOT
 * CVAR_ARCHIVE: a value written into config.cfg by one build would be read
 * back by another and would then be lying about the engine it is in.  The
 * value is stamped from HW_VERSION_NUM at registration rather than typed here,
 * so it cannot drift from HW_BASE_VERSION; engine/tests/version_cvar_test.c
 * fails the build if the two ever disagree.
 *
 * Registered in SV_Init, which both the client and h2ded run, because the mod
 * asking is server-side QuakeC. */
cvar_t	hexenwail_version	= {"hexenwail", "0", CVAR_ROM};

/* Deliberately not CVAR_ARCHIVE, as upstream: a ceiling silently persisted
 * into a config is how you get a mod that only fails on one machine.  Takes
 * effect at the next map load, since sv.edicts is sized once there.
 *
 * Ironwail's default and ceiling: 16384, clamped into MAX_EDICTS 32000.  The
 * ceiling was 8192 until GitHub #411, so this could only LOWER the limit;
 * the costs of raising it are in the note on MIN_EDICTS in quakedef.h, and
 * entities past 8192 make sounds only on protocol 100 (SV_SoundEntityFits). */
cvar_t	max_edicts		= {"max_edicts", "16384", CVAR_NONE};

/* Cap on the per-frame unreliable datagram, in bytes; 0 (the default) is no
 * cap beyond what the connection and protocol allow (SV_SendClientDatagram).
 * MAX_DATAGRAM is 4096, well over a 1500-byte path MTU, so on a real link
 * every full datagram is IP-fragmented in three and losing any one fragment
 * loses the frame; a frame is re-sent next tick anyway, so a smaller one that
 * arrives beats a larger one that does not.  ~1400 suits such links; sv_netsort
 * decides what to shed when the budget tightens.  Applies to local
 * connections too, which makes it the test lever for the shedding path that
 * stock maps never reach.  Floored at SV_MAXDATAGRAM_FLOOR so the
 * svc_clear_edicts reserve still leaves room for entities.  GitHub #82. */
cvar_t	sv_maxdatagram		= {"sv_maxdatagram", "0", CVAR_NONE};
#define	SV_MAXDATAGRAM_FLOOR	MAX_DATAGRAM_OLD	/* 1024: what protocols 18-20 always had */

/* Registered but never read by the engine, exactly as upstream, where it exists
 * so the 2021 rerelease's progs find a cvar to query rather than nothing.  No
 * Hexen II gamecode reads it either; it is here for parity and for a mod that
 * wants somewhere to hang the convention.  Hexen II's own cheat gate is the
 * deathmatch/coop/skill test inside each cheat command, and this does not
 * change it. */
cvar_t	sv_cheats		= {"sv_cheats", "0", CVAR_NONE};

/* crawler-fork: engine-patches/depraved-class.  Slot 5 is the mission pack's
 * Demoness, or, when a game sets depraved_class, our own Depraved class,
 * available without the mission pack and with any progs.dat.  Not archived:
 * the game sets it from its own autoexec.cfg. */
cvar_t	depraved_class		= {"depraved_class", "0", CVAR_NONE};

/* PlayerClass_Count is Rust now (#469 slice G).  Its C original is kept
 * verbatim in engine/rust/sv_main/tests/sv_main_class_h2.c for the differential
 * harness; the cvar and gameflags it reads stay C-owned. */

/* max_edicts resolved and clamped, latched at map load.  Read this, not the
 * cvar, so a mid-map change cannot outrun the allocation. */
int	sv_max_edicts = MAX_EDICTS;

/* Set when QC filled sv.datagram past the point where the next write would be
 * fatal.  Cleared every frame by SV_ClearDatagram.  See WriteDest in pr_cmds.c. */
qboolean	sv_datagram_dropped = false;

cvar_t	sv_ce_scale		= {"sv_ce_scale", "0", CVAR_ARCHIVE};
cvar_t	sv_ce_max_size		= {"sv_ce_max_size", "0", CVAR_ARCHIVE};

extern	cvar_t	sv_maxvelocity;
extern	cvar_t	sv_gameplayfix_elevators;	/* sv_phys.c */
extern	cvar_t	sv_freezenonclients;		/* sv_phys.c */
extern	cvar_t	sv_gameplayfix_random;		/* pr_cmds.c */
/* pr_checkextension is declared in progs.h, next to the registry it gates. */
extern	cvar_t	sv_gravity;
extern	cvar_t	sv_nostep;
extern	cvar_t	sv_friction;
extern	cvar_t	sv_edgefriction;
extern	cvar_t	sv_stopspeed;
extern	cvar_t	sv_maxspeed;
extern	cvar_t	sv_accelerate;
extern	cvar_t	sv_altnoclip;
extern	cvar_t	sv_idealpitchscale;
extern	cvar_t	sv_idealrollscale;
extern	cvar_t	sv_aim;
extern	cvar_t	sv_walkpitch;
extern	cvar_t	sv_flypitch;
extern	cvar_t	sv_debugmovestep;
extern void SV_DebugMoveStep_Changed (cvar_t *var);

int		current_skill;
int		sv_protocol = PROTOCOL_VERSION;	/* protocol version to use */

/* Whether sv_protocol got its value from the operator (-protocol or the
 * console verb) rather than from the default.  The auto-detect in
 * SV_SpawnServer used to test `sv_protocol == PROTOCOL_VERSION` for this,
 * which cannot distinguish "nobody chose" from "chose 19 on purpose" -- so
 * asking a server for maximum compatibility silently got you upgraded off it
 * again by the first mod whose progs tripped the sniff.  That is the exact
 * failure this protocol split exists to remove, so it has to be a separate
 * bit rather than an inference from the value.  Not static:
 * engine/rust/sv_main_target.c reads it for SV_SpawnServer (E2 / slice E). */
qboolean	sv_protocol_userset = false;
int		sv_kingofhill;		/* mission pack, king of the hill. */
unsigned int	info_mask, info_mask2;	/* mission pack, objectives */

//============================================================================
//
// SV_Init, SV_Protocol_f, Max_Edicts_f, SV_Edicts and Sv_Edicts_f are
// engine/rust/src/sv_main_init.rs (F / slice F of #469).  Their C original is
// engine/rust/sv_main/tests/sv_main_init_h2.c, and the differential harness
// there runs the port against it.  The cvars, sv, sv_protocol,
// sv_protocol_userset, sv_kingofhill and localmodels stay declared here and
// C-owned; engine/rust/sv_main_target.c hands their addresses to Rust.
//
//============================================================================
/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/* SV_StartParticle..4, SV_StopSound, SV_UpdateSoundPos and SV_StartSound --
 * the sound and particle messages -- and SV_MaxSounds, SV_MaxDatagram and
 * SV_MaxReliableMessage are engine/rust/src/sv_main_sound.rs (#469 slice A
 * of #455); the C originals are engine/rust/sv_main/tests/sv_main_sound_h2.c.
 * Declared in server.h. */

/* SV_ParseWorldspawnSky -- ported to Rust: engine/rust/src/sv_main_inv.rs
 * (#469 slice E); the C original is
 * engine/rust/sv_main/tests/sv_main_inv_h2.c.  Was static; its only caller,
 * SV_SpawnServer, is engine/rust/src/sv_main_spawn.rs.  Declared in server.h. */

/* SV_ClientInventoryPage and INV_UpdateExItem -- ported to Rust: engine/rust/src/sv_main_inv.rs
 * (#469 slice E); the C original is
 * engine/rust/sv_main/tests/sv_main_inv_h2.c.  Declared in server.h; also called from
 * host_cmd.c, pr_cmds.c, cl_parse.c and SV_SpawnServer (sv_main_spawn.rs). */

/*
==============================================================================

CLIENT SPAWNING

==============================================================================
*/

/* SV_SendServerinfo -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */

/* SV_ConnectClient -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Was static; only SV_CheckForNewClients called it. */


/* SV_CheckForNewClients -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h; called from host.c. */


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

/*
 * SV_ClearDatagram -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/* The entity updates -- the fat PVS, SV_EntityUpdateSize, SV_EntityPriority,
 * SV_PrepareClientEntities and SV_CleanupEnts -- are
 * engine/rust/src/sv_main_ents.rs (#469 slice B of #455); the C originals are
 * engine/rust/sv_main/tests/sv_main_ents_h2.c.  The fat PVS buffer is
 * engine/rust/sv_main_target.c's.  SV_SendClientDatagram and
 * SV_SendClientMessages (sv_main_datagram.rs) call these two. */

/*
 * SV_WriteClientdataToMessage -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_SendClientDatagram -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_UpdateToReliableMessages -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */


/*
 * SV_SendNop -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */

/*
 * SV_SendClientMessages -- engine/rust/src/sv_main_datagram.rs (#469 slice C of #455);
 * the C original is engine/rust/sv_main/tests/sv_main_datagram_h2.c.
 * Declared in server.h where the engine needs it.
 */


/*
==============================================================================

SERVER SPAWNING

==============================================================================
*/

/* SV_ModelIndex -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h. */

/* SV_SignonBufferCount and SV_SignonBufferSize -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Were static; only SV_SendSignonBuffer called them. */

/* SV_ReserveSignonSpace -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h; also called from pr_cmds.c. */

/* SV_SendSignonBuffer -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Declared in server.h. */

/* SV_CreateBaseline -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */


/* SV_SendReconnect -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Called from SV_SpawnServer, sv_main_spawn.rs (E2 / slice E). */


/* SV_GetLevelname -- ported to Rust: engine/rust/src/sv_main_inv.rs
 * (#469 slice E); the C original is
 * engine/rust/sv_main/tests/sv_main_inv_h2.c.  Declared in server.h; also called from host_cmd.c
 * and SV_PrintMapChecklist (sv_main_spawn.rs). */

/* SV_SaveSpawnparms -- ported to Rust: engine/rust/src/sv_main_inv.rs
 * (#469 slice E); the C original is
 * engine/rust/sv_main/tests/sv_main_inv_h2.c.  Declared in server.h; called from host_cmd.c. */

/* INV_WritePage, SV_LoadInventory and INV_SavePages -- ported to Rust: engine/rust/src/sv_main_inv.rs
 * (#469 slice E); the C original is
 * engine/rust/sv_main/tests/sv_main_inv_h2.c.  Declared in server.h; the savegame
 * code in host_cmd.c calls them on its FILE *. */

/* E2 / slice E of #469: the map_checks readout -- SV_MapCheckThresh,
 * SV_MapCheckTitle, SV_PrintMapChecklist -- and SV_SpawnServer are
 * engine/rust/src/sv_main_spawn.rs; the C originals, with the MAP CHECKLIST
 * note above them, are engine/rust/sv_main/tests/sv_main_spawn_h2.c.
 * SV_PrintMapCheck is C-variadic and stays C, in
 * engine/rust/sv_main_variadic.c; mapcheck_t moved to
 * engine/rust/sv_main_target.h.  SV_SpawnServer is declared in server.h. */
