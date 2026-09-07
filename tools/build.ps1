param([string]$Port = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
# Use the compiler declared by this ESP-IDF installation (15.2 for IDF 6.0.2).
$env:PATH = 'C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;' + $env:PATH
Push-Location $taskRoot
try {
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF build failed' }
    if ($Port) {
        idf.py -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF flash failed' }
    }
} finally { Pop-Location }
