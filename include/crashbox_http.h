/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A minimal blocking HTTP/1.0 server for reading crashbox reports over the
 * product's network (Wi-Fi hotspot, Ethernet, ...): one client at a time,
 * GET only, the response streamed until the connection closes. Run it in its
 * own task, apart from the product's web server, so the log stays readable
 * when the application is stuck.
 *
 * BSD sockets by default; build with CBX_HTTP_LWIP for lwIP's lwip_* API.
 */
#ifndef CRASHBOX_HTTP_H
#define CRASHBOX_HTTP_H

#include "crashbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Route: set *type, then emit the body; return CBX_NOT_FOUND for a 404.
 * cbx_service_http() is one. */
typedef int (*cbx_http_route_fn)(void *user, const char *path, const char *query, cbx_emit_fn emit,
                                 void *emit_user, const char **type);

/* Serves forever on `port` (all addresses). Returns only if the socket cannot
 * be set up (CBX_IO). */
int cbx_http_serve(uint16_t port, cbx_http_route_fn route, void *user);

/* One request on an accepted, connected socket (for servers with their own
 * accept loop); closes nothing. */
int cbx_http_handle(int fd, cbx_http_route_fn route, void *user);

#ifdef __cplusplus
}
#endif

#endif
