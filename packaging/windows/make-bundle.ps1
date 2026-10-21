# Assembles the standalone Windows DS64 bundle:
#
#   DS64-windows-x86_64/
#     melonDS.exe          (static Qt, static CRT)
#     python/              python-build-standalone-style runtime + site-packages
#     tools/               gesture/ and phone/ embedded Python scripts
#     make_controller_qr.py, lan_ip.py
#     web/                 controller.html / scan.html
#
# melonDS finds this layout next to the .exe at runtime (see
# ds64ConfigureBundledRuntime() in src/frontend/qt_sdl/main.cpp), so the game
# boots its embedded ROM, generates QR codes, and serves the controller page
# without any launcher script.

param(
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$Python,
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = "Stop"
# "Stop" doesn't cover native programs, so every pip/python call below
# checks $LASTEXITCODE: a failing pip would otherwise leave a bundle without
# mediapipe/OpenCV and still "succeed".

if (Test-Path $OutDir) {
    Remove-Item -Path $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutDir | Out-Null

Write-Host "==> Staging melonDS.exe"
Copy-Item -Path $Binary -Destination (Join-Path $OutDir "melonDS.exe")

# Install DS64's runtime side exactly the way the AppImage does: mediapipe
# without its declared dependencies (which drag in jax/scipy/sounddevice and
# a second OpenCV), then the small set of packages the embedded scripts
# actually import. This also avoids pip installing both opencv-python and
# mediapipe's opencv-contrib-python into the same cv2 directory.
$RuntimeRequirements = Join-Path $RepoRoot "packaging\linux\requirements-appimage.txt"
Write-Host "==> Installing Python dependencies into the bundled runtime"
& $Python -m pip install --disable-pip-version-check --no-input -q --no-deps "mediapipe==0.10.14"
if ($LASTEXITCODE -ne 0) { throw "pip install mediapipe failed ($LASTEXITCODE)" }
& $Python -m pip install --disable-pip-version-check --no-input -q -r $RuntimeRequirements
if ($LASTEXITCODE -ne 0) { throw "pip install -r $RuntimeRequirements failed ($LASTEXITCODE)" }

$PythonHome = & $Python -c "import sys; print(sys.base_prefix)"
if ($LASTEXITCODE -ne 0 -or -not $PythonHome) { throw "could not query the Python prefix" }
Write-Host "==> Copying Python runtime from $PythonHome"
$PythonOut = Join-Path $OutDir "python"
New-Item -ItemType Directory -Path $PythonOut | Out-Null
Copy-Item -Path (Join-Path $PythonHome "*") -Destination $PythonOut -Recurse -Force

# The loader resolves melonDS.exe's python312.dll/vcruntime140.dll imports
# before main() runs, so those have to sit next to the executable.
Get-ChildItem -Path $PythonHome -Filter "*.dll" -File |
    Copy-Item -Destination $OutDir -Force

# Python's own path setup is more reliable than environment variables here:
# python312.dll may have a separate copy of the static CRT's environment
# block, so a PYTHONHOME set by melonDS.exe could be invisible to it. A
# <dllname>._pth file next to the DLL is read by CPython regardless.
$PythonDll = Get-ChildItem -Path $PythonHome -Filter "python3*.dll" -File |
    Where-Object { $_.Name -match '^python3\d+\.dll$' } |
    Select-Object -First 1
if ($PythonDll) {
    $PthPath = Join-Path $OutDir ($PythonDll.BaseName + "._pth")
    @(
        "python\Lib",
        "python\Lib\site-packages",
        "python\DLLs",
        ".",
        "import site"
    ) | Set-Content -Path $PthPath -Encoding ascii
}

# Camera/phone native extensions (mediapipe/OpenCV) need the Visual C++
# runtime DLLs. Python ships vcruntime140 but not necessarily msvcp140.
foreach ($Dll in @("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                   "concrt140.dll", "vcomp140.dll")) {
    $Source = Join-Path $env:SystemRoot "System32\$Dll"
    if (Test-Path $Source) {
        Copy-Item -Path $Source -Destination $OutDir -Force
    }
}

# Not needed at runtime; mirrors the slimming the AppImage build does.
foreach ($Junk in @("test", "idlelib", "tkinter", "turtledemo", "ensurepip")) {
    $Path = Join-Path $PythonOut "Lib\$Junk"
    if (Test-Path $Path) {
        Remove-Item -Path $Path -Recurse -Force
    }
}
$ScriptsDir = Join-Path $PythonOut "Scripts"
if (Test-Path $ScriptsDir) {
    Remove-Item -Path $ScriptsDir -Recurse -Force
}

Write-Host "==> Adding DS64 scripts and controller page"
$ToolsOut = Join-Path $OutDir "tools"
New-Item -ItemType Directory -Path $ToolsOut | Out-Null
Copy-Item -Path (Join-Path $RepoRoot "vendor\melonDS-src\tools\gesture") -Destination $ToolsOut -Recurse
Copy-Item -Path (Join-Path $RepoRoot "vendor\melonDS-src\tools\phone") -Destination $ToolsOut -Recurse
Copy-Item -Path (Join-Path $RepoRoot "tools\make_controller_qr.py") -Destination $OutDir
Copy-Item -Path (Join-Path $RepoRoot "tools\lan_ip.py") -Destination $OutDir
Copy-Item -Path (Join-Path $RepoRoot "web") -Destination $OutDir -Recurse

Get-ChildItem -Path $OutDir -Recurse -Directory -Filter "__pycache__" |
    ForEach-Object { Remove-Item -Path $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }

$Size = (Get-ChildItem -Path $OutDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("==> Bundle ready: {0} ({1:N1} MB)" -f $OutDir, ($Size / 1MB))
