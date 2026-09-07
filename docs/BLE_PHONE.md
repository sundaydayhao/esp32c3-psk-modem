# 手机 BLE 接收协议

固件使用 NimBLE，按 Nordic UART Service UUID 提供接收文字。默认 Wi-Fi AP；
长按 SW1 两秒并松开或使用网页按钮可切换 BLE。切换写 NVS 并重启，选择持久保存，
文字历史和解调参数不保存。输出方式互斥，避免同时初始化两套无线服务的动态开销。
即使关闭本地 LCD，SW1 切换任务仍然工作。

| 特征 | UUID | 操作 | 内容 |
|---|---|---|---|
| 服务 | 6E400001-B5A3-F393-E0A9-E50E24DCCA9E | 发现 | UART |
| RX | 6E400002-B5A3-F393-E0A9-E50E24DCCA9E | Write / Write without response | 手机指令 |
| TX | 6E400003-B5A3-F393-E0A9-E50E24DCCA9E | Notify | 原始 Varicode 解出的文字字节 |
| 状态扩展 | 6E400004-B5A3-F393-E0A9-E50E24DCCA9E | Read / Long read | ASCII 键值状态 |

状态特征不是 Nordic 标准的一部分。读取完整状态需要手机支持 GATT 长读取。
`mode/tone/ready/signal/quality/rev` 为接收器状态；`dsp_us/max_us/dma_ovf` 为处理统计；
`cursor/history_lost/notify/notify_err/heap/cmd` 为 BLE 发送位置、累计跳过的历史字节、
通知入队次数、通知错误计数、当前空闲堆和最后指令结果。`notify_err` 是事件计数，
不是丢失字节数或调制误码率。

指令严格区分大小写，使用一个空格分隔字段，每行最多 63 个 ASCII 字符，以 LF
结束，忽略 CR。接收端支持跨多次写入组装以及一次写入多行；单次写入最多 180 字节，
手机还须遵守协商 MTU。默认 MTU 23 时普通写入最多 20 字节。

```text
SET BPSK500 1500
REPLAY
WIFI
```

`SET` 可选 BPSK31/63/125/250/500/1000，频率边界与网页一致：
`floor(baud)+100 <= tone <= min(3500, 4000-floor(baud)-100)` Hz。
非法、超长、溢出数字不会进入解调控制队列。`cmd=QUEUED` 后读取 `rev`、`mode` 和
`tone` 检查实际应用；`BUSY/AUDIO_NOT_READY/INVALID` 分别表示队列忙、音频未就绪和
格式错误。`WIFI` 只请求切换，若 NVS 写入失败会留在当前方式并在串口日志报告。

新连接订阅 TX 后从最近 4096 字节开始；取消再订阅继续当前游标，`REPLAY` 将游标
复位到当前历史起点。每 25 ms 最多发送一次，长度为 `min(MTU-3, 180)`，默认 MTU
允许调度约 800 字节/秒。这是软件调度额度，实际无线吞吐由连接参数、手机和信号决定。
相比约 50/100 字符每秒的 BPSK500/1000 文本输出，额度有余量，但还没有手机实测。

音频任务只写固定历史缓冲；BLE 在自己的主机事件队列读取并发送。即时分配/通知失败
保留未发送游标，下个周期重试；协议栈已接受后的发送失败仅计数，手机可重连或 REPLAY。
BLE 通知没有应用层确认、去重或端到端校验。UTF-8 要跨通知连续解码；手机后台暂停、
断连及历史覆盖不能保证无丢字。当前服务不要求加密配对，只接收一台手机连接。

验证包括主机上的指令分片/超长恢复、数字溢出、六档模式、频率边界、20/180 字节分包、
历史覆盖和即时失败游标重试；按键包含抖动、短按、长按松开、毫秒计数回绕。ESP32-C3
目标编译通过，尚未做真实 BLE 连接、后台恢复、NVS 重启切换及与音频并发的硬件验收。

参考：[Espressif 蓝牙架构](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/bt-architecture/overview.html)、
[Nordic NUS UUID 定义](https://nrfconnectdocs.nordicsemi.com/ncs/latest/zephyr/doxygen/html/nus_8h.html)。
