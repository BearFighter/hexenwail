/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * init_harness.c -- engine/rust/src/sv_main_init.rs against the C original of
 * Hexen II's SV_Init, SV_Protocol_f, Max_Edicts_f, SV_Edicts and Sv_Edicts_f
 * (F / slice F of #469), in one binary.
 *
 * run_init_harness.sh compiles c_ref_init.c with the fixture renamed to c_*,
 * compiles engine/rust/sv_main_target.c with -DSV_FIXTURE so its file shim
 * reaches this harness's recorder, and links both against the one Rust
 * archive.  Every callee is a recorder here: it appends its name, arguments
 * and the state it can observe to a call log, then does what the engine does
 * to the state the callee reads afterwards.  CON_Printf and the file writer
 * record the formatted text, so the C's printf and Rust's variadic calls into
 * the same shim produce the same bytes.
 *
 * Each case is forked once per side from an identical snapshot.  A child
 * serialises the call log, console log, file log, every engine field the port
 * can touch and its outcome counters; the parent compares the two byte for
 * byte.  Sys_Error records and exits 42 in both children, so fatal cases
 * compare too.  No case depends on the address of a Rust function: callbacks
 * and commands are normalised to names by pointer identity.
 */

#include "quakedef.h"
#include "filenames.h"
#include "sv_main_target.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- the ported functions, one set from C and one from Rust ---- */
void c_SV_Init (void);
void c_SV_Edicts (const char *Name);
void c_ref_ProtocolF (void);
void c_ref_EdictsF (void);
void c_ref_MaxEdicts (cvar_t *var);
cvarcallback_t c_ref_max_edicts_fn (void);
xcommand_t c_ref_protocol_fn (void);
xcommand_t c_ref_edicts_fn (void);
void SV_Init (void);
void SV_Edicts (const char *Name);
void SV_Protocol_f (void);
void Sv_Edicts_f (void);
void Max_Edicts_f (cvar_t *var);
void SV_DebugMoveStep_Changed (cvar_t *var);
size_t SvMainInitFacts_sizeof (void);
size_t SvMainInitFacts_offsetof_hw_version_num (void);

/* ---- the engine's storage ---- */
server_t	sv;
server_static_t	svs;
int		sv_protocol = PROTOCOL_VERSION;
qboolean	sv_protocol_userset;
int		sv_kingofhill;
char		localmodels[MAX_MODELS][8];
int		pr_edict_size;
dfunction_t	*pr_functions;
quakeparms_t	parms;
quakeparms_t	*host_parms = &parms;

#define MAXED	12
static edict_t		edicts[MAXED];
static dfunction_t	functions[8];
static char		strings[8][64];

/* progs.h made EDICT_NUM a function; the C original's SV_Edicts calls it. */
edict_t *EDICT_NUM (int n)
{
	return (n >= 0 && n < MAXED) ? &edicts[n] : &edicts[0];
}

/* The C fixture and SvMainInit_Cvar name these variables directly, so the
 * harness owns them one by one; cvar_ptrs only serialises them in enum order. */
#define INIT_CVAR(n) cvar_t n = { #n, "0", CVAR_NONE }
INIT_CVAR(sv_maxvelocity);
INIT_CVAR(sv_gravity);
INIT_CVAR(sv_friction);
INIT_CVAR(sv_edgefriction);
INIT_CVAR(sv_stopspeed);
INIT_CVAR(sv_maxspeed);
INIT_CVAR(sv_accelerate);
INIT_CVAR(sv_altnoclip);
INIT_CVAR(sv_idealpitchscale);
INIT_CVAR(sv_idealrollscale);
INIT_CVAR(sv_aim);
INIT_CVAR(sv_nostep);
INIT_CVAR(sv_walkpitch);
INIT_CVAR(sv_gameplayfix_elevators);
INIT_CVAR(sv_freezenonclients);
INIT_CVAR(max_edicts);
INIT_CVAR(sv_maxdatagram);
INIT_CVAR(sv_cheats);
INIT_CVAR(sv_gameplayfix_random);
INIT_CVAR(pr_checkextension);
cvar_t hexenwail_version = {"hexenwail", "0", CVAR_ROM};
INIT_CVAR(sv_flypitch);
INIT_CVAR(sv_debugmovestep);
INIT_CVAR(sv_sound_distance);
INIT_CVAR(sv_update_player);
INIT_CVAR(sv_update_monsters);
INIT_CVAR(sv_update_missiles);
INIT_CVAR(sv_update_misc);
INIT_CVAR(sv_netsort);
INIT_CVAR(sv_ce_scale);
INIT_CVAR(sv_ce_max_size);
#undef INIT_CVAR

static cvar_t *const cvar_ptrs[SV_MAIN_INIT_CVAR_COUNT] = {
	&sv_maxvelocity, &sv_gravity, &sv_friction, &sv_edgefriction,
	&sv_stopspeed, &sv_maxspeed, &sv_accelerate, &sv_altnoclip,
	&sv_idealpitchscale, &sv_idealrollscale, &sv_aim, &sv_nostep,
	&sv_walkpitch, &sv_gameplayfix_elevators, &sv_freezenonclients,
	&max_edicts, &sv_maxdatagram, &sv_cheats,
	&sv_gameplayfix_random, &pr_checkextension, &hexenwail_version,
	&sv_flypitch, &sv_debugmovestep, &sv_sound_distance,
	&sv_update_player, &sv_update_monsters, &sv_update_missiles,
	&sv_update_misc, &sv_netsort, &sv_ce_scale, &sv_ce_max_size
};

/* ---- logs ---- */
#define LOGSIZE	(1 << 20)
static char	calllog[LOGSIZE], conlog[LOGSIZE], filelog[LOGSIZE];
static size_t	calllog_len, conlog_len, filelog_len;

static void append (char *log, size_t *len, const char *s)
{
	size_t n = strlen (s);
	if (*len + n >= LOGSIZE)
		n = LOGSIZE - 1 - *len;
	memcpy (log + *len, s, n);
	*len += n;
	log[*len] = 0;
}

static void log_call (const char *fmt, ...) FUNC_PRINTF(1,2);
static void log_call (const char *fmt, ...)
{
	va_list	ap;
	char	tmp[8192];
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	append (calllog, &calllog_len, tmp);
}

/* ---- case parameters, computed identically in parent and children ---- */
enum operation { OP_INIT, OP_PROTOCOL, OP_EDICTS_F, OP_MAX_EDICTS, OP_EDICTS };

typedef struct params_s
{
	int		op;
	int		active;
	int		argc;
	const char	*argv[4];
	int		initial_protocol;
	int		parm_index;
	int		parm_value_index;
	int		launch_argc;
	char		*launch_argv[8];
	int		file_fail;
	int		num_edicts;
	double		time;
	int		name_index;
} params_t;

static const char *const names[] = {
	"edicts.txt", "custom.txt", "fail.txt", "sub/dir.txt"
};
static const char *const protocol_args[] = {
	"18", "19", "20", "21", "100", "junk", "-1"
};
static const int protocol_values[] = {
	PROTOCOL_RAVEN_111, PROTOCOL_RAVEN_112, PROTOCOL_UQE_113,
	PROTOCOL_UH2_114, PROTOCOL_HEXENWAIL_1
};

static uint64_t mix64 (uint64_t x)
{
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

static int protocol_valid (int p)
{
	int i;
	for (i = 0; i < (int)(sizeof protocol_values / sizeof protocol_values[0]); i++)
		if (p == protocol_values[i])
			return 1;
	return 0;
}

static void compute_params (int case_no, params_t *p)
{
	uint64_t r = mix64 ((uint64_t)(case_no + 1) * 0x9e3779b97f4a7c15ULL);
	int a = (int)(r & 0x3fffffff);

	memset (p, 0, sizeof *p);
	p->op = case_no % 5;
	p->active = (a & 1) != 0;
	p->time = (double)(a % 997) / 37.0 + (double)(r >> 32) / 1000000.0;
	p->num_edicts = 1 + ((a >> 5) % MAXED);

	switch (p->op)
	{
	case OP_INIT:
		p->initial_protocol = (a & 0x40)
			? 9999 : protocol_values[(a >> 3) % 5];
		/* Both arms of `i && i < com_argc - 1': no parm, and a parm at 1
		 * or 3 with enough arguments following it. */
		switch ((a >> 7) & 3)
		{
		case 0:
			p->parm_index = 0;
			p->launch_argc = 3;
			p->launch_argv[0] = "hexenwail";
			p->launch_argv[1] = "-foo";
			p->launch_argv[2] = "bar";
			break;
		case 1:
			p->parm_index = 1;
			p->parm_value_index = 2;
			p->launch_argc = 4;
			p->launch_argv[0] = "hexenwail";
			p->launch_argv[1] = "-protocol";
			p->launch_argv[2] = (char *)protocol_args[(a >> 9) % 7];
			p->launch_argv[3] = "tail";
			break;
		default:
			p->parm_index = 3;
			p->parm_value_index = 4;
			p->launch_argc = 6;
			p->launch_argv[0] = "hexenwail";
			p->launch_argv[1] = "-foo";
			p->launch_argv[2] = "bar";
			p->launch_argv[3] = "-protocol";
			p->launch_argv[4] = (char *)protocol_args[(a >> 9) % 7];
			p->launch_argv[5] = "tail";
			break;
		}
		break;

	case OP_PROTOCOL:
		/* a % 4 would make argc==2 imply an even `a`, hence active==0,
		 * and SV_Protocol_f's active-server warning would never run. */
		p->argc = (a >> 1) % 4;
		p->argv[1] = protocol_args[(a >> 9) % 7];
		p->argv[2] = "extra";
		p->initial_protocol = protocol_values[(a >> 3) % 5];
		p->launch_argc = 1;
		p->launch_argv[0] = "hexenwail";
		break;

	case OP_EDICTS_F:
		p->active = (a & 1) != 0;
		p->argc = (a & 2) ? 2 : 1;
		p->name_index = (a >> 4) % 4;
		p->argv[1] = names[p->name_index];
		p->file_fail = (p->name_index == 2) || ((a >> 6) & 1);
		break;

	case OP_MAX_EDICTS:
		p->active = (a & 1) != 0;
		break;

	case OP_EDICTS:
		p->name_index = (a >> 4) % 4;
		p->file_fail = (p->name_index == 2) || ((a >> 6) & 1);
		break;
	}
}

static const char *case_name (const params_t *p)
{
	return p->op == OP_EDICTS_F && p->argc < 2 ? "edicts.txt" : names[p->name_index];
}

/* ---- what the recorder stubs need to remember ---- */
static int cmd_argc;
static const char *cmd_argv[4];
static int case_file_fail;
static int fatal_count, file_fail_count;
static char file_tokens[4];
static int nfiles;
static int out_fd = -1;

static void setup_case (const params_t *p)
{
	int i;

	calllog_len = conlog_len = filelog_len = 0;
	calllog[0] = conlog[0] = filelog[0] = 0;
	memset (&sv, 0, sizeof sv);
	memset (&svs, 0, sizeof svs);
	memset (localmodels, 0x41, sizeof localmodels);
	memset (edicts, 0, sizeof edicts);
	memset (functions, 0, sizeof functions);
	memset (strings, 0, sizeof strings);

	sv.active = p->active;
	sv.time = p->time;
	sv.num_edicts = p->num_edicts;
	sv.edicts = edicts;
	pr_edict_size = sizeof (edict_t);
	pr_functions = functions;
	sv_protocol = p->initial_protocol;
	sv_protocol_userset = false;
	sv_kingofhill = 7;

	for (i = 0; i < SV_MAIN_INIT_CVAR_COUNT; i++) {
		cvar_ptrs[i]->value = 0;
		cvar_ptrs[i]->integer = 0;
		cvar_ptrs[i]->callback = NULL;
	}
	for (i = 0; i < 8; i++) {
		snprintf (strings[i], sizeof strings[i], "string-%d", i);
		functions[i].s_name = i;
	}
	for (i = 0; i < MAXED; i++) {
		edicts[i].v.nextthink = (float)i + 0.25f;
		edicts[i].v.classname = i % 8;
		edicts[i].v.model = (i + 3) % 8;
		edicts[i].v.think = i % 8;
		edicts[i].v.touch = (i + 2) % 8;
		edicts[i].v.use = (i + 5) % 8;
	}

	parms.argc = p->launch_argc;
	parms.argv = (char **)p->launch_argv;
	cmd_argc = p->argc;
	for (i = 0; i < 4; i++)
		cmd_argv[i] = p->argv[i];
	case_file_fail = p->file_fail;
	fatal_count = file_fail_count = nfiles = 0;
}

/* ---- serialisation ---- */
static unsigned char *outbuf;
static size_t outlen, outcap;

static void out_bytes (const void *p, size_t n)
{
	if (outlen + n > outcap) {
		outcap = (outlen + n) * 2 + 4096;
		outbuf = realloc (outbuf, outcap);
		if (!outbuf) _exit (3);
	}
	memcpy (outbuf + outlen, p, n);
	outlen += n;
}

static void out_tag (const char *t)
{
	out_bytes ("\n#", 2);
	out_bytes (t, strlen (t));
	out_bytes ("\n", 1);
}

static void out_int (int v)		{ out_bytes (&v, sizeof v); }
static void out_double (double v)	{ out_bytes (&v, sizeof v); }
static void out_float (float v)		{ out_bytes (&v, sizeof v); }

static void out_str (const char *s)
{
	if (!s) { out_int (-1); return; }
	out_int ((int)strlen (s));
	out_bytes (s, strlen (s));
}

static void serialize_state (void)
{
	int i;
	outlen = 0;

	out_tag ("calllog");
	out_bytes (calllog, calllog_len);
	out_tag ("conlog");
	out_bytes (conlog, conlog_len);
	out_tag ("filelog");
	out_bytes (filelog, filelog_len);

	out_tag ("globals");
	out_int (sv.active);
	out_double (sv.time);
	out_int (sv.num_edicts);
	out_int (sv_protocol);
	out_int (sv_protocol_userset);
	out_int (sv_kingofhill);
	out_int (pr_edict_size);
	out_int (fatal_count);
	out_int (file_fail_count);

	out_tag ("localmodels");
	out_bytes (localmodels, sizeof localmodels);
	out_tag ("edicts");
	out_bytes (edicts, sizeof edicts);
	out_tag ("functions");
	out_bytes (functions, sizeof functions);
	out_tag ("strings");
	out_bytes (strings, sizeof strings);

	out_tag ("cvars");
	for (i = 0; i < SV_MAIN_INIT_CVAR_COUNT; i++) {
		out_str (cvar_ptrs[i]->name);
		out_str (cvar_ptrs[i]->string);
		out_int ((int)cvar_ptrs[i]->flags);
		out_float (cvar_ptrs[i]->value);
		out_int (cvar_ptrs[i]->integer);
		out_int (cvar_ptrs[i]->callback != NULL);
	}
}

static void flush_and_exit (int code)
{
	size_t done = 0;
	while (done < outlen) {
		ssize_t w = write (out_fd, outbuf + done, outlen - done);
		if (w <= 0) {
			if (w < 0 && errno == EINTR)
				continue;
			_exit (4);
		}
		done += (size_t)w;
	}
	close (out_fd);
	exit (code);
}

/* ---- recorder stubs ---- */
static const char *callback_name (cvarcallback_t f)
{
	if (f == Host_Callback_Notify) return "host-notify";
	if (f == SV_DebugMoveStep_Changed) return "debugmove";
	if (f == (cvarcallback_t)Max_Edicts_f ||
	    f == c_ref_max_edicts_fn () ||
	    f == (cvarcallback_t)c_ref_MaxEdicts)
		return "max-edicts";
	return "unknown";
}

static const char *command_name (xcommand_t f)
{
	if (f == (xcommand_t)SV_Protocol_f ||
	    f == c_ref_protocol_fn () ||
	    f == (xcommand_t)c_ref_ProtocolF)
		return "sv_protocol";
	if (f == (xcommand_t)Sv_Edicts_f ||
	    f == c_ref_edicts_fn () ||
	    f == (xcommand_t)c_ref_EdictsF)
		return "sv_edicts";
	return "unknown";
}

void Cvar_RegisterVariable (cvar_t *var)
{
	log_call ("Cvar_RegisterVariable(%s)\n", var->name);
}

void Cvar_SetCallback (cvar_t *var, cvarcallback_t func)
{
	log_call ("Cvar_SetCallback(%s,%s)\n", var->name, callback_name (func));
}

void Cvar_SetValueROM (const char *name, float value)
{
	log_call ("Cvar_SetValueROM(%s,%a)\n", name, (double)value);
}

void Cmd_AddCommand (const char *name, xcommand_t func)
{
	log_call ("Cmd_AddCommand(%s,%s)\n", name, command_name (func));
}

int Cmd_Argc (void)
{
	log_call ("Cmd_Argc()=%d\n", cmd_argc);
	return cmd_argc;
}

const char *Cmd_Argv (int i)
{
	const char *s = (i >= 0 && i < 4 && cmd_argv[i]) ? cmd_argv[i] : "";
	log_call ("Cmd_Argv(%d)=%s\n", i, s);
	return s;
}

int COM_CheckParm (const char *parm)
{
	int i;
	log_call ("COM_CheckParm(%s)\n", parm);
	for (i = 1; i < parms.argc; i++)
		if (!strcmp (parms.argv[i], parm)) {
			log_call ("  -> %d\n", i);
			return i;
		}
	return 0;
}

void CON_Printf (unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	char tmp[8192];
	snprintf (tmp, sizeof tmp, "[%u] ", flags);
	append (conlog, &conlog_len, tmp);
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	append (conlog, &conlog_len, tmp);
	log_call ("CON_Printf(%u): %s\n", flags, tmp);
}

void Sys_Error (const char *fmt, ...)
{
	va_list ap;
	char tmp[4096];
	va_start (ap, fmt);
	vsnprintf (tmp, sizeof tmp, fmt, ap);
	va_end (ap);
	log_call ("Sys_Error: %s\n", tmp);
	fatal_count++;
	serialize_state ();
	flush_and_exit (42);
	_exit (42); /* not reached: tells the compiler Sys_Error never returns */
}

const char *SV_ProtocolName (int protocol)
{
	log_call ("SV_ProtocolName(%d)\n", protocol);
	if (protocol == PROTOCOL_RAVEN_111) return "Raven/H2/1.11";
	if (protocol == PROTOCOL_RAVEN_112) return "Raven/MP/1.12";
	if (protocol == PROTOCOL_UQE_113) return "UQE/1.13";
	if (protocol == PROTOCOL_UH2_114) return "Raven/MP/1.14";
	if (protocol == PROTOCOL_HEXENWAIL_1) return "Hexenwail/1";
	return NULL;
}

const char *PR_GetString (int num)
{
	/* Do not log here: C may evaluate fprintf's PR_GetString arguments in
	 * reverse order, while Rust spells them left to right.  The formatted
	 * line already proves which strings the port selected. */
	if (num >= 0 && num < 8)
		return strings[num];
	return "";
}

void Host_Callback_Notify (cvar_t *var)
{
	log_call ("Host_Callback_Notify(%s)\n", var->name);
}

void SV_DebugMoveStep_Changed (cvar_t *var)
{
	log_call ("SV_DebugMoveStep_Changed(%s)\n", var->name);
}

void SV_UserInit (void)
{
	log_call ("SV_UserInit()\n");
}

char *FS_MakePath (int base, int *error, const char *path)
{
	static char buf[512];
	log_call ("FS_MakePath(%d,%s)\n", base, path);
	if (error)
		*error = 0;
	snprintf (buf, sizeof buf, "/userdir/%s", path);
	return buf;
}

void *c_ref_fopen (const char *path, const char *mode)
{
	log_call ("fopen(%s,%s)=", path, mode);
	if (case_file_fail) {
		file_fail_count++;
		log_call ("NULL\n");
		return NULL;
	}
	if (nfiles >= 4)
		nfiles = 0;
	log_call ("file%d\n", nfiles);
	return &file_tokens[nfiles++];
}

int c_ref_vfprintf (void *file, const char *fmt, va_list ap)
{
	char tmp[16384];
	int idx = -1, i, r;
	for (i = 0; i < 4; i++)
		if (file == &file_tokens[i])
			idx = i;
	r = vsnprintf (tmp, sizeof tmp, fmt, ap);
	log_call ("fprintf(file%d): %s\n", idx, tmp);
	{
		char line[16384 + 32]; /* "file%d: " plus NUL around tmp[] */
		snprintf (line, sizeof line, "file%d: %s\n", idx, tmp);
		append (filelog, &filelog_len, line);
	}
	return r;
}

int c_ref_fprintf (void *file, const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start (ap, fmt);
	r = c_ref_vfprintf (file, fmt, ap);
	va_end (ap);
	return r;
}

void c_ref_fclose (void *file)
{
	int idx = -1, i;
	for (i = 0; i < 4; i++)
		if (file == &file_tokens[i])
			idx = i;
	log_call ("fclose(file%d)\n", idx);
}

/* ---- run one side ---- */
static void run_side (const params_t *p, int rust)
{
	const char *name = case_name (p);
	switch (p->op)
	{
	case OP_INIT:
		if (rust) SV_Init (); else c_SV_Init ();
		break;
	case OP_PROTOCOL:
		if (rust) SV_Protocol_f (); else c_ref_ProtocolF ();
		break;
	case OP_EDICTS_F:
		if (rust) Sv_Edicts_f (); else c_ref_EdictsF ();
		break;
	case OP_MAX_EDICTS:
		if (rust) Max_Edicts_f (cvar_ptrs[SV_MAIN_INIT_CVAR_MAX_EDICTS]);
		else c_ref_MaxEdicts (cvar_ptrs[SV_MAIN_INIT_CVAR_MAX_EDICTS]);
		break;
	case OP_EDICTS:
		if (rust) SV_Edicts (name); else c_SV_Edicts (name);
		break;
	}
	serialize_state ();
	flush_and_exit (0);
}

static int read_file (const char *path, unsigned char **buf, size_t *len)
{
	FILE *f = fopen (path, "rb");
	size_t cap = 4096, n = 0;
	unsigned char *b;
	if (!f) return 0;
	b = malloc (cap);
	if (!b) { fclose (f); return 0; }
	for (;;) {
		size_t got;
		if (n == cap) {
			unsigned char *nb;
			cap *= 2;
			nb = realloc (b, cap);
			if (!nb) { free (b); fclose (f); return 0; }
			b = nb;
		}
		got = fread (b + n, 1, cap - n, f);
		n += got;
		if (got == 0) break;
	}
	fclose (f);
	*buf = b;
	*len = n;
	return 1;
}

static int wait_status (pid_t pid)
{
	int status;
	if (waitpid (pid, &status, 0) != pid)
		return 999;
	if (WIFEXITED (status))
		return WEXITSTATUS (status);
	if (WIFSIGNALED (status))
		return 128 + WTERMSIG (status);
	return 998;
}

int main (void)
{
	const char *outdir = getenv ("OUT");
	const char *cases_env = getenv ("CASES");
	int cases = cases_env ? atoi (cases_env) : 6000;
	int case_no, status_c, status_r;
	int n_ops[5] = {0, 0, 0, 0, 0};
	int fatal = 0, file_fail = 0, active_yes = 0, active_no = 0;
	char path_c[512], path_r[512];

	if (!outdir) outdir = ".";
	if (SvMainInitFacts_sizeof () != sizeof (sv_main_init_facts_t) ||
	    SvMainInitFacts_offsetof_hw_version_num () !=
	        offsetof (sv_main_init_facts_t, hw_version_num)) {
		fprintf (stderr, "FAIL: sv_main_init facts layout mismatch\n");
		return 1;
	}

	for (case_no = 0; case_no < cases; case_no++) {
		params_t p;
		unsigned char *bc = NULL, *br = NULL;
		size_t lc = 0, lr = 0;
		pid_t pid;

		compute_params (case_no, &p);
		n_ops[p.op]++;
		if (p.active) active_yes++; else active_no++;
		if (p.file_fail) file_fail++;
		if (p.op == OP_INIT) {
			int chosen = p.initial_protocol;
			if (p.parm_index)
				chosen = atoi (p.launch_argv[p.parm_value_index]);
			if (!protocol_valid (chosen))
				fatal++;
		}

		snprintf (path_c, sizeof path_c, "%s/init-%06d-c.bin", outdir, case_no);
		snprintf (path_r, sizeof path_r, "%s/init-%06d-r.bin", outdir, case_no);

		pid = fork ();
		if (pid == 0) {
			setup_case (&p);
			out_fd = open (path_c, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (out_fd < 0) _exit (3);
			run_side (&p, 0);
		}
		status_c = wait_status (pid);

		pid = fork ();
		if (pid == 0) {
			setup_case (&p);
			out_fd = open (path_r, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (out_fd < 0) _exit (3);
			run_side (&p, 1);
		}
		status_r = wait_status (pid);

		if (status_c != status_r || !read_file (path_c, &bc, &lc) ||
		    !read_file (path_r, &br, &lr) || lc != lr ||
		    memcmp (bc, br, lc) != 0) {
			size_t at = 0, lim;
			fprintf (stderr,
				"MISMATCH case %d op %d: status %d/%d, bytes %zu/%zu\n",
				case_no, p.op, status_c, status_r, lc, lr);
			lim = lc < lr ? lc : lr;
			while (at < lim && bc[at] == br[at]) at++;
			fprintf (stderr, "first difference at %zu of %zu\n", at, lim);
			fprintf (stderr, "--- C ---\n");
			fwrite (bc, 1, lc < 4096 ? lc : 4096, stderr);
			fprintf (stderr, "\n--- Rust ---\n");
			fwrite (br, 1, lr < 4096 ? lr : 4096, stderr);
			fprintf (stderr, "\n");
			free (bc);
			free (br);
			return 1;
		}
		free (bc);
		free (br);
		unlink (path_c);
		unlink (path_r);
	}

	printf ("layouts: facts sizeof=%zu, hw_version_num offset=%zu\n",
		SvMainInitFacts_sizeof (), SvMainInitFacts_offsetof_hw_version_num ());
	printf ("outcomes: init %d protocol_f %d edicts_f %d max_edicts %d edicts %d "
		"fatal %d file_fail %d active %d/%d\n",
		n_ops[OP_INIT], n_ops[OP_PROTOCOL], n_ops[OP_EDICTS_F],
		n_ops[OP_MAX_EDICTS], n_ops[OP_EDICTS], fatal, file_fail,
		active_yes, active_no);
	printf ("arms (taken/not): fatal %d/%d file_fail %d/%d active %d/%d\n",
		fatal, cases - fatal, file_fail, cases - file_fail,
		active_yes, active_no);
	printf ("sv_main_init differential harness (h2): PASS (%d cases)\n", cases);
	return 0;
}
