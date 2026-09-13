param(
  [string]$ObsSrc = "C:\obs-studio",
  [string]$QtPrefix = $(if ($env:QTDIR) { $env:QTDIR } else { "" }),
  [switch]$Install
)

# libobs includes <emmintrin.h> under MSVC on x86/x64; every other Windows target
# (MinGW, ARM) includes <simde/x86/sse2.h>, so SIMDe is fetched only when needed.
$simdeNotNeeded = ($env:VSCMD_ARG_TGT_ARCH -eq "x64" -or $env:VSCMD_ARG_TGT_ARCH -eq "x86")
if (-not $simdeNotNeeded -and -not (Test-Path "$ObsSrc\deps\simde\simde\x86\sse2.h")) {
  git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git "$ObsSrc\deps\simde"
}

$cmakeArgs = @("-S", $PSScriptRoot, "-B", "$PSScriptRoot\build-win", "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DOBS_SRC=$ObsSrc")
if ($QtPrefix) { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix" }
cmake @cmakeArgs
cmake --build "$PSScriptRoot\build-win"

if ($Install) {
  $dest = "$env:ProgramData\obs-studio\plugins\iso-recorder"
  New-Item -ItemType Directory -Force -Path "$dest\bin\64bit" | Out-Null
  New-Item -ItemType Directory -Force -Path "$dest\data" | Out-Null
  Copy-Item "$PSScriptRoot\build-win\iso-recorder.dll" "$dest\bin\64bit\" -Force
  Copy-Item "$PSScriptRoot\data\*" "$dest\data\" -Recurse -Force
  Write-Host "Installed to $dest. Restart OBS."
} else {
  Write-Host "Install: re-run with -Install to copy iso-recorder.dll to %ProgramData%\obs-studio\plugins\iso-recorder\bin\64bit\ and data\ to %ProgramData%\obs-studio\plugins\iso-recorder\data\"
}
