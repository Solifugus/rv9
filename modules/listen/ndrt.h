/*
 * The module's side of Whistle's engine: what listen.c sets up before it
 * calls the engine. See nd_mod.h for the context itself.
 */
#pragma once

#include "rv9/module.h"

void nd_pool_init(nd_pool_t *p, void *base, uint32_t size);
