# scripts/fetch_lua.ps1 - one-time vendoring of Lua 5.4 for the on-device VM.
#
# Run it once, then flash:
#
#     powershell -ExecutionPolicy Bypass -File scripts/fetch_lua.ps1
#     pio run -t upload
#
# Uses the tar.exe that ships with Windows 10 1803+ to unpack the .tar.gz.

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "lib\lua\src"
$Ini  = Join-Path $Root "platformio.ini"
$Ver  = if ($env:LUA_VERSION) { $env:LUA_VERSION } else { "5.4.7" }

Write-Host "[lua] 0N3P0rK - vendoring Lua $Ver into lib\lua\src"

$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("lua-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $Dest, $Tmp | Out-Null

try {
    $Url = "https://www.lua.org/ftp/lua-$Ver.tar.gz"
    $Tgz = Join-Path $Tmp "lua.tar.gz"
    Write-Host "[lua] downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Tgz -UseBasicParsing

    Write-Host "[lua] unpacking"
    & tar -xzf $Tgz -C $Tmp
    if ($LASTEXITCODE -ne 0) { throw "tar failed - Windows 10 1803+ or a tar on PATH is required" }

    $Src = Join-Path $Tmp "lua-$Ver\src"
    if (-not (Test-Path $Src)) {
        $Src = (Get-ChildItem -Path $Tmp -Directory -Recurse |
                Where-Object { $_.Name -eq "src" } |
                Select-Object -First 1).FullName
    }
    if (-not $Src -or -not (Test-Path $Src)) { throw "no src/ folder inside the archive" }

    # lua.c / luac.c each define main() and would fight the Arduino core.
    $skip = @("lua.c", "luac.c", "onelua.c")
    $copied = 0
    Get-ChildItem -Path $Src -File |
        Where-Object {
            ($_.Extension -eq ".c" -or $_.Extension -eq ".h") -and
            ($skip -notcontains $_.Name)
        } |
        ForEach-Object { Copy-Item $_.FullName -Destination $Dest -Force; $copied++ }
    Write-Host "[lua] copied $copied files to lib\lua\src"

    if (-not (Test-Path (Join-Path $Dest "lua.h"))) {
        throw "lua.h missing - archive layout unexpected"
    }

    $text = Get-Content -Path $Ini -Raw
    if ($text -match '(?m)^\s*;-DPORK_LUA=1') {
        $text = [regex]::Replace($text, '(?m)^\s*;-DPORK_LUA=1', '    -DPORK_LUA=1')
        Set-Content -Path $Ini -Value $text -NoNewline
        Write-Host "[lua] enabled -DPORK_LUA=1 in platformio.ini"
    } elseif ($text -match '(?m)^\s*-DPORK_LUA=1') {
        Write-Host "[lua] -DPORK_LUA=1 already enabled"
    } else {
        Write-Warning "[lua] could not find the -DPORK_LUA=1 line - add it by hand"
    }

    Write-Host "[lua] done.  Now run:  pio run -t upload"
    Write-Host "[lua] then copy examples\scripts\*.lua to /0N3P0rK/scripts/ on the card"
}
finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}