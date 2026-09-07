# ESP32-C3 PSK 接收器

参考 `C:\projects\hpm6750-psk-modem` 的单载波接收流程，面向 ESP32-C3
重新实现定点 DSP。默认上电建立 Wi-Fi AP，也可切换 BLE，把收到的文字传给手机。

## 速率结论

- 默认 **BPSK500：500 波特，原始 500 bit/s**，优先用于实际音频链路。
- 实现上限 **BPSK1000：1000 波特，原始 1000 bit/s**。无附加噪声的主机
  交叉测试通过；这不是已经测出的 ESP32-C3 板上零误码上限。
- 同时支持 BPSK31 / 63 / 125 / 250，实际符号率为 31.25 / 62.5 / 125 / 250。
- Varicode 文本平均净吞吐低于原始比特率；按约 10 bit/字符估计，500 / 1000
  模式约 50 / 100 字符每秒，具体由文字内容决定，中文每个 UTF-8 字节单独编码。
- 旧工程 QPSK/RTTY 路径没有完整可用的解码实现，保留原始源码和枚举，但不再
  放入可选菜单。没有移植 HPM 的多载波、8PSK/FEC、图片协议、语音协议或发送功能。

详细评估、计算依据、测试边界和上板验收见 [docs/RATE_ASSESSMENT.md](docs/RATE_ASSESSMENT.md)。

## 手机连接

1. 烧录并上电，搜索 **ESP32C3-PSK-xxxx**；后四位取自 AP MAC，方便区别多块板。
2. 密码：**pskmodem123**。
3. 选择继续连接此无互联网的 Wi-Fi，浏览器打开 **http://192.168.4.1**。
4. 发射端与接收端选择相同 BPSK 模式和音频中心频率；默认 **BPSK500 / 1500 Hz**。
5. 手机页面可改模式/频率、查看接收文字、保存文本及检查 DMA 溢出和处理耗时。

不依赖 CDN 或互联网，不要求手机安装应用。此版本使用手动浏览器入口，
不包含强制门户自动弹窗。AP 最多 4 台手机，频道 6，WPA2-PSK。
解调模式/频率在当前会话有效，重新上电恢复默认。修改热点参数在 `main/board_config.h`。

板端保留最近 4096 字节，超过后覆盖最旧文字；手机游标落后时会显示丢失提示。
每台手机独立读取，网页最多保留 32768 个字符。“清空本页”只清空该手机显示，
保存文本只保存该手机现有内容。没有 CRC 的普通文本不能显示一个可信的实时 BER。

### 可选 BLE

Wi-Fi AP 与 BLE 二选一运行；输出方式保存到 NVS，切换需要重启，板端文字历史会清空。
在网页底部点击“切换到蓝牙（重启）”，或在**板子已运行时长按 SW1 两秒再松开**。
BLE 模式下，同样长按并松开 SW1 可切回 AP；短按仍打开本地菜单。
GPIO9 也是 BOOT 引脚，不要按住它给板子上电来选择输出方式。

ESP32-C3 支持 BLE，不支持传统蓝牙串口 SPP；使用兼容 Nordic UART Service 的手机
应用，例如 nRF Connect，直接在应用内扫描和连接，不需要先在手机系统设置中配对。

1. 扫描并连接 **ESP32C3-PSK-BLE-xxxx**，同一时间支持一台手机。
2. 找到 UART 服务 `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`。
3. 对 TX 特征 `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` 开启 Notify（通知），
   选择 UTF-8/文本显示，即可看到收到的内容；新连接会先发送板端仍保留的历史。
4. RX 特征 `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` 支持写入 ASCII 指令，
   每条以换行 LF 结束（也接受 CRLF）：`SET BPSK500 1500` 修改解调参数，
   `REPLAY` 重发当前保留的历史，`WIFI` 保存 AP 选择并重启。
5. 读取状态特征 `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` 可看模式、频率、
   音频状态、处理耗时、DMA 溢出、历史覆盖、通知错误和最后指令结果。
   `cmd=QUEUED` 表示配置入队，`rev` 增加且模式/频率匹配才表示已经应用。

此 GATT 服务不要求配对或加密，没有蓝牙口令。文本和状态分开传输；手机应用须
按字节流拼接通知后解 UTF-8，多字节字符可能跨通知边界。普通 BLE 调试工具可能
逐包显示，连续中文显示需要支持流式 UTF-8 的 UART 终端。
通知发送成功只表示协议栈接受数据，不是手机应用保存成功；断连、手机后台限制和
超过 4 KB 的历史覆盖都可能丢失显示内容。`REPLAY` 会重复现有历史，不能恢复已覆盖数据。

协议说明见 [BLE_PHONE.md](docs/BLE_PHONE.md)。

## 硬件与任务

沿用工程已有 LCKFB SZPI ESP32-C3 + ES8311 + ST7789 + FT6336/FT5x06 引脚：

- ES8311：I2C SDA GPIO0 / SCL GPIO1；I2S MCLK GPIO10、BCLK GPIO8、WS GPIO12、
  DOUT GPIO11、DIN GPIO7，16 kHz / 16 bit / stereo。
- 默认固定取左声道，防止原来按每块幅度切左右声道造成相位跳变。
  若板上 ES8311 输出在右槽，把 `BOARD_AUDIO_SLOT_LEFT` 改为 0。
- ES8311 模拟输入增益沿用 24 dB；输入电平过大时先衰减，观察页面削顶计数。
- LCD：ST7789 240×320，SPI2，SCLK GPIO3、MOSI GPIO5、CS GPIO4、DC GPIO6、背光 GPIO2。
- 触摸：与 ES8311 共用 I2C；SW1 GPIO9。
- 无 LCD/触摸的硬件可以把 `BOARD_LOCAL_UI_ENABLED` 改为 0，仍可用手机接收。

这里的音频来自 **ES8311 ADC 的 I2S 数据**，不是 ESP32-C3 内部 SAR ADC。
具体模拟输入焊接点、电平和元件改动须以当前板卡原理图为准；固件没有改引脚方案。

音频任务独占解码状态；控制请求经过队列在块边界应用。网页和 BLE 发送都不在音频任务中执行。
8×256 帧 DMA，约 128 ms 缓冲；47 抽头低通将 16 kHz 转成 8 kHz。
接收器与 FFT 使用整数运算，LCD 频谱更新降为 4 Hz。
若音频芯片初始化失败，AP 页面或 BLE 状态特征仍可显示初始化失败状态。

## 编译、烧录

当前环境沿用远端更新：ESP-IDF v5.5.5，ESP RISC-V GCC esp-14.2.0_20260121，
CPU 160 MHz，`-O2`。脚本使用匹配的 SDK/编译器，新输出放在 `build/idf55`，
避免混用原来的 IDF 6 构建缓存。

```powershell
cd C:\projects\esp32c3-psk-modem
.\tools\build.ps1
# 连接板子后，把 COMx 替换为实际串口；此命令会编译并烧录。
.\tools\build.ps1 -Port COMx
```

固件应用文件：`build/idf55/psk_modem_c3.bin`；标准 IDF 烧录按 `build/idf55/flasher_args.json`
同时写入 bootloader、分区表和应用。应用地址 0x10000，不能把应用 BIN 单独写到 0x0。
首次构建请保留 `sdkconfig.defaults`，当前使用 8 MB Flash / single-app-large 分区。

本次应用大小 1,411,008 字节，完整固件编译、72 个短流与 18 个长流测试，以及
BLE 指令/分包/游标重试和 SW1 手势测试通过。
构建哈希及内存报告在评估文档中。

## 主机验证

```powershell
.\tools\test_host.ps1
# Reference 参数可更换参考项目位置；测试使用其真实发送器代码，参考项目仅被读取。
.\tools\test_host.ps1 -Reference C:\projects\hpm6750-psk-modem
# 可选：浏览器页面的模拟接口，仅绑定本机。页面明确标注模拟数据。
python tests\preview_server.py
```

主机测试不等同于板端 AP 并发、BLE 手机连接和模式切换测试。当前工具调用未检测到串口设备，
本次没有烧录或取得板上 CPU 占用/物理音频 BER 数据。
