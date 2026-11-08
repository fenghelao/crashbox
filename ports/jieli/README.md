# 杰理 AC79 系列（AC7925B）移植

crashbox 的存储与掉电算法来自 AC7925B 特斯拉仪表固件的 `dashboard_diagnostics`；
2026-11-08 起该固件反过来改用 crashbox（分支 `feature/crashbox-diagnostics`）：

| 项 | 做法 |
|---|---|
| 存储 | 原 DIAG 保留区（`0x7bf000`，256 KiB，64 槽）；`sdfile_reserve_zone_read/write/erase` 直接填 `cbx_io_t` |
| 配置 | `dashboard_diagnostics.h` 先定义 `CBX_SLOT_LIMIT 64`、`CBX_RING_CAPACITY 1024`、`CBX_EVENT_CAPACITY 64`、`CBX_TEXT_CAPACITY 0`（诊断不收字符串），再包含 `crashbox.h` |
| 编译 | `vendor/crashbox.c` 由 `dashboard_diagnostics.c` `#include`，同一编译单元，Makefile 与主机测试脚本都不用改（放进 `vendor/` 是为了躲开组件目录的 `*.c` 通配） |
| 接口 | 原 `dd_*` 类型直接 typedef 为 `cbx_*`，字段同名；产品的原因码 1..7 原样存储，故障按原规则（原因 ≥3，或低压/看门狗/异常复位后的开机）在写入时判定 |
| 读取 | 原 HTTPS 管理接口 `GET /crashes`、`/crashes/{id}`（JSON），新增 `/crashes/{id}/text`、`/log.txt` |

```c
static int storage_read(void *u, uint32_t off, void *d, uint32_t n)
{ return sdfile_reserve_zone_read(d, zone_address + off, n, 0) == (int)n ? 0 : -1; }
/* write / erase 同理：sdfile_reserve_zone_write / sdfile_reserve_zone_erase */
cbx_io_t io = {NULL, DIAGNOSTICS_FLASH_SIZE, storage_read, storage_write, storage_erase};
```

## 注意

- 槽格式（魔数 `CBX1`）与原 `DIAG` v1 不同：升级前写入的旧报告不再读出，随新报告循环覆盖。
- 异常入口尚未接 `cbx_crash_record()`：需先在不改公共 `debug_exception.c` 的前提下找到安全挂点，
  并实测哪段 RAM 跨看门狗复位保留。
