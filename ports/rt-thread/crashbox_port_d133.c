/*
 * crashbox on the D133EBS box: a fault and work log that survives reboots and
 * is read over the hotspot at http://100.96.0.1:8090/log.txt (or with `cbx`).
 *
 * - Reports go to /data/crashbox.bin (256 KiB, 64 slots of 4 KiB) on littlefs,
 *   which spreads the writes; one at boot, one every CBX_CHECKPOINT_MIN
 *   minutes, one after a crash or watchdog reset.
 * - Everything printed on the console is copied (colours stripped) into a 4 KiB
 *   log ring in SRAM that the boot ROM / SPL leave alone across a warm reset
 *   (AIC_SRAM_RETAIN_SIZE keeps the heap out of it; 0x30110000-0x30134000
 *   measured intact on this board). So the boot report after a crash carries
 *   the last lines printed before it. A power-on reset clears it.
 * - The CPU exception handler (wl_board.c) and the hang watchdog interrupt
 *   (wl_wdt.c) store pc / ra / sp / mcause / mtval / thread in the same SRAM;
 *   the next boot puts that into its report. No flash IO in those handlers.
 * - The reset cause is read before drv_wri_init() clears it.
 *
 * Shell: cbx [status|save|show [n]|clear]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <aic_core.h>
#include <csi_core.h>
#include <dfs_fs.h>
#include "aic_reboot_reason.h"
#include "hal_wri.h"
#include "crashbox_file.h"
#include "crashbox_http.h"
#include "crashbox_service.h"

#ifdef LPKG_USING_CRASHBOX

#define CBX_FILE            "/data/crashbox.bin"
#define CBX_FILE_SIZE       (64u * CBX_SLOT_SIZE)
#define CBX_HTTP_PORT       8090
#define CBX_CHECKPOINT_MIN  30
#define RETAIN_MAGIC        0x4b424243u    /* "CBBK" */
#define RETAIN_VERSION      1u

/* lives at the start of the retained SRAM (linker: __sram_retain_start) */
struct retained {
    uint32_t magic, version;
    cbx_crash_t crash;
    cbx_text_t text;
    uint32_t magic_end;
};

extern char __sram_retain_start[];
#define R ((struct retained *)(void *)__sram_retain_start)

static cbx_service_t g_cbx;
static struct rt_mutex g_lock;
static rt_base_t g_irq_level;
static uint32_t g_reset_flags, g_hw_type, g_sw_reason;
static int g_text_retained, g_started;
static char g_firmware[CBX_NAME_SIZE];

/* ---- console tee: every console write also lands in the retained log ---- */

static struct rt_device g_tee;
static rt_device_t g_console;
static uint8_t g_esc;                       /* inside an ANSI escape sequence */

static void text_add(const char *data, rt_size_t size)
{
    char chunk[96];
    rt_size_t n = 0;

    for (rt_size_t i = 0; i < size; ++i) {
        char ch = data[i];

        if (g_esc) {                        /* ESC [ params letter */
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
                g_esc = 0;
            continue;
        }
        if (ch == 0x1b) {
            g_esc = 1;
            continue;
        }
        if (ch == '\r' || ch == 0)
            continue;
        chunk[n++] = ch;
        if (n == sizeof(chunk) || i + 1 == size) {
            rt_base_t level = rt_hw_interrupt_disable();
            cbx_text_write(&R->text, chunk, n);
            rt_hw_interrupt_enable(level);
            n = 0;
        }
    }
    if (n) {
        rt_base_t level = rt_hw_interrupt_disable();
        cbx_text_write(&R->text, chunk, n);
        rt_hw_interrupt_enable(level);
    }
}

static rt_size_t tee_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    (void)dev;
    text_add(buffer, size);
    return g_console ? rt_device_write(g_console, pos, buffer, size) : size;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops tee_ops = {RT_NULL, RT_NULL, RT_NULL, RT_NULL, tee_write, RT_NULL};
#endif

/* ---- before drv_wri_init() clears the reset flags ---- */

static uint32_t reset_flags(uint32_t hw, uint32_t sw)
{
    uint32_t f = 0;

    switch (sw) {
    case REBOOT_REASON_CMD_REBOOT:
    case REBOOT_REASON_UPGRADE:
    case REBOOT_REASON_BL_UPGRADE:
        return CBX_RESET_SOFTWARE;          /* `reboot`, `aicupg`: the WDT was only the means */
    case REBOOT_REASON_SW_LOCKUP:
    case REBOOT_REASON_HW_LOCKUP:
        f |= CBX_RESET_WATCHDOG;
        break;
    case REBOOT_REASON_PANIC:
        f |= CBX_RESET_EXCEPTION;
        break;
    default:
        break;
    }
    switch (hw) {
    case WRI_TYPE_POR: f |= CBX_RESET_POWER_ON; break;
    case WRI_TYPE_WDT: f |= CBX_RESET_WATCHDOG; break;
    case WRI_TYPE_EXT: f |= CBX_RESET_PIN; break;
    case WRI_TYPE_CMP: f |= CBX_RESET_LOW_VOLTAGE; break;
    default: f |= CBX_RESET_UNKNOWN; break;
    }
    return f;
}

static int cbx_port_early(void)
{
    g_hw_type = aic_wr_type_get();
    g_text_retained = g_hw_type != WRI_TYPE_POR && R->magic == RETAIN_MAGIC && R->magic_end == RETAIN_MAGIC &&
                      R->version == RETAIN_VERSION && R->text.head < CBX_TEXT_RING_SIZE &&
                      R->text.count <= CBX_TEXT_RING_SIZE;
    if (!g_text_retained) {
        memset(R, 0, sizeof(*R));
        R->magic = R->magic_end = RETAIN_MAGIC;
        R->version = RETAIN_VERSION;
    }
    text_add("\n---- boot ----\n", 16);

    rt_memset(&g_tee, 0, sizeof(g_tee));
    g_tee.type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    g_tee.ops = &tee_ops;
#else
    g_tee.write = tee_write;
#endif
    g_console = rt_console_get_device();
    /* rt_console_set_device() closes the old console: hold one more reference so
     * the UART stays open underneath the tee */
    if (g_console && rt_device_open(g_console, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_STREAM) == RT_EOK &&
        rt_device_register(&g_tee, "cbxcon", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STREAM) == RT_EOK)
        rt_console_set_device("cbxcon");
    return 0;
}
INIT_PREV_EXPORT(cbx_port_early);

/* ---- crash hooks: RAM only ---- */

void crashbox_port_crash(uint32_t reason, uint32_t pc, uint32_t lr, uint32_t sp, uint32_t cause, uint32_t addr)
{
    cbx_crash_info_t c;
    rt_thread_t t = rt_thread_self();

    memset(&c, 0, sizeof(c));
    c.reason = reason;
    c.pc = pc;
    c.lr = lr;
    c.sp = sp;
    c.cause = cause;
    c.addr = addr;
    c.uptime_ms = rt_tick_get_millisecond();
    if (t)
        strncpy(c.task, t->name, CBX_TASK_SIZE - 1);
    cbx_crash_record(&R->crash, &c);
}

/* ---- service ---- */

static uint64_t uptime(void)
{
    static uint32_t last;
    static uint64_t high;
    uint32_t now = (uint32_t)rt_tick_get_millisecond();

    if (now < last)
        high += UINT64_C(1) << 32;
    last = now;
    return high | now;
}

static void lock(void *u) { (void)u; rt_mutex_take(&g_lock, RT_WAITING_FOREVER); }
static void unlock(void *u) { (void)u; rt_mutex_release(&g_lock); }
static void text_lock(void *u) { (void)u; g_irq_level = rt_hw_interrupt_disable(); }
static void text_unlock(void *u) { (void)u; rt_hw_interrupt_enable(g_irq_level); }

void crashbox_port_event(uint16_t type, uint16_t code, uint32_t a, uint32_t b, uint32_t c)
{
    if (g_started)
        cbx_service_event(&g_cbx, type, code, a, b, c);
}

void crashbox_port_request(uint32_t reason)
{
    if (g_started)
        cbx_service_request(&g_cbx, reason, 0);
}

static void http_thread(void *arg)
{
    (void)arg;
    rt_thread_mdelay(500);
    if (cbx_http_serve(CBX_HTTP_PORT, cbx_service_http, &g_cbx) != CBX_OK)
        rt_kprintf("cbx: no HTTP on port %d\n", CBX_HTTP_PORT);
}

static void cbx_thread(void *arg)
{
    static cbx_file_t file;
    cbx_service_config_t cfg;
    rt_thread_t t;
    int i;

    (void)arg;
    for (i = 0; i < 50 && !dfs_filesystem_lookup("/data"); i++)   /* data_fs.c may format it first */
        rt_thread_mdelay(100);
    memset(&cfg, 0, sizeof(cfg));
    if (cbx_file_open(&file, CBX_FILE, CBX_FILE_SIZE, &cfg.io) != CBX_OK) {
        rt_kprintf("cbx: cannot open %s\n", CBX_FILE);
        return;
    }
    snprintf(g_firmware, sizeof(g_firmware), "d133 %s %s", __DATE__, __TIME__);
    cfg.firmware = g_firmware;
    cfg.reset_flags = g_reset_flags;
    cfg.checkpoint_minutes = CBX_CHECKPOINT_MIN;
    cfg.crash = &R->crash;
    cfg.text = &R->text;
    cfg.text_retained = 1;                  /* cbx_port_early() already set it up */
    cfg.uptime_ms = uptime;
    cfg.lock = lock;
    cfg.unlock = unlock;
    cfg.text_lock = text_lock;
    cfg.text_unlock = text_unlock;
    if (cbx_service_start(&g_cbx, &cfg) != CBX_OK)
        rt_kprintf("cbx: boot report not stored (%d)\n", g_cbx.last_error);
    g_started = 1;
    rt_kprintf("cbx: boot %lu, reset 0x%lx (hw %lu sw %lu), %u reports; http://<box>:%d/log.txt\n",
               (unsigned long)g_cbx.boot_count, (unsigned long)g_reset_flags, (unsigned long)g_hw_type,
               (unsigned long)g_sw_reason, (unsigned)g_cbx.store.valid_count, CBX_HTTP_PORT);
    t = rt_thread_create("cbxhttp", http_thread, RT_NULL, 6144, RT_THREAD_PRIORITY_MAX - 6, 10);
    if (t)
        rt_thread_startup(t);
    for (;;) {
        (void)cbx_service_poll(&g_cbx);
        rt_thread_mdelay(1000);
    }
}

static int cbx_port_init(void)
{
    rt_thread_t t;

    g_sw_reason = (uint32_t)aic_get_reboot_reason();   /* cached by drv_wri_init() */
    g_reset_flags = reset_flags(g_hw_type, g_sw_reason);
    rt_mutex_init(&g_lock, "cbx", RT_IPC_FLAG_PRIO);
    t = rt_thread_create("cbx", cbx_thread, RT_NULL, 4096, RT_THREAD_PRIORITY_MAX - 5, 10);
    if (t)
        rt_thread_startup(t);
    return 0;
}
INIT_APP_EXPORT(cbx_port_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

static int print_emit(void *user, const char *text, size_t size)
{
    (void)user;
    rt_kprintf("%.*s", (int)size, text);
    return 0;
}

static void cmd_cbx(int argc, char **argv)
{
    const char *type;

    if (!g_started) {
        rt_kprintf("cbx: not started\n");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "save")) {
        rt_kprintf("cbx save: %d\n", cbx_service_save(&g_cbx, CBX_REASON_REQUEST, 0));
    } else if (argc >= 2 && !strcmp(argv[1], "show")) {
        char q[16];
        snprintf(q, sizeof(q), "n=%d", argc >= 3 ? atoi(argv[2]) : 1);
        (void)cbx_service_http(&g_cbx, "/log.txt", q, print_emit, RT_NULL, &type);
    } else if (argc >= 2 && !strcmp(argv[1], "clear")) {
        (void)cbx_service_http(&g_cbx, "/clear", "confirm=yes", print_emit, RT_NULL, &type);
    } else {
        rt_kprintf("cbx: boot %lu, reset 0x%lx, %u of %u slots used, newest #%lu, last error %d, "
                   "log %lu B (%s), http port %d\n",
                   (unsigned long)g_cbx.boot_count, (unsigned long)g_reset_flags,
                   (unsigned)g_cbx.store.valid_count, (unsigned)g_cbx.store.slot_count,
                   (unsigned long)g_cbx.store.newest_sequence, g_cbx.last_error, (unsigned long)R->text.count,
                   g_text_retained ? "kept from the last run" : "new", CBX_HTTP_PORT);
        rt_kprintf("cbx status | save | show [n] | clear\n");
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_cbx, cbx, crashbox fault and work log);
#endif

#endif /* LPKG_USING_CRASHBOX */
