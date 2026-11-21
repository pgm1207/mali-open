/* SPDX-License-Identifier: MIT */
/* 4x MSAA is OPT-IN again (MALI_EXPERIMENTAL_MSAA=1): the sample counts, the resolve and the draw path exist, but loadOp LOAD and
 * storeOp STORE together with a resolve are refused (vk/src/mali_cmd.c:995-1006), so a title that picks 4x from the advertised
 * limits gets a black frame: zink took 4x whenever it was offered (HL2 on CMF: "MSAA colour storeOp STORE together with a
 * resolve", 2x "MSAA colour loadOp LOAD"). Advertising a sample count whose LOAD/resolve combination we refuse is a compatibility
 * trap, so the default is 1x; the feature stays behind the env until MSAA LOAD + STORE/resolve land (driver-msaa.md). The limits,
 * VkImageFormatProperties and pipeline creation all read this one switch. */
#ifndef MALI_MSAA_H
#define MALI_MSAA_H

#include <stdbool.h>
#include <stdlib.h>

static inline bool
mali_experimental_msaa(void)
{
   const char *e = getenv("MALI_EXPERIMENTAL_MSAA");
   return !(e && e[0] == '0');   /* default on: =0 keeps the 1x-only limits and pipeline rules */
}

#endif
