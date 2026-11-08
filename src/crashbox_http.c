/* SPDX-License-Identifier: Apache-2.0 */
#include "crashbox_http.h"
#include <stdio.h>
#include <string.h>

#ifdef CBX_HTTP_LWIP
#include "lwip/sockets.h"
#define CBX_SOCK(f) lwip_##f
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CBX_SOCK(f) f
#endif

typedef struct {
    int fd;
    bool head_sent, failed;
    const char *type;
    uint8_t buf[512];                /* coalesce the many small emits */
    size_t used;
} conn_t;

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        int n = (int)CBX_SOCK(send)(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int flush(conn_t *c)
{
    if (c->used && send_all(c->fd, c->buf, c->used)) c->failed = true;
    c->used = 0;
    return c->failed ? -1 : 0;
}

static int send_head(conn_t *c, int status)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.0 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status == 200 ? "200 OK" : status == 404 ? "404 Not Found" : "400 Bad Request",
                     c->type ? c->type : "text/plain; charset=utf-8");
    c->head_sent = true;
    if (n < 0 || send_all(c->fd, head, (size_t)n)) c->failed = true;
    return c->failed ? -1 : 0;
}

static int emit(void *user, const char *text, size_t size)
{
    conn_t *c = user;
    if (c->failed) return -1;
    if (!c->head_sent && send_head(c, 200)) return -1;
    while (size) {
        size_t room = sizeof(c->buf) - c->used, n = size < room ? size : room;
        memcpy(c->buf + c->used, text, n);
        c->used += n;
        text += n;
        size -= n;
        if (c->used == sizeof(c->buf) && flush(c)) return -1;
    }
    return 0;
}

int cbx_http_handle(int fd, cbx_http_route_fn route, void *user)
{
    static conn_t conn;              /* one client at a time */
    char req[512], *path, *end, *query;
    size_t got = 0;
    int result;

    memset(&conn, 0, sizeof(conn));
    conn.fd = fd;
    while (got < sizeof(req) - 1u) {
        int n = (int)CBX_SOCK(recv)(fd, req + got, sizeof(req) - 1u - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = 0;
    if (strncmp(req, "GET ", 4) != 0) {
        send_head(&conn, 400);
        return CBX_INVALID;
    }
    path = req + 4;
    end = strpbrk(path, " \r\n");
    if (end) *end = 0;
    query = strchr(path, '?');
    if (query) *query++ = 0;
    result = route(user, path, query, emit, &conn, &conn.type);
    if (result == CBX_NOT_FOUND && !conn.head_sent) {
        send_head(&conn, 404);
        emit(&conn, "not found: try /log.txt\n", 24);
    }
    flush(&conn);
    return result;
}

int cbx_http_serve(uint16_t port, cbx_http_route_fn route, void *user)
{
    struct sockaddr_in addr;
    int one = 1;
    int s = CBX_SOCK(socket)(AF_INET, SOCK_STREAM, 0);

    if (s < 0 || !route) return CBX_IO;
    (void)CBX_SOCK(setsockopt)(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (CBX_SOCK(bind)(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || CBX_SOCK(listen)(s, 2) != 0) {
        CBX_SOCK(close)(s);
        return CBX_IO;
    }
    for (;;) {
        struct timeval tv = {5, 0};  /* a silent client cannot hold the server */
        int c = CBX_SOCK(accept)(s, NULL, NULL);
        if (c < 0) continue;
        (void)CBX_SOCK(setsockopt)(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)CBX_SOCK(setsockopt)(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        (void)cbx_http_handle(c, route, user);
        CBX_SOCK(close)(c);
    }
}
