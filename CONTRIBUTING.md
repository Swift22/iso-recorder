# Contributing

Thanks for wanting to help. This is a small plugin with one job — record every OBS
source to its own file — and the bar for a change is simple: recording gets more
reliable, or the dock gets easier to understand.

## Before you start

- **OBS Studio 32.x is the target.** The plugin links `libobs` and the frontend API,
  which are ABI-sensitive: a new OBS major means a rebuild, and headers from a
  different version can compile but misbehave at runtime.
- **`docs/spec.md` is the design authority.** `docs/plan.md` says how it was built and
  `docs/adr/` records the decisions that are expensive to reverse. If your change
  contradicts one of those, say so in the pull request — an argument is welcome, a
  silent divergence is not.
- **Windows x64 and macOS arm64 are the supported platforms.** Linux was never built;
  if you get it working, that is a fine pull request.

## Building

Both platforms need an `obs-studio` **source checkout** for headers — the installed OBS
ships none — at a tag matching the OBS you run:

```
git clone --depth 1 --branch 32.2.2 https://github.com/obsproject/obs-studio.git
```

**macOS:** Qt6 (`brew install qtbase`), CMake, Ninja.

```
OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh          # → build-mac/iso-recorder.plugin
OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh --install  # → ~/Library/Application Support/obs-studio/plugins/
```

`build-mac.sh` fetches the header-only SIMDe library into the checkout, rewrites the
plugin's Qt load commands to OBS's `@rpath` form, and re-signs it ad-hoc. Restart OBS
to pick up a new build.

**Windows:** Visual Studio 2022 (x64), Qt6, CMake, Ninja.

```
powershell -File build-win.ps1 -ObsSrc C:\obs-studio            # → build-win\iso-recorder.dll
powershell -File build-win.ps1 -ObsSrc C:\obs-studio -Install   # → %ProgramData%\obs-studio\plugins\iso-recorder\
```

Close OBS before installing: Windows will not replace a DLL that is loaded, and the
installer refuses rather than leaving a half-updated plugin in place.

## Testing

Two modules are pure C++ with no OBS and no Qt — `session-writer` (file names,
`session.json`, `README.txt`) and `wav-writer` (the 24-bit WAV header and writer). They
are covered by tests that run as part of the build:

```
ctest --test-dir build-mac --output-on-failure
```

If you touch them, or add another pure helper, add or extend a test. The tests assert
**exact bytes** — the expected `session.json` and README strings are literal in the test
file. When a format changes, change the expectation deliberately; do not paste the new
output over the old one without reading the diff.

Anything that only runs inside OBS has to be checked by hand. `README.md` has the manual
checklist; the short version is: tick one visual source, record ten seconds, stop, and
confirm the file plays and `session.json` says `complete`.

## Reporting a bug

Use the bug template. The most useful things to include:

- the plugin version, from the OBS log line `[iso-recorder] loaded (v…)`
- your OBS version and platform
- which sources you ticked, and what came out
- the log around the failure — `%APPDATA%\obs-studio\logs` on Windows,
  `~/Library/Application Support/obs-studio/logs` on macOS

Logs contain file paths, which often include your user name. Skim before pasting, and
trim anything you would rather not publish.

## Pull requests

- One change per pull request. A small diff gets reviewed; a rewrite does not.
- Say what you tested, how, and on which platform. "Compiles" is not a test.
- Match the file you are editing: tabs, C++17, and no comments unless the line encodes
  something non-obvious (an OBS quirk, a format constraint). Comments that restate the
  code are noise — the design record carries the *why*.
- Do not reformat untouched code. A diff that is mostly whitespace hides the change.

Your contribution is licensed under the GPL-2.0, the same as the rest of the project.
