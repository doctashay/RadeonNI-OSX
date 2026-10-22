/*
 * Where the bundle finds things inside Apple's OpenGL engine, per Mac OS X
 * version. The offsets are those of the engine's context (CGLContextObj
 * ->rend) and belong to one build of GLEngine; the bundle must not touch
 * the engine on a system it does not know, so an unknown kernel gives NULL.
 *
 * Found with tools/guest/glctx_scan.c (docs/GLD-INTERFACE.md):
 *
 *   10.4.11 (Darwin 8): pointer to the application's table at rend+0x4680,
 *     driver table at rend+0x4698 (33 words), 684 entries in the table;
 *     gldCreateContext's sixth argument points rend+0x360.
 *   10.5.8 (Darwin 9.8): the same offsets, but 757 dispatch entries;
 *     CoreGraphics stores CGLContextObj->priv at context+0xbd8.
 *   10.5.9 (Darwin 9.9): pointer to the application's table at rend+0x5c78,
 *     driver table at rend+0x5cc4 (42 words), 759 entries in the table
 *     (the 10.5 SDK's header declares 759); gldCreateContext's sixth
 *     argument points rend+0x780 (0x56f7a0 - 0x56f020 on the G5).
 *
 * The application's table is followed in memory by CGLContextObj's `priv'
 * and `stak': a count that is too large corrupts them.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_ENGINE_H
#define RDN_ENGINE_H

#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

struct rdn_engine_layout {
	const char *os;
	unsigned table_off;	/* driver table, from the engine context */
	unsigned disp_back;	/* bytes before the driver table: pointer to the app's table */
	unsigned disp_entries;	/* entries of the application's table */
	unsigned table_entries;	/* words of the driver table */
	int present;		/* index of "present" in it, -1: not known */
	unsigned create_arg;	/* gldCreateContext's sixth argument: bytes into the engine context */
};

/* The largest application table of any layout. */
#define RDN_MAX_DISP_ENTRIES 759

static inline const struct rdn_engine_layout *rdn_engine_layout(void)
{
	static const struct rdn_engine_layout tiger = {
		"10.4", 0x4698, 0x18, 684, 33, 24, 0x360
	};
#ifdef __LP64__
	/* Measured on 9.8: app table at 0x6570; gliCreateContext passes rend+0x7b0. */
	static const struct rdn_engine_layout leopard_1058 = {
		"10.5.8", 0x65e0, 0x70, 757, 42, -1, 0x7b0
	};
#else
	static const struct rdn_engine_layout leopard_1058 = {
		"10.5.8", 0x5cc4, 0x4c, 757, 42, -1, 0x780
	};
#endif
	static const struct rdn_engine_layout leopard_1059 = {
		"10.5", 0x5cc4, 0x4c, 759, 42, -1, 0x780
	};
	static int major = -1;
	static int leopard_minor;

	if (major < 0) {
		struct utsname u;

		major = 0;
		if (uname(&u) == 0) {
			major = atoi(u.release);
			if (!strncmp(u.release, "9.8.", 4))
				leopard_minor = 8;
			else if (!strncmp(u.release, "9.9.", 4))
				leopard_minor = 9;
		}
	}
	if (major == 8)
#ifdef __LP64__
		return NULL;
#else
		return &tiger;
#endif
	if (major == 9 && leopard_minor == 8)
		return &leopard_1058;
	if (major == 9 && leopard_minor == 9)
#ifdef __LP64__
		return NULL;
#else
		return &leopard_1059;
#endif
	return NULL;
}

#endif /* RDN_ENGINE_H */
