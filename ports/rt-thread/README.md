# RT-Thread 移植示例（ArtInChip D133 / Luban-Lite）

`crashbox_port_d133.c` 是实际在 D133EBS 盒子上运行的板级适配，可按它改到别的 RT-Thread 板子：

- 存储：`/data/crashbox.bin`（littlefs，256 KiB），`cbx_file_open()`。
- 控制台旁路：注册一个 `cbxcon` 设备接管 `rt_console`，所有打印（去掉颜色码）同时写进日志环，
  再转给原串口。注意 `rt_console_set_device()` 会关掉原设备，切换前要先多打开一次原串口。
- 热复位保留 RAM：链接脚本里把 SRAM 顶部划出堆（Luban-Lite `AIC_SRAM_RETAIN_SIZE`），
  崩溃记录和 4 KiB 日志环放在那里。哪段 SRAM 跨看门狗复位保留要在板子上实测
  （D133：`0x30110000`–`0x30134000` 完整保留，更高处被启动代码零散改写）。
- 复位原因：在 `INIT_PREV` 阶段读硬件复位类型，赶在 `drv_wri_init()` 清除之前。
- 崩溃挂点：CPU 异常回调（`trap_c_callback`，取 mepc/ra/sp/mcause/mtval）和挂死看门狗的
  预警中断（取被打断处的 mepc 和线程名）。
- 读取：`cbx_http_serve(8090, ...)`（lwIP，`CBX_HTTP_LWIP`）、shell `cbx`。
