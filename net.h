#pragma once

#include "deps/quickjs/quickjs.h"

// Adds a `pljsFdwNet` global object to `ctx`: raw blocking TCP sockets
// (connect/send/recv/close) plus the two hash primitives SCRAM-SHA-256
// authentication needs (sha256/hmacSha256). See net.c for the full API.
// Called once per JSContext from pljs_fdw_extend_namespace() in fdw.c.
void pljs_fdw_net_init_namespace(JSContext *ctx);
