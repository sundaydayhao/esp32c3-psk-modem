# ESP32-C3 PSK 接收器

参考 `C:\projects\hpm6750-psk-modem` 的单载波接收流程，面向 ESP32-C3
重新实现定点 DSP。上电建立 Wi-Fi AP，手机可以实时查看收到的文字。

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
配置当前会话有效，重新上电恢复默认。修改热点参数在 `main/board_config.h`。

板端保留最近 4096 字节，超过后覆盖最旧文字；手机游标落后时会显示丢失提示。
每台手机独立读取，网页最多保留 32768 个字符。“清空本页”只清空该手机显示，
保存文本只保存该手机现有内容。没有 CRC 的普通文本不能显示一个可信的实时 BER。

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

音频任务独占解码状态；控制请求经过队列在块边界应用。采样不受网页发送阻塞。
8×256 帧 DMA，约 128 ms 缓冲；47 抽头低通将 16 kHz 转成 8 kHz。
接收器与 FFT 使用整数运算，LCD 频谱更新降为 4 Hz。
若音频芯片初始化失败，AP 页面仍可显示初始化失败状态。

## 编译、烧录

当前环境：ESP-IDF v6.0.2，ESP RISC-V GCC esp-15.2.0_20251204，CPU 160 MHz，`-O2`。
使用匹配的编译器，不沿用旧 README 中的 GCC 16 路径。

```powershell
cd C:\projects\esp32c3-psk-modem
.\tools\build.ps1
# 连接板子后，把 COMx 替换为实际串口；此命令会编译并烧录。
.\tools\build.ps1 -Port COMx
```

固件应用文件：`build/psk_modem_c3.bin`；标准 IDF 烧录按 `build/flasher_args.json`
同时写入 bootloader、分区表和应用。应用地址 0x10000，不能把应用 BIN 单独写到 0x0。
首次构建请保留 `sdkconfig.defaults`，当前使用 8 MB Flash / single-app-large 分区。

本次最终应用大小 1,178,672 字节，完整固件编译、72 个短流与 18 个长流测试通过。
构建哈希及内存报告在评估文档中。

## 主机验证

```powershell
.\tools\test_host.ps1
# Reference 参数可更换参考项目位置；测试使用其真实发送器代码，参考项目仅被读取。
.\tools\test_host.ps1 -Reference C:\projects\hpm6750-psk-modem
# 可选：浏览器页面的模拟接口，仅绑定本机。页面明确标注模拟数据。
python tests\preview_server.py
```

主机测试不等同于板端 AP 并发压力测试。当前工具调用未检测到串口设备，
本次没有烧录或取得板上 CPU 占用/物理音频 BER 数据。
