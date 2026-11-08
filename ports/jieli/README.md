# 杰理 AC79 系列（AC7925B）移植说明

crashbox 的存储与掉电算法就来自 AC7925B 特斯拉仪表固件的 `dashboard_diagnostics`，
那边已经有完整的一套：

| 已有 | 位置 |
|---|---|
| 故障报告环 DIAG（`0x7bf000`，256 KiB，64 槽） | `sdk/components/dashboard_diagnostics/` + `services/diagnostics/tesla_diagnostics.c` |
| 事件日志 LOGFS（littlefs，128 KiB，最近 512 条） | `services/diagnostics/tesla_logfs*.c` |
| 任务健康监督、停喂前记录 | `platform/tesla_task_health.c` |
| 读取 | HTTPS 管理接口 `GET /crashes`、`/crashes/{id}`，Flutter App |

## 迁移步骤（未执行，等确认）

1. 把 `include/crashbox.h`、`src/crashbox.c` 放到 `sdk/components/crashbox/`，编译时定义
   `CBX_TEXT_CAPACITY=0`（保持"诊断不收字符串"的规则）、`CBX_RING_CAPACITY=1024`。
2. `tesla_diagnostics.c` 的存储回调直接复用：

   ```c
   static int storage_read(void *u, uint32_t off, void *d, uint32_t n)
   { return sdfile_reserve_zone_read(d, state.zone_address + off, n, 0) == (int)n ? 0 : -1; }
   /* write / erase 同理：sdfile_reserve_zone_write / sdfile_reserve_zone_erase */
   cbx_io_t io = {NULL, DIAGNOSTICS_FLASH_SIZE, storage_read, storage_write, storage_erase};
   ```

   `dd_*` 换成 `cbx_*`：`dd_ring_push` → `cbx_ring_push`，`dd_ring_snapshot` →
   `cbx_snapshot`（原因码 1..7 原样保留，用 `CBX_FLAG_FAULT` 按原规则标故障），
   `dd_store_*` → `cbx_store_*`，`dd_report_json` → `cbx_report_json`
   （字段是原 schemaVersion 1 的超集，App 按"字段可增加"兼容）。
3. 管理接口加 `GET /log.txt`：`cbx_store_text()` 输出可读文本，便于售后直接用浏览器看。
4. 异常入口：在不改公共 `debug_exception.c` 的前提下找到安全挂点后调用
   `cbx_crash_record()`，记录放在复位不清零的 RAM（须实板确认哪段 RAM 跨看门狗复位保留）。

## 要注意

- **格式**：crashbox 的槽格式（魔数 `CBX1`）与原 DIAG（`DIAG` v1）不同，升级后旧报告
  读不到。若要保留，需在 `cbx_store_open` 前加一个只读的旧格式扫描，或升级时先导出。
- 该仓库按 `AGENTS.md`：改诊断前读 DIAGNOSTICS / LOGFS / RESET_STORM /
  TASK_HEALTH_AND_LOGS，用 `scripts/build.ps1` 构建、`scripts/validate.py` 过门禁，
  烧录须用户明确同意。
