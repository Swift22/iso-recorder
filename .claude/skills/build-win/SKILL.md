---
name: build-win
description: Build, test, and optionally install the iso-recorder plugin on the Windows box (MISTY) over ssh. Use when asked to build or test on Windows, check a change compiles under MSVC, or install the DLL into Windows OBS.
---

The Mac is the source of truth. Windows only holds a working copy at `E:\dev\projects\iso-recorder`.

## Paths already used on MISTY (from its CMakeCache)

- OBS source checkout: `E:\dev\obs-studio`
- Prefix path: `E:\Tools\QtSDK\6.8.3\msvc2022_64;E:\dev\obs-libs` (OBS libs MUST be on it or `find_library` fails)
- Compiler: VS 2022 BuildTools, MSVC 14.44. Its dev shell also provides ninja and sets `VSCMD_ARG_TGT_ARCH` (which lets `build-win.ps1` skip SIMDe).

## Steps

1. Push: `winpush /Users/misty/Developer/personal/iso-recorder`. It excludes `.git`, `dist`, `build`, but not `build-mac`, and never deletes stale files on the far side.
2. Build and test in one ssh call (dev shell env doesn't survive across calls):

   ```bash
   ssh win '& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null; cd E:\dev\projects\iso-recorder; .\build-win.ps1 -ObsSrc E:\dev\obs-studio -QtPrefix "E:\Tools\QtSDK\6.8.3\msvc2022_64;E:\dev\obs-libs"; if ($LASTEXITCODE -eq 0) { ctest --test-dir build-win --output-on-failure }'
   ```

   Run it with a timeout (a cold build takes a few minutes). A non-zero exit is a failure even if output scrolls past quietly.
3. Install only when asked: add `-Install` to the `build-win.ps1` call. First check OBS is closed (`ssh win 'Get-Process obs64 -ErrorAction SilentlyContinue'`), or the DLL won't be replaced. If OBS is running, tell the user; don't kill it (they may be streaming).
4. Report: build result, ctest output, and whether it was installed. Anything touching OBS still needs the README checklist done by hand in OBS.
