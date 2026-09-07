param([string]$Port = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
# Match the remote project's ESP-IDF 5.5.5 and its required compiler.
$env:PATH = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;' + $env:PATH
Push-Location $taskRoot
try {
    # Keep the earlier IDF 6 build/cache separate from this SDK's output.
    idf.py -B build/idf55 build
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build failed' }
    if ($Port) {
        idf.py -B build/idf55 -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF flash failed' }
    }
} finally { Pop-Location }
