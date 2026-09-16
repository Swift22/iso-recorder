---
name: release
description: Cut an iso-recorder release — bump the version everywhere, build on Windows, package the installer, and prepare the GitHub release.
disable-model-invocation: true
argument-hint: <new version, e.g. 0.5.0>
---

New version: `$ARGUMENTS`. If empty, ask for it.

## 1. Bump all 6 version spots

Replace the old version with the new one in:

- `CMakeLists.txt`: `project(iso-recorder VERSION x.y.z ...)`
- `buildspec.json`: `"version"`
- `data/manifest.json`: `"version"`
- `packaging/windows/make-installer.ps1`: `$Version` default
- `packaging/windows/README.txt`: line 1 (keep CRLF line endings)
- `.github/ISSUE_TEMPLATE/bug_report.yml`: version placeholder

Verify with `grep -rn "<old version>" --exclude-dir={dist,build-mac,.git,.omo} . | grep -v docs/plan.md`; it should print nothing.

## 2. Test on the Mac

`OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh && ctest --test-dir build-mac --output-on-failure`. Stop on failure.

## 3. Build and package on Windows

1. Run the `/build-win` steps (push, build, ctest).
2. Package, in the same dev-shell call style:
   `ssh win 'cd E:\dev\projects\iso-recorder; .\packaging\windows\make-installer.ps1'`
3. Pull the artifacts: `winpull 'E:\dev\projects\iso-recorder\dist' /Users/misty/Developer/personal/iso-recorder/dist`. Confirm `iso-recorder-<version>-windows-x64.zip` and `-setup.exe` exist locally.

## 4. Commit

Commit the bump as `v<version>: <short user-facing summary>`, with a bullet list of what changed since the last tag (`git log <last tag>..HEAD --oneline`).

## 5. Stop and confirm before anything public

Show the user the commit, the artifact list, and draft release notes. Only after an explicit yes:

```bash
git tag v<version> && git push origin main v<version>
gh release create v<version> dist/iso-recorder-<version>-windows-x64.zip dist/iso-recorder-<version>-windows-x64-setup.exe --title "v<version>" --notes-file <notes>
```

Release notes are for streamers: plain words, what they'll notice, no internals.
