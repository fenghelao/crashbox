# crashbox

嵌入式产品的"黑匣子"：把最近的事件和日志放在 RAM 里，开机、定时、出故障时生成一份报告，
存进 Flash 上的循环区（裸分区或一个固定大小的文件），重启后仍在，通过产品自己的网络
（Wi-Fi 热点、以太网）用浏览器读取，例如 `http://<设备>:8090/log.txt`。

A small flight recorder for embedded products: recent events and log text kept in RAM, a report
on boot, on a timer and on faults, stored in a ring of 4 KiB slots on raw flash or in one
fixed-size file, read back as plain text or JSON over the product's own network.
Portable C99, no heap, no locks, no OS calls in the core. Apache-2.0.

## 特点

| | |
|---|---|
| 磨损均衡 | 报告按槽循环写入，擦写均匀分布在整个区域；256 KiB 区域每 30 分钟一份报告，每个扇区约每天擦一次。文件后端交给 littlefs / UBIFS 等文件系统再做一层均衡 |
| 掉电安全 | 正文、CRC、独立提交字依次写入并回读；断在任何一个字节都只丢正在写的那份，最新已提交的报告不会被覆盖（主机测试逐字节验证） |
| 故障单独保留 | 四分之一的槽（至少两个）只存故障报告，日常检查点不会把崩溃挤掉 |
| 崩溃现场 | `cbx_crash_record()` 只写 RAM（无锁、无堆、不碰 Flash），可在异常/看门狗中断里调用；放在热复位不清零的 RAM 里，下次开机写进开机报告 |
| 日志文本 | 每份报告带最近 2 KiB 文本（相当于 txt 的结尾）；可编译成纯数值模式（`CBX_TEXT_CAPACITY=0`），不存任何字符串 |
| 读取 | 可读文本（`/log.txt`）和 JSON（`/log.json`），流式输出，不需要大缓冲 |

## 组成

```text
include/crashbox.h          核心：事件环、文本环、崩溃记录、报告、存储、文本/JSON 输出
include/crashbox_service.h  可选：常用策略（开机报告、检查点、延后落盘、HTTP 路由）
include/crashbox_file.h     可选：固定大小文件做存储（POSIX）
include/crashbox_http.h     可选：极小的阻塞式 HTTP 服务（BSD socket，定义 CBX_HTTP_LWIP 用 lwIP）
ports/linux/crashboxd.c     Linux 守护进程：跟踪日志文件和内核日志，故障自动出报告
ports/rt-thread/            RT-Thread（Luban-Lite D133）移植示例
ports/jieli/                杰理 AC79 系列移植说明
tests/                      主机测试（NOR 写规则、逐字节掉电、损坏、回绕、服务、HTTP）
```

## 快速移植

1. 提供存储：裸 Flash 分区填 `cbx_io_t` 的 read / write / erase（4 KiB 擦除单位）；
   有文件系统就用 `cbx_file_open()`。区域大小为 16 KiB ~ 1 MiB，4 KiB 对齐。
2. 填 `cbx_service_config_t`：存储、固件名、本次复位原因、时钟、锁；有热复位保留 RAM 就给
   `crash` 和 `text` 指过去。
3. 开机调用 `cbx_service_start()`，后台低优先级任务每秒调 `cbx_service_poll()`。
4. 异常处理和看门狗中断里调 `cbx_crash_record()`；业务里用 `cbx_service_log()` /
   `cbx_service_event()`，出故障用 `cbx_service_request()`（不做 IO，可在任意任务调用）。
5. 起一个任务跑 `cbx_http_serve(8090, cbx_service_http, &svc)`，或把 `cbx_service_http()`
   挂到现有 Web 服务上。

```c
static cbx_service_t svc;
cbx_service_config_t cfg = {0};
cbx_file_open(&file, "/data/crashbox.bin", 256 * 1024, &cfg.io);
cfg.firmware = "box 1.2.0";
cfg.reset_flags = my_reset_cause();          /* CBX_RESET_* */
cfg.checkpoint_minutes = 30;
cfg.crash = &retained->crash;                /* 热复位保留 RAM，没有就 NULL */
cfg.text = &retained->text;
cfg.uptime_ms = my_uptime_ms;
cfg.lock = my_lock; cfg.unlock = my_unlock;
cbx_service_start(&svc, &cfg);
```

输出示例：

```text
==== report #12: boot (1) FAULT | reset watchdog | boot 7 | up 0h00m04.717s | fw box 1.2.0
crash: watchdog in task 'aic_rx' at 0h41m02.120s: pc 0x400a1234 lr 0x00000000 sp 0x00000000 cause 0x0 addr 0x00000000
log:
[2461.880] box: iPhone joined the hotspot
[2462.101] watchdog: not fed for 5 s, running "aic_rx" - resetting
```

## 已用于

| 平台 | 存储 | 崩溃现场 | 读取 |
|---|---|---|---|
| ArtInChip D133（RT-Thread / Luban-Lite） | littlefs 上 256 KiB 文件 | 异常入口与挂死看门狗中断写 SRAM 保留区，连同最近 4 KiB 串口输出跨热复位保留 | `:8090/log.txt`、shell `cbx` |
| 全志 T113-S3（Tina Linux） | UBIFS 上 256 KiB 文件 | 内核日志错误、进程崩溃/OOM、异常结束（无干净关机标记） | `:8090/log.txt` |
| 杰理 AC7925B | 原 256 KiB DIAG 保留区（本组件的算法来源） | 见 `ports/jieli/README.md` | 管理接口 |

## 测试

```bash
cmake -S . -B build -G Ninja && ninja -C build && (cd build && ctest)
```

## 许可

Apache-2.0，见 `LICENSE`、`NOTICE`。存储与掉电算法来自 tesla_ble_ac7925b 固件的
dashboard diagnostics 日志（同为 Apache-2.0）。
