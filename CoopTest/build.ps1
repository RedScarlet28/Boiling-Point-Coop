# build.ps1 - builds CoopTest\bin\dinput.dll (the Boiling Point co-op test mod).
#   powershell -NoProfile -ExecutionPolicy Bypass -File CoopTest\build.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$msvc = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*" -Directory |
        Where-Object { Test-Path "$($_.FullName)\bin\Hostx86\x86\cl.exe" } | Sort-Object Name | Select-Object -Last 1
if (-not $msvc) { throw "MSVC x86 toolchain (cl.exe) not found" }
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk = Get-ChildItem "$kits\Lib" -Directory | Where-Object { Test-Path "$($_.FullName)\um\x86\kernel32.lib" } |
       Sort-Object Name | Select-Object -Last 1
$m = $msvc.FullName; $v = $sdk.Name
$env:PATH    = "$m\bin\Hostx86\x86;$env:PATH"
$env:INCLUDE = "$m\include;$kits\Include\$v\ucrt;$kits\Include\$v\um;$kits\Include\$v\shared"
$env:LIB     = "$m\lib\x86;$kits\Lib\$v\ucrt\x86;$kits\Lib\$v\um\x86"
New-Item -ItemType Directory -Force "$here\bin", "$here\obj" | Out-Null
& cl.exe /nologo /W3 /O2 /MT /D_CRT_SECURE_NO_WARNINGS /LD "$here\src\bp_coop.c" "/Fo$here\obj\\" "/Fe$here\bin\dinput.dll" /link kernel32.lib user32.lib "/DEF:$here\src\dinput.def" "/IMPLIB:$here\obj\dinput.lib"
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "built $here\bin\dinput.dll"
