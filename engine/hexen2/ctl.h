/* ctl.h -- loopback UDP message channel for an external world controller
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

#ifndef __CTL_H__
#define __CTL_H__

void CTL_Init (void);	/* registers ctl_port, ctl_version and ctl_status */

/* server progs builtins #200-#204, see the pr_builtin table in pr_cmds.c */
void PF_ctl_send (void);
void PF_ctl_recv (void);
void PF_ctl_tokenize (void);
void PF_ctl_argv (void);
void PF_ctl_stof (void);

#endif	/* __CTL_H__ */
