/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * class_harness.c -- engine/rust/src/sv_main_class.rs against the C original of
 * Hexen II's PlayerClass_Count, in one binary.
 *
 * run_class_harness.sh compiles c_ref_class.c with the fixture renamed to
 * c_PlayerClass_Count, compiles engine/rust/sv_main_target.c so the Rust side
 * reads the same C-owned cvar/global, and links both against one Rust archive.
 * Each case sets both inputs, calls the two implementations, and compares the
 * returned class count.  The generated modes reach every arm of the C original
 * and every executable line of the fixture is required under gcov.
 */

#include "quakedef.h"
#include "sv_main_target.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- the ported function, one from C and one from Rust ---- */
int c_PlayerClass_Count (void);
int PlayerClass_Count (void);
size_t SvMainClassFacts_sizeof (void);
size_t SvMainClassFacts_offsetof_game_portals (void);

/* ---- the C-owned inputs the Rust side reads through sv_main_target.c ---- */
cvar_t	depraved_class = {"depraved_class", "0", CVAR_NONE};
unsigned int	gameflags;

static uint64_t mix64 (uint64_t x)
{
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

int main (void)
{
	const char	*env = getenv ("CASES");
	int		cases = env ? atoi (env) : 4096;
	int		i;
	int		cvar_on = 0, portals_on = 0, neither = 0, both = 0;

	if (cases < 1000)
		cases = 1000;

	if (SvMainClass_TargetFacts.max_player_class != MAX_PLAYER_CLASS
	    || SvMainClass_TargetFacts.portals_extra_classes != PORTALS_EXTRA_CLASSES
	    || SvMainClass_TargetFacts.game_portals != (unsigned int) GAME_PORTALS
	    || SvMainClassFacts_sizeof () != sizeof (SvMainClass_TargetFacts)
	    || SvMainClassFacts_offsetof_game_portals ()
	       != offsetof (sv_main_class_facts_t, game_portals))
	{
		printf ("layouts: sv_main_class facts DO NOT match the headers\n");
		return 1;
	}
	printf ("layouts: sv_main_class facts match the headers\n");

	for (i = 0; i < cases; i++)
	{
		uint64_t	r = mix64 ((uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL);
		int		mode = (int)(r & 3);
		int		want, got;

		switch (mode)
		{
		case 0:
			depraved_class.integer = 0;
			gameflags = 0;
			neither++;
			break;
		case 1:
			depraved_class.integer = (int)(r >> 32) | 1;
			gameflags = GAME_DEMO;
			cvar_on++;
			break;
		case 2:
			depraved_class.integer = 0;
			gameflags = GAME_PORTALS | GAME_DEMO;
			portals_on++;
			break;
		default:
			depraved_class.integer = -((int)(r >> 32) | 1);
			gameflags = GAME_PORTALS;
			both++;
			break;
		}

		want = c_PlayerClass_Count ();
		got = PlayerClass_Count ();
		if (want != got)
		{
			printf ("MISMATCH case %d: cvar %d, gameflags 0x%x, C %d, Rust %d\n",
				i, depraved_class.integer, gameflags, want, got);
			return 1;
		}
	}

	printf ("outcomes: cases=%d cvar=%d portals=%d neither=%d both=%d\n",
		cases, cvar_on, portals_on, neither, both);
	printf ("arms (taken/not): cvar %d/%d portals %d/%d\n",
		cvar_on + both, portals_on + neither,
		portals_on + both, cvar_on + neither);
	printf ("sv_main_class differential harness (h2): PASS (%d cases)\n", cases);
	return 0;
}
