# ESP32-C3 PSK Modem

ESP-IDF project for the LCKFB SZPI ESP32-C3 board used as an audio-input
digital-mode receiver. The hardware plan is to remove R27/R28 and R31/R32,
then inject AM receiver audio into the board ADC path.

The firmware is structured around the modes present in `nuepsk_source`:

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

The old NUE-PSK source is dsPIC-oriented and depends on dsPIC registers,
memory attributes, and Microchip DSP library types. This project keeps
NUE-PSK as the single source for mode coverage, RTTY behavior, menus, and
receiver workflow. No third-party PSK modem library is used.

## DSP Strategy

`components/nuepsk` is the ESP-IDF porting layer for `nuepsk_source`.
Current modes are the modes defined by the NUE firmware:

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

The component already uses NUE's Varicode table and RTTY Baudot table. The
remaining porting work is to move the FIR, AFC, symbol sync, and QPSK Viterbi
logic out of `nuepsk_source/C Source/PskDet.c` while replacing dsPIC-only
`fractional`, `FIRDecimate`, `FFTComplexIP`, register, and LCD calls with
portable ESP32 code.

## UI

- Top half: audio spectrum, centered around the selected receive tone.
- Bottom half: decoded text stream.
- SW1: open/close mode menu.
- Touch in menu: select modulation mode.
- Touch on spectrum: change receive tone.

## Hardware Defaults

Pin assignments are taken from
`C:\projects\lichuang-esp32c3-demo\08-spi_lcd_touch\spi_lcd_touch` and
`02-gpio_key`.

Edit `main/board_config.h` if your board revision is different:

- `BOARD_AUDIO_ADC_CHANNEL`
- `BOARD_SW1_GPIO`
- `BOARD_LCD_*`
- `BOARD_TOUCH_*`

Current display/touch defaults:

- LCD: ST7789, 240x320, SPI2, SCLK GPIO3, MOSI GPIO5, CS GPIO4, DC GPIO6,
  backlight GPIO2.
- Touch: FT6336/FT5x06, I2C SDA GPIO0, SCL GPIO1.
- SW1: GPIO9.

## Build

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
$env:PATH='C:\Espressif\tools\tools\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin;' + $env:PATH
idf.py set-target esp32c3
idf.py build
```
