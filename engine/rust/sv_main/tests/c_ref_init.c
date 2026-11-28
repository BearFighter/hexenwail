/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_init.c -- the C original of sv_main.c's init and console verbs as one
 * translation unit, for the differential harness (F / slice F of #469).
 * run_init_harness.sh compiles it with -DSV_FIXTURE naming sv_main_init_h2.c
 * and renames every ported function to c_*, so it and the Rust link into one
 * binary.  Its callees are not renamed: both sides call the harness's
 * recorders, including the file-output shims in sv_main_target.c.
 *
 * The three static functions stay static; non-static doors at the bottom are
 * what the harness calls to reach them.  Defining `static' away would also
 * change the fixture's file-scope declarations, which are not part of this
 * slice, so it is deliberately not done.
 */

#include "quakedef.h"
#include "sv_main_target.h"

/* The runner passes -Dfopen/-Dfprintf/-Dfclose on the command line, but
 * glibc's fortify headers can redefine fprintf again while quakedef.h is
 * included.  Restore the shim names after the headers, before the fixture. */
#undef fopen
#undef fprintf
#undef fclose
extern void *c_ref_fopen (const char *path, const char *mode);
extern int c_ref_fprintf (void *file, const char *fmt, ...) FUNC_PRINTF(2,3);
extern void c_ref_fclose (void *file);
#define fopen			c_ref_fopen
#define fprintf			c_ref_fprintf
#define fclose			c_ref_fclose

#define SV_Init			c_SV_Init
#define SV_Edicts		c_SV_Edicts
#define SV_Protocol_f		c_SV_Protocol_f
#define Sv_Edicts_f		c_Sv_Edicts_f
#define Max_Edicts_f		c_Max_Edicts_f

#include SV_FIXTURE

void c_SV_Init (void);
void c_SV_Edicts (const char *Name);
void c_ref_ProtocolF (void);
void c_ref_EdictsF (void);
void c_ref_MaxEdicts (cvar_t *var);

void c_ref_ProtocolF (void)
{
	c_SV_Protocol_f ();
}

void c_ref_EdictsF (void)
{
	c_Sv_Edicts_f ();
}

void c_ref_MaxEdicts (cvar_t *var)
{
	c_Max_Edicts_f (var);
}

/* The C originals of these three are file-static.  Hand their addresses to
 * the harness so it can normalise registered callbacks/commands to names
 * without exporting the statics or changing the fixture's declarations. */
cvarcallback_t c_ref_max_edicts_fn (void)
{
	return c_Max_Edicts_f;
}

xcommand_t c_ref_protocol_fn (void)
{
	return c_SV_Protocol_f;
}

xcommand_t c_ref_edicts_fn (void)
{
	return c_Sv_Edicts_f;
}
