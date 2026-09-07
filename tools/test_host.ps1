param(
    [string]$Reference = 'C:\projects\hpm6750-psk-modem',
    [string]$Compiler = 'C:\cygwin64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
    New-Item -ItemType Directory -Force build\host | Out-Null
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    & $Compiler -std=c11 -O2 -Wall -Wextra -I main -I components\nuepsk\include `
        -I "$Reference\app\modem" -I "$Reference\third_party\psk_rtty\include" `
        tests\host_bpsk.c main\bpsk_rx.c main\audio_decimator.c "$Reference\app\modem\psk_tx.c" `
        "$Reference\app\modem\psk_modes.c" "$Reference\app\modem\psk_codec.c" `
        -lm -o build\host\host_bpsk.exe
    if ($LASTEXITCODE -ne 0) { throw 'Host compile failed' }
    & .\build\host\host_bpsk.exe
    if ($LASTEXITCODE -ne 0) { throw 'BPSK regression failed' }
    & $Compiler -std=c11 -O2 -Wall -Wextra -Werror -I main -I components\nuepsk\include `
        tests\host_buffers.c main\rx_history.c main\audio_decimator.c main\bpsk_rx.c main\modem.c main\spectrum.c `
        -lm -o build\host\host_buffers.exe
    if ($LASTEXITCODE -ne 0) { throw 'Buffer test compile failed' }
    & .\build\host\host_buffers.exe
    if ($LASTEXITCODE -ne 0) { throw 'Buffer tests failed' }
    & $Compiler -std=c11 -O2 -Wall -Wextra -ftrapv -I main -I components\nuepsk\include `
        -I "$Reference\app\modem" -I "$Reference\third_party\psk_rtty\include" `
        tests\host_long.c main\bpsk_rx.c main\audio_decimator.c "$Reference\app\modem\psk_tx.c" `
        "$Reference\app\modem\psk_modes.c" "$Reference\app\modem\psk_codec.c" `
        -lm -o build\host\host_long.exe
    if ($LASTEXITCODE -ne 0) { throw 'Long stream compile failed' }
    & .\build\host\host_long.exe
    if ($LASTEXITCODE -ne 0) { throw 'Long stream regression failed' }
} finally { Pop-Location }
