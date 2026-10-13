@echo off
setlocal
cd /d "%~dp0"
echo.
echo  Configuracion inicial de shadPS4 para inFAMOUS Second Son
echo  ---------------------------------------------------------
echo  1) Registra el emulador de esta carpeta en la interfaz (shadPS4QtLauncher).
echo  2) Copia los ajustes recomendados del juego (CUSA00004.json).
echo  No borra nada: si ya tenias ajustes del juego, se guardan como copia .bak.
echo.
if not exist "%~dp0emulador\shadPS4.exe" (
    echo  ERROR: no encuentro emulador\shadPS4.exe. Descomprime todo el zip antes de ejecutar esto.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$here = (Resolve-Path '%~dp0').Path.TrimEnd('\');" ^
  "$exe = ($here + '\emulador\shadPS4.exe').Replace('\','/');" ^
  "$utf8 = New-Object System.Text.UTF8Encoding($false);" ^
  "$launcher = Join-Path $here 'launcher';" ^
  "New-Item -ItemType Directory -Force -Path $launcher | Out-Null;" ^
  "$versionsFile = Join-Path $launcher 'versions.json';" ^
  "$list = @(); if (Test-Path $versionsFile) { try { foreach ($v in (Get-Content -Raw $versionsFile | ConvertFrom-Json)) { $list += $v } } catch { $list = @() } };" ^
  "$list = @($list | Where-Object { $_.name -ne 'inFAMOUS SS (shadPS4 mod)' });" ^
  "$list += [pscustomobject]@{ name = 'inFAMOUS SS (shadPS4 mod)'; path = $exe; date = (Get-Date -Format 'yyyy-MM-dd'); codename = 'Local'; type = 2 };" ^
  "[IO.File]::WriteAllText($versionsFile, (ConvertTo-Json -InputObject $list -Depth 3), $utf8);" ^
  "$ini = Join-Path $launcher 'qt_ui.ini';" ^
  "$lines = @(); if (Test-Path $ini) { $lines = @(Get-Content $ini | Where-Object { $_ -notmatch '^versionSelected=' }) };" ^
  "$idx = [Array]::IndexOf($lines, '[version_manager]');" ^
  "if ($idx -lt 0) { $lines += '[version_manager]'; $lines += ('versionSelected=' + $exe) } else { $before = $lines[0..$idx]; $after = @(); if ($idx + 1 -lt $lines.Count) { $after = $lines[($idx + 1)..($lines.Count - 1)] }; $lines = $before + ('versionSelected=' + $exe) + $after };" ^
  "[IO.File]::WriteAllText($ini, (($lines -join \"`r`n\") + \"`r`n\"), $utf8);" ^
  "$cfgDir = Join-Path $env:APPDATA 'shadPS4\custom_configs';" ^
  "New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null;" ^
  "$dst = Join-Path $cfgDir 'CUSA00004.json';" ^
  "if (Test-Path $dst) { Copy-Item -Force $dst ($dst + '.bak') };" ^
  "Copy-Item -Force (Join-Path $here 'config\CUSA00004.json') $dst;" ^
  "Write-Host ('  Emulador registrado: ' + $exe);" ^
  "Write-Host ('  Ajustes del juego copiados a: ' + $dst)"
if errorlevel 1 (
    echo  ERROR: no se pudo completar la configuracion.
    pause
    exit /b 1
)
echo.
echo  Listo. Ahora abre shadPS4QtLauncher.exe (desde ESTA carpeta), agrega la carpeta
echo  donde tienes el juego y la actualizacion 1.07, y juega desde ahi.
echo.
pause
