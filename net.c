// Raw blocking TCP sockets + the two hash primitives SCRAM-SHA-256 needs,
// exposed to JS as `pljsFdwNet`. See net.h for when this gets wired in.
//
// Deliberately minimal and generic: this is not a Postgres-wire-protocol
// binding, just connect/send/recv/close plus sha256/hmacSha256. The actual
// protocol logic (startup, auth handshake, query/row parsing) lives entirely
// in JS, as a pljs.modules-stored library other FDW modules can require().
// Blocking, matching the rest of this framework's synchronous execute()
// model -- no async I/O primitive exists here, on purpose (see
// ARCHITECTURE.md's "deliberately not implemented" section on async
// execution).
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "net.h"

static JSClassID pljs_fdw_net_socket_class_id;

static void pljs_fdw_net_socket_finalizer(JSRuntime *rt, JSValue val) {
  intptr_t fd = (intptr_t)JS_GetOpaque(val, pljs_fdw_net_socket_class_id);

  if (fd >= 0) {
    close((int)fd);
  }
}

static JSClassDef pljs_fdw_net_socket_class = {
    "PljsFdwSocket",
    .finalizer = pljs_fdw_net_socket_finalizer,
};

/**
 * @brief Unwraps a Uint8Array argument to a raw pointer + length. The
 * returned pointer is only valid as long as @p out_buf (the underlying
 * ArrayBuffer, which the caller must JS_FreeValue()) stays alive.
 */
static uint8_t *pljs_fdw_net_get_u8array(JSContext *ctx, JSValueConst val,
                                         size_t *plen, JSValue *out_buf) {
  size_t offset, length, elem_size;
  JSValue array_buffer =
      JS_GetTypedArrayBuffer(ctx, val, &offset, &length, &elem_size);

  if (JS_IsException(array_buffer)) {
    return NULL;
  }

  size_t buf_len;
  uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, array_buffer);

  if (!buf) {
    JS_FreeValue(ctx, array_buffer);
    return NULL;
  }

  *plen = length;
  *out_buf = array_buffer;

  return buf + offset;
}

/**
 * @brief Copies @p len bytes at @p buf into a new Uint8Array. Goes through
 * the real global `Uint8Array` constructor (rather than JS_NewTypedArray,
 * whose argc/argv here don't behave like `new Uint8Array(buffer)` -- it
 * silently produces a zero-length array) so this matches ordinary JS
 * `new Uint8Array(buffer)` semantics exactly.
 */
static JSValue pljs_fdw_net_new_u8array(JSContext *ctx, const uint8_t *buf,
                                        size_t len) {
  JSValue array_buffer = JS_NewArrayBufferCopy(ctx, buf, len);

  if (JS_IsException(array_buffer)) {
    return array_buffer;
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JS_FreeValue(ctx, global);

  JSValueConst ctor_args[] = {array_buffer};
  JSValue result = JS_CallConstructor(ctx, ctor, 1, ctor_args);

  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, array_buffer);

  return result;
}

static int pljs_fdw_net_get_fd(JSContext *ctx, JSValueConst sock_val) {
  if (!JS_IsObject(sock_val)) {
    JS_ThrowTypeError(ctx, "pljsFdwNet: expected a socket");
    return -1;
  }

  intptr_t fd = (intptr_t)JS_GetOpaque(sock_val, pljs_fdw_net_socket_class_id);

  if (fd < 0) {
    JS_ThrowTypeError(ctx, "pljsFdwNet: socket is closed");
    return -1;
  }

  return (int)fd;
}

/**
 * @brief `pljsFdwNet.connect(host, port)` -- opens a blocking TCP
 * connection, trying every address getaddrinfo() resolves (IPv4 and IPv6
 * both handled) until one connects, and returns a socket object.
 */
static JSValue pljs_fdw_net_connect(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  const char *host = JS_ToCString(ctx, argv[0]);
  int32_t port;

  if (!host || JS_ToInt32(ctx, &port, argv[1])) {
    JS_FreeCString(ctx, host);
    return JS_ThrowTypeError(ctx, "pljsFdwNet.connect(host, port): bad arguments");
  }

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%d", port);

  struct addrinfo hints = {0};
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result;

  int gai_err = getaddrinfo(host, port_str, &hints, &result);

  if (gai_err != 0) {
    JSValue exc = JS_ThrowTypeError(ctx, "pljsFdwNet.connect: %s:%d: %s", host,
                                    port, gai_strerror(gai_err));
    JS_FreeCString(ctx, host);
    return exc;
  }

  int fd = -1;
  int last_errno = 0;

  for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

    if (fd < 0) {
      last_errno = errno;
      continue;
    }

    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }

    last_errno = errno;
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);

  if (fd < 0) {
    JSValue exc = JS_ThrowTypeError(ctx, "pljsFdwNet.connect: %s:%d: %s", host,
                                    port, strerror(last_errno));
    JS_FreeCString(ctx, host);
    return exc;
  }

  JS_FreeCString(ctx, host);

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  JSValue sock_obj = JS_NewObjectClass(ctx, pljs_fdw_net_socket_class_id);

  if (JS_IsException(sock_obj)) {
    close(fd);
    return sock_obj;
  }

  JS_SetOpaque(sock_obj, (void *)(intptr_t)fd);

  return sock_obj;
}

/**
 * @brief `pljsFdwNet.send(sock, data)` -- writes every byte of @p data (a
 * Uint8Array/ArrayBuffer), blocking, retrying on a partial write or EINTR.
 * @returns the number of bytes written (always @p data's full length, or
 * throws).
 */
static JSValue pljs_fdw_net_send(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  int fd = pljs_fdw_net_get_fd(ctx, argv[0]);

  if (fd < 0) {
    return JS_EXCEPTION;
  }

  size_t len;
  JSValue buf_ref;
  uint8_t *buf = pljs_fdw_net_get_u8array(ctx, argv[1], &len, &buf_ref);

  if (!buf) {
    return JS_ThrowTypeError(ctx, "pljsFdwNet.send: expected a Uint8Array");
  }

  size_t total = 0;
  JSValue exc = JS_UNDEFINED;

  while (total < len) {
    ssize_t n = send(fd, buf + total, len - total, 0);

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }

      exc = JS_ThrowInternalError(ctx, "pljsFdwNet.send: %s", strerror(errno));
      break;
    }

    total += (size_t)n;
  }

  JS_FreeValue(ctx, buf_ref);

  return JS_IsException(exc) ? exc : JS_NewInt64(ctx, (int64_t)total);
}

/**
 * @brief `pljsFdwNet.recv(sock, maxLen)` -- a single blocking recv() call
 * for up to @p maxLen bytes (partial reads are normal and expected -- the
 * caller's own message framing loops as needed, same as any raw socket
 * API). Returns a zero-length Uint8Array on EOF.
 */
static JSValue pljs_fdw_net_recv(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  int fd = pljs_fdw_net_get_fd(ctx, argv[0]);

  if (fd < 0) {
    return JS_EXCEPTION;
  }

  int32_t max_len;

  if (JS_ToInt32(ctx, &max_len, argv[1]) || max_len < 0) {
    return JS_ThrowTypeError(ctx, "pljsFdwNet.recv: bad maxLen");
  }

  uint8_t *buf = js_malloc(ctx, (size_t)max_len > 0 ? (size_t)max_len : 1);

  if (!buf) {
    return JS_ThrowOutOfMemory(ctx);
  }

  ssize_t n;

  for (;;) {
    n = recv(fd, buf, (size_t)max_len, 0);

    if (n < 0 && errno == EINTR) {
      continue;
    }

    break;
  }

  if (n < 0) {
    js_free(ctx, buf);
    return JS_ThrowInternalError(ctx, "pljsFdwNet.recv: %s", strerror(errno));
  }

  JSValue result = pljs_fdw_net_new_u8array(ctx, buf, (size_t)n);
  js_free(ctx, buf);

  return result;
}

/**
 * @brief `pljsFdwNet.close(sock)` -- closes the socket; safe to call more
 * than once.
 */
static JSValue pljs_fdw_net_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(ctx, "pljsFdwNet: expected a socket");
  }

  intptr_t fd = (intptr_t)JS_GetOpaque(argv[0], pljs_fdw_net_socket_class_id);

  if (fd >= 0) {
    close((int)fd);
    JS_SetOpaque(argv[0], (void *)(intptr_t)-1);
  }

  return JS_UNDEFINED;
}

/**
 * @brief `pljsFdwNet.sha256(data)` -- returns the 32-byte SHA-256 digest of
 * @p data (a Uint8Array/ArrayBuffer) as a Uint8Array.
 */
static JSValue pljs_fdw_net_sha256(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  size_t len;
  JSValue buf_ref;
  uint8_t *buf = pljs_fdw_net_get_u8array(ctx, argv[0], &len, &buf_ref);

  if (!buf) {
    return JS_ThrowTypeError(ctx, "pljsFdwNet.sha256: expected a Uint8Array");
  }

  uint8_t digest[SHA256_DIGEST_LENGTH];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  SHA256(buf, len, digest);
#pragma GCC diagnostic pop

  JS_FreeValue(ctx, buf_ref);

  return pljs_fdw_net_new_u8array(ctx, digest, SHA256_DIGEST_LENGTH);
}

/**
 * @brief `pljsFdwNet.hmacSha256(key, data)` -- returns the 32-byte
 * HMAC-SHA-256 of @p data under @p key (both Uint8Array/ArrayBuffer) as a
 * Uint8Array.
 */
static JSValue pljs_fdw_net_hmac_sha256(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
  size_t key_len;
  JSValue key_ref;
  uint8_t *key = pljs_fdw_net_get_u8array(ctx, argv[0], &key_len, &key_ref);
  size_t data_len;
  JSValue data_ref;
  uint8_t *data = key ? pljs_fdw_net_get_u8array(ctx, argv[1], &data_len,
                                                 &data_ref)
                      : NULL;

  if (!key || !data) {
    if (key) {
      JS_FreeValue(ctx, key_ref);
    }
    return JS_ThrowTypeError(ctx,
                             "pljsFdwNet.hmacSha256: expected two Uint8Arrays");
  }

  uint8_t digest[SHA256_DIGEST_LENGTH];
  unsigned int digest_len = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  bool ok = HMAC(EVP_sha256(), key, (int)key_len, data, data_len, digest,
                &digest_len) != NULL;
#pragma GCC diagnostic pop

  JS_FreeValue(ctx, key_ref);
  JS_FreeValue(ctx, data_ref);

  if (!ok) {
    return JS_ThrowInternalError(ctx, "pljsFdwNet.hmacSha256: HMAC failed");
  }

  return pljs_fdw_net_new_u8array(ctx, digest, digest_len);
}

void pljs_fdw_net_init_namespace(JSContext *ctx) {
  JSRuntime *rt = JS_GetRuntime(ctx);

  if (pljs_fdw_net_socket_class_id == 0) {
    JS_NewClassID(&pljs_fdw_net_socket_class_id);
    JS_NewClass(rt, pljs_fdw_net_socket_class_id, &pljs_fdw_net_socket_class);
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue net = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, net, "connect",
                    JS_NewCFunction(ctx, pljs_fdw_net_connect, "connect", 2));
  JS_SetPropertyStr(ctx, net, "send",
                    JS_NewCFunction(ctx, pljs_fdw_net_send, "send", 2));
  JS_SetPropertyStr(ctx, net, "recv",
                    JS_NewCFunction(ctx, pljs_fdw_net_recv, "recv", 2));
  JS_SetPropertyStr(ctx, net, "close",
                    JS_NewCFunction(ctx, pljs_fdw_net_close, "close", 1));
  JS_SetPropertyStr(ctx, net, "sha256",
                    JS_NewCFunction(ctx, pljs_fdw_net_sha256, "sha256", 1));
  JS_SetPropertyStr(
      ctx, net, "hmacSha256",
      JS_NewCFunction(ctx, pljs_fdw_net_hmac_sha256, "hmacSha256", 2));

  JS_SetPropertyStr(ctx, global, "pljsFdwNet", net);
  JS_FreeValue(ctx, global);
}
