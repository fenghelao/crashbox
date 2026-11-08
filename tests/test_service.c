/* SPDX-License-Identifier: Apache-2.0 */
/* Service policy and the HTTP front end (over a socketpair) on the host. */
#include "crashbox_http.h"
#include "crashbox_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); abort(); } } while (0)

static unsigned checks, locks, text_locks;
static uint8_t flash[CBX_SLOT_SIZE * 16];
static uint64_t clock_ms;
static cbx_service_t svc;
static cbx_crash_t retained_crash;
static cbx_text_t retained_text;

static int m_read(void *u, uint32_t o, void *d, uint32_t n) { (void)u; memcpy(d, flash + o, n); return 0; }
static int m_write(void *u, uint32_t o, const void *d, uint32_t n)
{
    (void)u;
    for (uint32_t i = 0; i < n; ++i) flash[o + i] &= ((const uint8_t *)d)[i];
    return 0;
}
static int m_erase(void *u, uint32_t o, uint32_t n) { (void)u; memset(flash + o, 0xff, n); return 0; }
static uint64_t uptime(void) { return clock_ms; }
static void lk(void *u) { (void)u; ++locks; }
static void ul(void *u) { (void)u; }
static void tlk(void *u) { (void)u; ++text_locks; }

static cbx_service_config_t config(uint32_t reset_flags, bool retained)
{
    cbx_service_config_t c;
    memset(&c, 0, sizeof(c));
    c.io.size = sizeof(flash);
    c.io.read = m_read;
    c.io.write = m_write;
    c.io.erase = m_erase;
    c.firmware = "svc-test";
    c.reset_flags = reset_flags;
    c.checkpoint_minutes = 30;
    c.crash = &retained_crash;
    c.text = &retained_text;
    c.text_retained = retained;
    c.uptime_ms = uptime;
    c.lock = lk;
    c.unlock = ul;
    c.text_lock = tlk;
    c.text_unlock = ul;
    return c;
}

static char body[65536];
static size_t body_len;
static int collect(void *u, const char *t, size_t n)
{
    (void)u;
    if (n > sizeof(body) - body_len - 1) return -1;
    memcpy(body + body_len, t, n);
    body_len += n;
    body[body_len] = 0;
    return 0;
}

static const char *get(const char *path, const char *query)
{
    const char *type = NULL;
    body_len = 0;
    body[0] = 0;
    int r = cbx_service_http(&svc, path, query, collect, NULL, &type);
    return r == CBX_NOT_FOUND ? NULL : body;
}

static void test_boot_crash_and_text(void)
{
    memset(flash, 0xff, sizeof(flash));
    cbx_service_config_t c = config(CBX_RESET_POWER_ON, false);
    CHECK(cbx_service_start(&svc, &c) == CBX_OK && svc.boot_count == 1 && svc.store.valid_count == 1);
    cbx_service_log(&svc, "box: hotspot up on ch %d", 149);
    cbx_service_event(&svc, 3, 1, 2, 3, 4);
    clock_ms = 5000;
    CHECK(cbx_service_poll(&svc) == CBX_OK && svc.store.valid_count == 1);   /* nothing due */

    /* the run "crashes": the handler records into retained RAM, text survives */
    cbx_crash_info_t crash = {0x400a0000, 0x400b0000, 0x30100000, 7, 0x8, CBX_REASON_EXCEPTION, 5000, "box"};
    cbx_crash_record(&retained_crash, &crash);
    cbx_service_log(&svc, "last words before the crash");

    clock_ms = 0;
    c = config(CBX_RESET_WATCHDOG | CBX_RESET_EXCEPTION, true);
    CHECK(cbx_service_start(&svc, &c) == CBX_OK && svc.boot_count == 2 && svc.store.valid_count == 2);
    cbx_report_info_t info;
    CHECK(cbx_store_list(&svc.store, 0, &info) == CBX_OK);
    CHECK((info.flags & (CBX_FLAG_CRASH | CBX_FLAG_FAULT)) == (CBX_FLAG_CRASH | CBX_FLAG_FAULT));
    CHECK(info.crash.pc == 0x400a0000 && !strcmp(info.crash.task, "box"));
    CHECK(cbx_store_read(&svc.store, info.sequence, &svc.report) == CBX_OK);
    CHECK(strstr(svc.report.text, "hotspot up on ch 149") && strstr(svc.report.text, "last words before the crash"));
    CHECK(text_locks > 0 && locks > 0);
    /* the next boot finds no crash record */
    c = config(CBX_RESET_SOFTWARE, true);
    CHECK(cbx_service_start(&svc, &c) == CBX_OK && svc.boot_count == 3);
    CHECK(cbx_store_list(&svc.store, 0, &info) == CBX_OK && !(info.flags & CBX_FLAG_CRASH));
}

static void test_requests_and_checkpoints(void)
{
    memset(flash, 0xff, sizeof(flash));
    clock_ms = 0;
    cbx_service_config_t c = config(CBX_RESET_POWER_ON, false);
    CHECK(cbx_service_start(&svc, &c) == CBX_OK);
    cbx_service_request(&svc, CBX_REASON_WATCHDOG, 0);
    cbx_service_request(&svc, CBX_REASON_REQUEST, 0);       /* routine does not replace a fault */
    CHECK(svc.pending_reason == CBX_REASON_WATCHDOG);
    CHECK(cbx_service_poll(&svc) == CBX_OK && svc.store.valid_count == 2 && !svc.pending_reason);
    cbx_report_info_t info;
    CHECK(cbx_store_list(&svc.store, 0, &info) == CBX_OK && info.reason == CBX_REASON_WATCHDOG);
    clock_ms = 30u * 60000u;
    CHECK(cbx_service_poll(&svc) == CBX_OK && svc.store.valid_count == 3);
    CHECK(cbx_store_list(&svc.store, 0, &info) == CBX_OK && info.reason == CBX_REASON_CHECKPOINT);
    clock_ms += 1000;
    CHECK(cbx_service_poll(&svc) == CBX_OK && svc.store.valid_count == 3);
}

static void test_routes(void)
{
    const char *b;
    CHECK((b = get("/log.txt", NULL)) && strstr(b, "3 reports in 16 slots") && strstr(b, "report #3: checkpoint"));
    CHECK((b = get("/", "n=1")) && strstr(b, "report #3") && !strstr(b, "report #2"));
    CHECK((b = get("/log.json", "n=2")) && b[0] == '[' && strstr(b, "\"id\":3") && strstr(b, "}\n,{"));
    CHECK((b = get("/save", NULL)) && strstr(b, "save: ok (report #4)"));
    CHECK((b = get("/clear", NULL)) && strstr(b, "confirm=yes") && svc.store.valid_count == 4);
    CHECK((b = get("/clear", "confirm=yes")) && strstr(b, "clear: ok") && svc.store.valid_count == 0);
    CHECK(get("/nothing", NULL) == NULL);
}

static void test_http(void)
{
    int sv[2];
    char reply[8192];
    ssize_t n, got = 0;

    CHECK(cbx_service_save(&svc, CBX_REASON_REQUEST, 0) == CBX_OK);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *req = "GET /log.txt?n=5 HTTP/1.1\r\nHost: box\r\n\r\n";
    CHECK(write(sv[1], req, strlen(req)) == (ssize_t)strlen(req));
    CHECK(cbx_http_handle(sv[0], cbx_service_http, &svc) == CBX_OK);
    close(sv[0]);
    while ((n = read(sv[1], reply + got, sizeof(reply) - 1 - (size_t)got)) > 0) got += n;
    reply[got] = 0;
    close(sv[1]);
    CHECK(!strncmp(reply, "HTTP/1.0 200 OK\r\nContent-Type: text/plain", 41));
    CHECK(strstr(reply, "\r\n\r\ncrashbox ") && strstr(reply, "report #5: request"));

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    req = "GET /missing HTTP/1.0\r\n\r\n";
    CHECK(write(sv[1], req, strlen(req)) == (ssize_t)strlen(req));
    CHECK(cbx_http_handle(sv[0], cbx_service_http, &svc) == CBX_NOT_FOUND);
    close(sv[0]);
    got = 0;
    while ((n = read(sv[1], reply + got, sizeof(reply) - 1 - (size_t)got)) > 0) got += n;
    reply[got] = 0;
    close(sv[1]);
    CHECK(!strncmp(reply, "HTTP/1.0 404 Not Found", 22));
}

/* one non-recursive lock for everything, as most ports do: must never be taken twice */
static int held;
static void strict_lock(void *u) { (void)u; CHECK(!held); held = 1; }
static void strict_unlock(void *u) { (void)u; CHECK(held); held = 0; }

static void test_single_lock(void)
{
    memset(flash, 0xff, sizeof(flash));
    clock_ms = 0;
    cbx_service_config_t c = config(CBX_RESET_POWER_ON, false);
    c.lock = strict_lock;
    c.unlock = strict_unlock;
    c.text_lock = NULL;
    c.text_unlock = NULL;
    CHECK(cbx_service_start(&svc, &c) == CBX_OK);
    cbx_service_log(&svc, "single lock");
    cbx_service_event(&svc, 1, 0, 0, 0, 0);
    cbx_service_request(&svc, CBX_REASON_REQUEST, 0);
    CHECK(cbx_service_poll(&svc) == CBX_OK && cbx_service_save(&svc, CBX_REASON_REQUEST, 0) == CBX_OK);
    CHECK(get("/log.txt", NULL) && strstr(body, "single lock") && !held);
}

int main(void)
{
    test_single_lock();
    test_boot_crash_and_text();
    test_requests_and_checkpoints();
    test_routes();
    test_http();
    printf("crashbox service: %u checks passed (service %u B)\n", checks, (unsigned)sizeof(cbx_service_t));
    return 0;
}
