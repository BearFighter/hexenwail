/*
 * sv_main_class_h2.c -- the C original of sv_main.c's PlayerClass_Count, moved
 * verbatim out of engine/hexen2/sv_main.c.  The differential harness beside
 * this file includes it; no engine target compiles it.  The engine's
 * implementation is engine/rust/src/sv_main_class.rs (slice G of the #469
 * server oxidation).
 *
 * (from sv_main.c -- server main program)
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

/* sv_main.c's file-scope storage from above the moved text.  The harness owns
 * both objects; these are only the declarations the function needs. */
extern	cvar_t		depraved_class;
extern	unsigned int	gameflags;

/*============================================================================*/

int PlayerClass_Count (void)
{
	if (depraved_class.integer || (gameflags & GAME_PORTALS))
		return MAX_PLAYER_CLASS;
	return MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
}
