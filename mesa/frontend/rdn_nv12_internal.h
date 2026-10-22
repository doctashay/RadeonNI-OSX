/* Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_NV12_INTERNAL_H
#define RDN_NV12_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include "GL/rdn_nv12.h"
struct pipe_context;
struct pipe_resource;
struct pipe_screen;
struct rdn_nv12;

bool rdn_nv12_source_valid(const struct OSMesaNV12Source *source);
struct rdn_nv12 *rdn_nv12_create(struct pipe_screen *screen);
bool rdn_nv12_available(const struct rdn_nv12 *pack);
int rdn_nv12_run(struct rdn_nv12 *pack, struct pipe_resource *source[4],
                 struct pipe_resource *destination[2], uint64_t timeout_ns);
void rdn_nv12_destroy(struct rdn_nv12 *pack);
bool rdn_nv12_has_quarantine(void);
#endif
