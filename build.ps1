# Builds expere.exe with the MSYS2 UCRT64 toolchain.
$ErrorActionPreference = 'Stop'
$bin = 'C:\msys64\ucrt64\bin'
$env:PATH = "$bin;$env:PATH"
Set-Location $PSScriptRoot

& "$bin\windres.exe" -O coff -o expere.res expere.rc
if ($LASTEXITCODE) { exit $LASTEXITCODE }

& "$bin\gcc.exe" -std=gnu11 -Os -s -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows -static-libgcc `
    -ffunction-sections -fdata-sections '-Wl,--gc-sections' `
    -o expere.exe expere.c expere.res `
    -luser32 -lgdi32 -lshell32 -lole32 -luuid -lcomctl32 -ldwmapi -lshcore -lpowrprof -lshlwapi -ladvapi32 -loleaut32
if ($LASTEXITCODE) { exit $LASTEXITCODE }

"Built {0} ({1:N0} KB)" -f (Resolve-Path expere.exe), ((Get-Item expere.exe).Length / 1KB)
