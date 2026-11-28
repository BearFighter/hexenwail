/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * c_ref_class.c -- compile the frozen C original with PlayerClass_Count
 * renamed, so one harness binary can call both the C and the Rust version.
 */

#include "quakedef.h"

#define PlayerClass_Count c_PlayerClass_Count
#include "sv_main_class_h2.c"
#undef PlayerClass_Count
