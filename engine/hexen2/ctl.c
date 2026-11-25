/* ctl.c -- loopback UDP message channel for an external world controller
 * crawler-fork private patch; design in engine-patches/ctl/SPEC.md
 *
 * Copyright (C) 2026  crawler-fork contributors
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

#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "ctl.h"

#define	CTL_MAX_MSG	1400	/* bytes per message, either direction */
#define	CTL_MAX_TOKENS	64
#define	CTL_TOKEN_LEN	256
#define	CTL_RECV_TRIES	64	/* datagrams examined per ctl_recv call, at most */

static cvar_t	ctl_port = {"ctl_port", "0", CVAR_NONE};
static cvar_t	ctl_version = {"ctl_version", "1", CVAR_ROM};

static sys_socket_t		ctl_socket = INVALID_SOCKET;
static int			ctl_bound_port;		/* port ctl_socket is bound to; 0 if closed */
static int			ctl_failed_port;	/* last port that failed to open; not retried until ctl_port changes */
static struct sockaddr_in	ctl_peer;
static qboolean			ctl_have_peer;
static unsigned long		ctl_sent, ctl_received, ctl_dropped;

static char	ctl_sendbuf[CTL_MAX_MSG + 1];
static char	ctl_recvbuf[CTL_MAX_MSG + 1];
static char	ctl_tokens[CTL_MAX_TOKENS][CTL_TOKEN_LEN];
static int	ctl_numtokens;
static char	ctl_empty[1];		/* "" handed to QC for out-of-range argv */

#if defined(PLATFORM_WINDOWS)
static qboolean	ctl_wsa_started;
static WSADATA	ctl_wsadata;
#endif

/* CTL_Close */
static void CTL_Close (void)
{
	if (ctl_socket != INVALID_SOCKET)
	{
		closesocket (ctl_socket);
		ctl_socket = INVALID_SOCKET;
	}
	ctl_bound_port = 0;
	ctl_have_peer = false;
#if defined(PLATFORM_WINDOWS)
	if (ctl_wsa_started)
	{
		WSACleanup ();
		ctl_wsa_started = false;
	}
#endif
}

/* CTL_Open */
static qboolean CTL_Open (int port)
{
	sys_socket_t s = INVALID_SOCKET;
	struct sockaddr_in address;
#if defined(PLATFORM_WINDOWS)
	u_long one = 1;
#else
	int one = 1;
#endif

#if defined(PLATFORM_WINDOWS)
	if (!ctl_wsa_started)
	{
		if (WSAStartup (MAKEWORD(1,1), &ctl_wsadata) != 0)
			return false;
		ctl_wsa_started = true;
	}
#endif

	s = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		goto fail;
	if (ioctlsocket (s, FIONBIO, IOCTLARG_P(&one)) == SOCKET_ERROR)
		goto fail;

	memset (&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)port);

	if (bind (s, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR)
		goto fail;

	ctl_socket = s;
	ctl_bound_port = port;
	ctl_have_peer = false;
	return true;

fail:
	if (s != INVALID_SOCKET)
		closesocket (s);
#if defined(PLATFORM_WINDOWS)
	if (ctl_wsa_started)
	{
		WSACleanup ();
		ctl_wsa_started = false;
	}
#endif
	return false;
}

/* CTL_Check */
static qboolean CTL_Check (void)
{
	int port = (int)ctl_port.value;

	if (port < 0 || port > 65535)
		port = 0;

	if (port == 0)
	{
		if (ctl_socket != INVALID_SOCKET)
			CTL_Close ();
		ctl_failed_port = 0;
		return false;
	}
	if (ctl_socket != INVALID_SOCKET && port == ctl_bound_port)
		return true;
	if (port == ctl_failed_port)
		return false;

	CTL_Close ();
	if (CTL_Open (port))
	{
		ctl_failed_port = 0;
		return true;
	}
	ctl_failed_port = port;
	Con_DPrintf ("ctl: cannot open UDP 127.0.0.1:%d\n", port);
	return false;
}

/* PF_ctl_send */
void PF_ctl_send (void)
{
	int i;
	size_t len;

	G_FLOAT(OFS_RETURN) = 0;
	if (!CTL_Check () || !ctl_have_peer)
		return;

	ctl_sendbuf[0] = 0;
	for (i = 0; i < pr_argc; i++)
	{
		if (q_strlcat (ctl_sendbuf, G_STRING(OFS_PARM0 + i*3), sizeof(ctl_sendbuf)) >= sizeof(ctl_sendbuf))
		{
			ctl_dropped++;
			return;
		}
	}

	len = strlen (ctl_sendbuf);
	if (len == 0)
		return;

	if (sendto (ctl_socket, ctl_sendbuf, (int)len, 0, (struct sockaddr *)&ctl_peer, sizeof(ctl_peer)) == SOCKET_ERROR)
		return;

	ctl_sent++;
	G_FLOAT(OFS_RETURN) = 1;
}

/* PF_ctl_recv */
void PF_ctl_recv (void)
{
	int tries;
	int n;
	struct sockaddr_in from;
	socklen_t fromlen;

	ctl_recvbuf[0] = 0;
	if (CTL_Check ())
	{
		for (tries = 0; tries < CTL_RECV_TRIES; tries++)
		{
			fromlen = sizeof(from);
			n = recvfrom (ctl_socket, ctl_recvbuf, CTL_MAX_MSG + 1, 0, (struct sockaddr *)&from, &fromlen);
			if (n == SOCKET_ERROR)
			{
#if defined(PLATFORM_WINDOWS)
				int err = SOCKETERRNO;

				if (err == WSAEMSGSIZE)
				{
					ctl_dropped++;
					ctl_recvbuf[0] = 0;
					continue;
				}
				if (err == WSAECONNRESET)
				{
					ctl_recvbuf[0] = 0;
					continue;
				}
#endif
				ctl_recvbuf[0] = 0;
				break;
			}

			if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
			{
				ctl_dropped++;
				ctl_recvbuf[0] = 0;
				continue;
			}
			if (n > CTL_MAX_MSG)
			{
				ctl_dropped++;
				ctl_recvbuf[0] = 0;
				continue;
			}
			if (n == 0)
			{
				ctl_recvbuf[0] = 0;
				continue;
			}

			ctl_recvbuf[n] = 0;
			ctl_peer = from;
			ctl_have_peer = true;
			ctl_received++;
			break;
		}
	}

	G_INT(OFS_RETURN) = PR_SetEngineString (ctl_recvbuf);
}

/* PF_ctl_tokenize */
void PF_ctl_tokenize (void)
{
	const char *data = G_STRING(OFS_PARM0);

	ctl_numtokens = 0;
	while (ctl_numtokens < CTL_MAX_TOKENS)
	{
		data = COM_Parse (data);
		if (!data)
			break;
		q_strlcpy (ctl_tokens[ctl_numtokens], com_token, CTL_TOKEN_LEN);
		ctl_numtokens++;
	}

	G_FLOAT(OFS_RETURN) = ctl_numtokens;
}

/* PF_ctl_argv */
void PF_ctl_argv (void)
{
	int n = (int)G_FLOAT(OFS_PARM0);

	if (n < 0 || n >= ctl_numtokens)
		G_INT(OFS_RETURN) = PR_SetEngineString (ctl_empty);
	else
		G_INT(OFS_RETURN) = PR_SetEngineString (ctl_tokens[n]);
}

/* PF_ctl_stof */
void PF_ctl_stof (void)
{
	G_FLOAT(OFS_RETURN) = (float) atof (G_STRING(OFS_PARM0));
}

/* CTL_Status_f */
static void CTL_Status_f (void)
{
	if (CTL_Check ())
		Con_Printf ("ctl: listening on 127.0.0.1:%d\n", ctl_bound_port);
	else
		Con_Printf ("ctl: off (ctl_port \"%s\")\n", ctl_port.string);

	if (ctl_have_peer)
		Con_Printf ("ctl: peer 127.0.0.1:%d\n", (int)ntohs(ctl_peer.sin_port));
	else
		Con_Printf ("ctl: no peer yet\n");

	Con_Printf ("ctl: sent %lu, received %lu, dropped %lu\n", ctl_sent, ctl_received, ctl_dropped);
}

/* CTL_Init */
void CTL_Init (void)
{
	Cvar_RegisterVariable (&ctl_port);
	Cvar_RegisterVariable (&ctl_version);
	Cmd_AddCommand ("ctl_status", CTL_Status_f);
}
