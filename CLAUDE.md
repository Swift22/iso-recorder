# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

OBS Studio plugin (C++17, CMake, Qt6 dock) for OBS 32.x, macOS arm64 and Windows x64 only.

## Build and test

- macOS: `OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh [--install]` → `build-mac/`. `--install` copies only the `.plugin` bundle; restart OBS after.
- Windows builds happen on the MISTY box, not the Mac: use `/build-win`.
- Tests are NOT run by the build. Run them yourself: `ctest --test-dir build-mac --output-on-failure` (one: `-R session_writer` or `-R wav_writer`). CONTRIBUTING.md says otherwise; it is wrong.
- Only `session-writer` and `wav-writer` are testable. Their tests assert exact bytes against literal strings: edit the expected string on purpose, never paste new output over it.
- Anything touching OBS is verified by hand with the checklist in `README.md`. "Compiles" is not a test.
- CI (`.github/workflows/build-project.yaml`) has never been validated and does not run ctest. Don't treat it as a signal.

## Design authority

- `docs/spec.md` is the spec; `docs/adr/` records decisions that are expensive to reverse (e.g. the dock owns recording, not a filter). A change that contradicts either must say so in the commit/PR.
- `docs/plan.md` is a build log, not current truth. Terms: `docs/glossary.md`.

## Code rules

- Match OBS style: `.clang-format` is OBS's own. Existing code doesn't fully conform, so format only the lines you changed: `/opt/homebrew/opt/llvm/bin/git-clang-format` (after `git add`). Never run `clang-format -i` on a whole file.
- Lint changed files: `/opt/homebrew/opt/llvm/bin/clang-tidy -p build-mac --extra-arg=-isysroot"$(xcrun --show-sdk-path)" <file>` (needs a build first for `compile_commands.json`; without the sysroot arg it can't find `<optional>`).
- Comments only for OBS quirks or non-obvious constraints.
- Nothing fails quietly: every failure gets `blog(LOG_WARNING, "[iso-recorder] ...")` AND a plain-English `setStatus` in the dock, and a failed/aborted recording records its reason in `session.json`.
- Errors: return `bool` with a `std::string *error` out-param.
- One source failing never stops the others. The encoder warning asks, never refuses.
- Pair every `obs_*_get_ref` / `get_*` with a release. `source_remove` can arrive off the main thread: hop to UI with `obs_queue_task(OBS_TASK_UI, ...)`.
- Stop recordings on `OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN`, not just `EXIT`.
- UI strings stay hard-coded English `QStringLiteral`; `data/locale/en-US.ini` is vestigial.

## Gotchas

- Version lives in 6 places: `CMakeLists.txt`, `buildspec.json`, `data/manifest.json`, `packaging/windows/make-installer.ps1` (`$Version`), `packaging/windows/README.txt` line 1, `.github/ISSUE_TEMPLATE/bug_report.yml` placeholder. Use `/release`.
- `*.cmd`, `*.ps1`, and `packaging/windows/README.txt` must stay CRLF (`.gitattributes`); LF breaks `.cmd` labels/`goto`.

## Commits

- Subject: sentence-case imperative describing what the user sees ("Fall back to x264 when a hardware encoder refuses a source"). No conventional-commit prefixes. Body: prose on why.
- Release commits: `vX.Y.Z: ...` with a bullet list.
