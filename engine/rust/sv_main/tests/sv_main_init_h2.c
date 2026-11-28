/*
 * sv_main_init_h2.c -- the C original of sv_main.c's init and console verbs,
 * moved verbatim out of engine/hexen2/sv_main.c: Max_Edicts_f, SV_Protocol_f,
 * SV_Init, SV_Edicts and Sv_Edicts_f, with their comments.  The differential
 * harness beside this file includes it; no engine target compiles it.  The
 * engine's implementation is engine/rust/src/sv_main_init.rs (F / slice F of
 * #469).
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

/* sv_main.c's file-scope storage and declarations from above the moved text.
 * The harness owns the storage; these are only the declarations the functions
 * need to compile. */
extern	server_t		sv;
extern	int			sv_protocol;
extern	qboolean		sv_protocol_userset;
extern	int			sv_kingofhill;
extern	char			localmodels[MAX_MODELS][8];
extern	int			pr_edict_size;
extern	dfunction_t		*pr_functions;

extern	cvar_t	sv_maxvelocity;
extern	cvar_t	sv_gameplayfix_elevators;
extern	cvar_t	sv_freezenonclients;
extern	cvar_t	sv_gameplayfix_random;
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
extern	cvar_t	sv_sound_distance;
extern	cvar_t	sv_update_player;
extern	cvar_t	sv_update_monsters;
extern	cvar_t	sv_update_missiles;
extern	cvar_t	sv_update_misc;
extern	cvar_t	sv_netsort;
extern	cvar_t	max_edicts;
extern	cvar_t	sv_maxdatagram;
extern	cvar_t	sv_cheats;
extern	cvar_t	pr_checkextension;
extern	cvar_t	hexenwail_version;
extern	cvar_t	sv_ce_scale;
extern	cvar_t	sv_ce_max_size;
extern void SV_DebugMoveStep_Changed (cvar_t *var);
extern void SV_UserInit (void);

/*============================================================================*/


static void Sv_Edicts_f(void);

static void Max_Edicts_f (cvar_t *var)
{
	(void) var;
	if (sv.active)
		Con_Printf ("max_edicts will not take effect until the next map load.\n");
}

/* SV_ProtocolName -- ported to Rust: engine/rust/src/sv_main_connect.rs
 * (#469 slice D); the C original is
 * engine/rust/sv_main/tests/sv_main_connect_h2.c.  Still called from C
 * (SV_Protocol_f, the -protocol startup check), so it stays a C-visible
 * symbol implemented by the Rust module. */
const char *SV_ProtocolName (int protocol);

/*
===============
SV_Protocol_f

Ironwail's sv_protocol verb.  -protocol has always been able to pick the wire
format, but only at launch, which on a dedicated server means restarting the
server to record a demo an older client can play back.

Not a cvar, and deliberately not archived, for the same reason it is not one
upstream: the value is written into the signon at SV_SendServerinfo and read
back out of it by every client for the life of the map, so it is a property of
the running server rather than a preference.  A change lands at the next map
load -- announced, because pretending otherwise is worse than useless here:
SV_MaxSounds() is derived from sv_protocol and is consulted at PRECACHE time,
so a mid-map change would put the sound indices already handed out past what
the protocol can express.  sv_main.c's svc_sound path carries the matching
guard and says so.
===============
*/
static void SV_Protocol_f (void)
{
	int	i;

	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf ("\"sv_protocol\" is \"%i\" (%s)\n",
			    sv_protocol, SV_ProtocolName (sv_protocol));
		break;

	case 2:
		i = atoi (Cmd_Argv(1));
		if (!SV_ProtocolName (i))
		{
			Con_Printf ("sv_protocol must be %i, %i, %i, %i or %i\n",
				    PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112,
				    PROTOCOL_UQE_113, PROTOCOL_UH2_114,
				    PROTOCOL_HEXENWAIL_1);
			break;
		}
		sv_protocol = i;
		sv_protocol_userset = true;
		Con_Printf ("sv_protocol set to %i (%s)\n", i, SV_ProtocolName (i));
		if (sv.active)
			Con_Printf ("changes will not take effect until the next map load.\n");
		break;

	default:
		Con_SafePrintf ("usage: sv_protocol <protocol>\n");
		break;
	}
}

/*
===============
SV_Init
===============
*/
void SV_Init (void)
{
	int		i;
	const char	*p;

	Cvar_RegisterVariable (&sv_maxvelocity);
	Cvar_RegisterVariable (&sv_gravity);
	Cvar_RegisterVariable (&sv_friction);
	Cvar_SetCallback (&sv_gravity, Host_Callback_Notify);
	Cvar_SetCallback (&sv_friction, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_edgefriction);
	Cvar_RegisterVariable (&sv_stopspeed);
	Cvar_RegisterVariable (&sv_maxspeed);
	Cvar_SetCallback (&sv_maxspeed, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_accelerate);
	Cvar_RegisterVariable (&sv_altnoclip);
	Cvar_RegisterVariable (&sv_idealpitchscale);
	Cvar_RegisterVariable (&sv_idealrollscale);
	Cvar_RegisterVariable (&sv_aim);
	Cvar_RegisterVariable (&sv_nostep);
	Cvar_RegisterVariable (&sv_walkpitch);
	Cvar_RegisterVariable (&sv_gameplayfix_elevators);
	Cvar_RegisterVariable (&sv_freezenonclients);
	Cvar_RegisterVariable (&max_edicts);
	Cvar_RegisterVariable (&sv_maxdatagram);
	Cvar_SetCallback (&max_edicts, Max_Edicts_f);
	Cvar_RegisterVariable (&sv_cheats);
	Cvar_RegisterVariable (&sv_gameplayfix_random);
	Cvar_RegisterVariable (&pr_checkextension);
	Cvar_RegisterVariable (&hexenwail_version);
	/* Stamp the real value now that it is registered.  Cvar_SetValueROM is
	 * the only way in -- CVAR_ROM rejects Cvar_Set from console or config. */
	Cvar_SetValueROM ("hexenwail", (float)HW_VERSION_NUM);
	Cvar_RegisterVariable (&sv_flypitch);
	Cvar_RegisterVariable (&sv_debugmovestep);
	Cvar_SetCallback (&sv_debugmovestep, SV_DebugMoveStep_Changed);
	Cvar_RegisterVariable (&sv_sound_distance);
	Cvar_RegisterVariable (&sv_update_player);
	Cvar_RegisterVariable (&sv_update_monsters);
	Cvar_RegisterVariable (&sv_update_missiles);
	Cvar_RegisterVariable (&sv_update_misc);
	Cvar_RegisterVariable (&sv_netsort);
	Cvar_RegisterVariable (&sv_ce_scale);
	Cvar_RegisterVariable (&sv_ce_max_size);

	SV_UserInit ();

	Cmd_AddCommand ("sv_edicts", Sv_Edicts_f);	
	Cmd_AddCommand ("sv_protocol", SV_Protocol_f);

	for (i = 0; i < MAX_MODELS; i++)
		sprintf (localmodels[i], "*%i", i);

	// initialize King of Hill to world
	sv_kingofhill = 0;

	i = COM_CheckParm ("-protocol");
	if (i && i < com_argc - 1)
	{
		sv_protocol = atoi (com_argv[i + 1]);
		sv_protocol_userset = true;
	}
	p = SV_ProtocolName (sv_protocol);
	if (!p)
	{
		/* Still fatal at startup, unlike the console verb: a bad
		 * -protocol means the operator asked for a wire format we cannot
		 * speak, and coming up on a different one would silently hand
		 * every connecting client the wrong answer. */
		Sys_Error ("Bad protocol version request %i. Accepted values: %i, %i, %i, %i, %i.",
				sv_protocol, PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112, PROTOCOL_UQE_113,
				PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1);
		return; /* silence compiler */
	}
	Sys_Printf ("Server using protocol %i (%s)\n", sv_protocol, p);
}

void SV_Edicts (const char *Name)
{
	FILE	*FH;
	int		i;
	edict_t	*e;

	FH = fopen(FS_MakePath(FS_USERDIR,NULL,Name), "w");
	if (!FH)
	{
		Con_Printf("Could not open %s\n", Name);
		return;
	}

	fprintf(FH, "Number of Edicts: %d\n", sv.num_edicts);
	fprintf(FH, "Server Time: %f\n", sv.time);
	fprintf(FH, "\n");
	fprintf(FH, "Num.     Time Class Name                     Model                          Think                                    Touch                                    Use\n");
	fprintf(FH, "---- -------- ------------------------------ ------------------------------ ---------------------------------------- ---------------------------------------- ----------------------------------------\n");

	for (i = 1; i < sv.num_edicts; i++)
	{
		e = EDICT_NUM(i);
		fprintf(FH, "%3d. %8.2f %-30s %-30s %-40s %-40s %-40s\n",
			i, e->v.nextthink, PR_GetString(e->v.classname), PR_GetString(e->v.model),
			PR_GetString(pr_functions[e->v.think].s_name), PR_GetString(pr_functions[e->v.touch].s_name),
			PR_GetString(pr_functions[e->v.use].s_name));
	}
	fclose(FH);
}

static void Sv_Edicts_f (void)
{
	const char	*Name;

	if (!sv.active)
	{
		Con_Printf("This command can only be executed on a server running a map\n");
		return;
	}

	if (Cmd_Argc() < 2)
	{
		Name = "edicts.txt";
	}
	else
	{
		Name = Cmd_Argv(1);
	}

	SV_Edicts(Name);
}
