# Implementation plan — ISO Recorder

Status: the build brief for the OBS plugin. `docs/spec.md` is the spec; this
file says how it gets built, in what order, and what "done" means for each task. Every
implementation agent reads the spec before its task; where the two disagree, the spec wins.

This plugin is not part of the Toast pipeline. `CONTEXT.md` vocabulary does not apply here; **ISO**
means one isolated recording of one source. Commits go straight to `main`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** An OBS 32 plugin whose dock records each ticked source to its own frame-synced file, one
folder per stream, for an external editor.

**Architecture:** A single in-process C++ plugin. The dock owns arming and settings; an
`IsoSession` owns one `SourceRecorder` per armed source; each recorder taps its source
pre-composite — a private `obs_view_t` for picture, a private `audio_output` for sound — runs its
own encoder, and muxes to its own file. `session-writer` is the one pure seam (filenames,
`session.json`, `README.txt`) and is the only part with automated tests. Everything that needs a
running OBS is verified with the manual checklist in `README.md`.

**Tech Stack:** C++17, libobs, obs-frontend-api, Qt6 Widgets, CMake ≥ 3.20, ctest. OBS 32.x,
macOS arm64 and Windows x64.

**Spec:** `docs/spec.md`

## Risk order

The spec names its own risk: the mechanism is proven, the multi-recorder session and long-session
sync are not. Build in this order and **stop to measure before finishing the dock**:

1. Tasks 1–3 make the project build and the pure seam correct.
2. Tasks 4–6 produce the first real recordings and the first two-recorder session.
3. **Run Task 6's drift and encoder-ceiling measurements before starting Task 7.** If audio drifts
   against video over hours, the fix (shared audio clock or periodic resync) changes the recorder
   and the session, and the dock must not be built on top of the wrong one.
4. Tasks 7–10 are the UI, wiring, failure handling and packaging.

## Global Constraints

Copied from the spec; every task's requirements include these.

- Target **OBS 32.x** on both platforms; build the dock with **Qt6**.
- New top-level folder `iso-recorder/`. Nothing in `studio/`, `emote-forge/`, `site/`.
- Plugin id `iso-recorder`; dock id `iso-recorder-dock`; config section `iso-recorder`.
- Files: `NN_<sanitized>.<ext>`, video and audio numbered **independently** in dock order; a second
  segment of one source is suffixed `_2`, then `_3`. Extensions `.mov` (video) and `.wav` (audio).
- Default base path: macOS `~/Movies/Mist ISO/<YYYY-MM-DD HH-MM-SS>/`, Windows
  `E:\Stream\Mist\ISO\<YYYY-MM-DD HH-MM-SS>\`. Both overridable in the dock.
- Audio default: **WAV, 24-bit PCM, 48 kHz** (`pcm_s24le`). Video default: the stream's hardware
  encoder family, falling back to the platform preferred (VideoToolbox on macOS; NVENC, then QSV,
  then AMF on Windows), then `obs_x264`.
- Video container: the crash-tolerant hybrid MOV (`mov_output`). Audio-only files are written by us
  as WAV; no OBS output accepts an audio-only stream (verified — see the spec's mechanism notes).
- Every recorder started for one arm event is started inside **one** `obs_queue_task(OBS_TASK_UI,
  …)`. No OBS call is made from the Qt thread directly.
- Manifest `version` is `1`. `startOffset` is seconds from the session epoch, frame-quantized for
  video, `null` when the file never recorded.
- `session-writer` touches no OBS object, no filesystem, no clock. It is the only automated test
  target.
- No comments in code unless a non-obvious OBS constraint needs one.

## File structure

```
iso-recorder/
  CMakeLists.txt
  cmake/
    mac/Info.plist.in            macOS .plugin bundle Info.plist
    win/plugin.rc                Windows version resource (optional)
  build-mac.sh
  build-win.ps1
  README.md                      build + install + the manual checklist
  data/locale/en-US.ini          UI strings
  src/
    plugin-main.cpp              module load/unload, dock registration, frontend events
    iso-dock.hpp / .cpp          the Qt dock (UI only; asks IsoSession)
    iso-session.hpp / .cpp       one session: recorders, epoch, arm/disarm/start/stop, manifest
    source-recorder.hpp / .cpp   one source's capture + encode + mux
    session-writer.hpp / .cpp    pure: filenames, session.json, README.txt
    wav-writer.hpp / .cpp        pure header builder + the WAV file consumer
    iso-settings.hpp / .cpp      persisted dock settings (wraps a config section)
  tests/
    test_session_writer.cpp
    test_wav_writer.cpp
```

`session-writer` and the header builder in `wav-writer` are pure and tested. `wav-writer`'s file
I/O, `iso-settings`, `source-recorder`, `iso-session`, `iso-dock` and `plugin-main` all touch OBS
and are verified by the manual checklist.

---

## Task 1: Project skeleton that builds and loads in OBS

The build is the fiddliest part of this plugin and the first thing to get right. This Mac's
`/Applications/OBS.app` ships `libobs.framework`, `obs-frontend-api.dylib` and the Qt frameworks
but **no headers**, so the build takes headers from an `obs-studio` source checkout and links the
installed app's libraries.

**Files:**
- Create: `iso-recorder/CMakeLists.txt`
- Create: `iso-recorder/cmake/mac/Info.plist.in`
- Create: `iso-recorder/src/plugin-main.cpp`
- Create: `iso-recorder/data/locale/en-US.ini`
- Create: `iso-recorder/build-mac.sh`
- Create: `iso-recorder/build-win.ps1`
- Create: `README.md`
- Modify: `.gitignore` (append `iso-recorder/build*/`)

**Interfaces:**
- Produces: `MODULE` target `iso-recorder`; exported `obs_module_load()` / `obs_module_unload()`;
  a shared `iso_log(const char *fmt, ...)`-style module logger in `plugin-main.cpp`.

- [ ] **Step 1: Get the OBS headers on disk**

The headers must match the installed OBS 32.x. Clone the OBS source at the machine's tag (both
machines), into the path the build default expects:

```bash
git clone --depth 1 --branch 32.0.1 https://github.com/obsproject/obs-studio.git \
  "$HOME/Developer/obs-studio"
```

Also confirm the toolchain:

```bash
xcode-select -p          # mac
brew list --versions qt cmake ninja
```

Expected: the clone succeeds and `$HOME/Developer/obs-studio/libobs/obs.h` exists. On Windows use
`git clone` into `C:\obs-studio`, and install Qt6, CMake and Ninja (Qt online installer or
`aqtinstall`) — the Windows build needs the same headers plus the OBS install's import libraries.

- [ ] **Step 2: Write the CMake project**

`iso-recorder/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(iso-recorder VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OBS_SRC "" CACHE PATH "Path to an obs-studio source checkout (headers)")
set(OBS_APP "" CACHE PATH "Path to the installed OBS.app (macOS)")

find_package(Qt6 REQUIRED COMPONENTS Widgets)

if(NOT OBS_SRC)
  message(FATAL_ERROR "Pass -DOBS_SRC=<obs-studio checkout>")
endif()

set(OBS_INCLUDES
  "${OBS_SRC}/libobs"
  "${OBS_SRC}/frontend/api"
  "${OBS_SRC}/deps/obs-scripting"        # harmless if absent
)
include_directories(SYSTEM ${OBS_INCLUDES})
add_compile_definitions(OBS_SWIG=1)

if(APPLE)
  if(NOT OBS_APP)
    set(OBS_APP "/Applications/OBS.app")
  endif()
  set(OBS_FW "${OBS_APP}/Contents/Frameworks")
  find_library(OBS_LIB obs PATHS "${OBS_FW}" REQUIRED)
  find_library(OBS_FRONTEND_LIB obs-frontend-api PATHS "${OBS_FW}" REQUIRED)
elseif(WIN32)
  find_library(OBS_LIB obs REQUIRED)
  find_library(OBS_FRONTEND_LIB obs-frontend-api REQUIRED)
else()
  message(FATAL_ERROR "macOS and Windows only")
endif()

add_library(iso-recorder MODULE
  src/plugin-main.cpp
  src/iso-dock.cpp
  src/iso-session.cpp
  src/source-recorder.cpp
  src/session-writer.cpp
  src/wav-writer.cpp
  src/iso-settings.cpp
)
target_link_libraries(iso-recorder PRIVATE
  Qt6::Widgets ${OBS_LIB} ${OBS_FRONTEND_LIB})

set_target_properties(iso-recorder PROPERTIES PREFIX "" SUFFIX ".plugin" OUTPUT_NAME iso-recorder)
if(APPLE)
  set_target_properties(iso-recorder PROPERTIES MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mac/Info.plist.in")
endif()

enable_testing()
add_executable(test_session_writer tests/test_session_writer.cpp src/session-writer.cpp)
target_include_directories(test_session_writer PRIVATE src)
add_test(NAME session_writer COMMAND test_session_writer)
add_executable(test_wav_writer tests/test_wav_writer.cpp src/wav-writer.cpp)
target_include_directories(test_wav_writer PRIVATE src)
add_test(NAME wav_writer COMMAND test_wav_writer)
```

Every later task adds its sources to this target; the file list above is the finished set, which
is why later tasks only need to write the `.cpp`. (OBS's plugin template does the same.)

- [ ] **Step 3: Write the macOS bundle plist**

`iso-recorder/cmake/mac/Info.plist.in`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>iso-recorder</string>
  <key>CFBundleIdentifier</key><string>tech.misty.iso-recorder</string>
  <key>CFBundleName</key><string>ISO Recorder</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
</dict>
</plist>
```

- [ ] **Step 4: Write the module entry with a log line**

`iso-recorder/src/plugin-main.cpp`:

```cpp
#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ISO Recorder — one file per source while streaming";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[iso-recorder] loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[iso-recorder] unloaded");
}
```

`iso-recorder/data/locale/en-US.ini`:

```ini
iso-recorder="ISO Recorder"
```

- [ ] **Step 5: Build and confirm OBS loads it**

`iso-recorder/build-mac.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
OBS_SRC="${OBS_SRC:-$HOME/Developer/obs-studio}"
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
cmake -S "$(dirname "$0")" -B "$(dirname "$0")/build-mac" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SRC="$OBS_SRC" -DOBS_APP="$OBS_APP"
cmake --build "$(dirname "$0")/build-mac"
echo "Built. Install with:"
echo "  cp -R build-mac/iso-recorder.plugin \"$HOME/Library/Application Support/obs-studio/plugins/\""
```

Run:

```bash
chmod +x iso-recorder/build-mac.sh && iso-recorder/build-mac.sh
```

Expected: `build-mac/iso-recorder.plugin/Contents/MacOS/iso-recorder` exists. Install it, start
OBS, open Help → Log Files → View Current Log. Expected: a line containing `[iso-recorder] loaded`.

`iso-recorder/build-win.ps1`:

```powershell
param([string]$ObsSrc = "C:\obs-studio")
$root = Split-Path $PSScriptRoot -Parent
cmake -S $PSScriptRoot -B "$PSScriptRoot\build-win" -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SRC="$ObsSrc"
cmake --build "$PSScriptRoot\build-win"
Write-Host "Install: copy iso-recorder.dll to %ProgramData%\obs-studio\plugins\iso-recorder\bin\64bit\"
```

- [ ] **Step 6: Write the README skeleton and ignore the build dir**

`README.md` starts with how to build and install per platform, and carries a
`## Manual checklist` heading that later tasks fill. `.gitignore` gains `iso-recorder/build*/`.

- [ ] **Step 7: Commit**

```bash
git add iso-recorder .gitignore
git commit -m "Bring up the ISO Recorder plugin: builds and loads in OBS 32"
```

---

## Task 2: `session-writer` — the pure seam

Everything the editor reads is produced here, and it is the only code with automated tests. Write
the tests first.

**Files:**
- Create: `iso-recorder/src/session-writer.hpp`
- Create: `iso-recorder/src/session-writer.cpp`
- Test: `iso-recorder/tests/test_session_writer.cpp`

**Interfaces:**
- Produces: namespace `iso`; `enum class Kind { Video, Audio }`; `enum class Status { Recording,
  Complete, Aborted, Failed }`; `struct Recording`, `struct VideoFormat`, `struct SessionInfo`;
  `sanitizeName`, `makeFilename`, `extensionFor`, `kindName`, `statusName`, `formatClock`,
  `formatSeconds`, `buildSessionJson`, `buildReadme`. Every later task uses these names exactly.

- [ ] **Step 1: Write the header and the failing tests**

`iso-recorder/src/session-writer.hpp`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace iso {

enum class Kind { Video, Audio };
enum class Status { Recording, Complete, Aborted, Failed };

struct Recording {
	std::string source;
	Kind kind = Kind::Video;
	std::string file;                  // "01_game.mov"; empty when it never recorded
	std::string codec;                 // "h264", "pcm_s24le"; empty when unknown
	std::optional<double> startOffset; // seconds from the session epoch; nullopt => JSON null
	std::optional<double> duration;    // seconds; nullopt => JSON null
	Status status = Status::Recording;
	std::string error;                 // failed only, plain words
	std::string label;                 // README only; empty => falls back to `source`
};

struct VideoFormat {
	int width = 1920;
	int height = 1080;
	double fps = 60.0;
};

struct SessionInfo {
	std::string started;               // "2026-09-12T14:32:05+03:00"
	std::string dateLine;              // "12 September 2026", for the README
	std::optional<std::string> ended;
	std::optional<double> duration;
	std::string obsVersion;            // "32.0.1"
	std::string platform;              // "macos" | "windows"
	VideoFormat video;
};

const char *kindName(Kind kind);
const char *statusName(Status status);
const char *extensionFor(Kind kind);

std::string sanitizeName(const std::string &source);
std::string makeFilename(Kind kind, int index, const std::string &source, int segment);
std::string formatClock(double seconds);
std::string formatSeconds(double seconds);

std::string buildSessionJson(const SessionInfo &info, const std::vector<Recording> &recordings);
std::string buildReadme(const SessionInfo &info, const std::vector<Recording> &recordings);

} // namespace iso
```

`iso-recorder/tests/test_session_writer.cpp` — a dependency-free harness:

```cpp
#include "session-writer.hpp"

#include <cstdio>
#include <string>

using namespace iso;

static int failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
			++failures;                                                      \
		}                                                                        \
	} while (0)

#define CHECK_EQ(got, want)                                                              \
	do {                                                                             \
		std::string _g = (got);                                                      \
		std::string _w = (want);                                                     \
		if (_g != _w) {                                                              \
			std::printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n", __FILE__,    \
				    __LINE__, #got, _g.c_str(), _w.c_str());                \
			++failures;                                                          \
		}                                                                        \
	} while (0)

int main()
{
	/* sanitize */
	CHECK_EQ(sanitizeName("Game Capture"), "Game_Capture");
	CHECK_EQ(sanitizeName("Discord/voice"), "Discord_voice");
	CHECK_EQ(sanitizeName("  spaced  "), "spaced");
	CHECK_EQ(sanitizeName("???"), "source");

	/* filenames */
	CHECK_EQ(makeFilename(Kind::Video, 1, "game", 1), "01_game.mov");
	CHECK_EQ(makeFilename(Kind::Video, 2, "veado", 1), "02_veado.mov");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 1), "03_Mic.wav");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 2), "03_Mic_2.wav");
	CHECK_EQ(makeFilename(Kind::Audio, 3, "Mic", 3), "03_Mic_3.wav");

	/* clock */
	CHECK_EQ(formatClock(0.0), "0:00.0");
	CHECK_EQ(formatClock(10.5), "0:10.5");
	CHECK_EQ(formatClock(65.0), "1:05.0");
	CHECK_EQ(formatClock(4210.5), "1:10:10.5");
	CHECK_EQ(formatClock(3599.9), "59:59.9");

	/* JSON numbers keep one decimal, extras trimmed */
	CHECK_EQ(formatSeconds(0.0), "0.0");
	CHECK_EQ(formatSeconds(8123.4), "8123.4");
	CHECK_EQ(formatSeconds(0.017), "0.017");

	/* the full manifest, byte for byte */
	SessionInfo info;
	info.started = "2026-09-12T14:32:05+03:00";
	info.ended = "2026-09-12T16:47:28+03:00";
	info.duration = 8123.4;
	info.obsVersion = "32.0.1";
	info.platform = "macos";
	info.video = {1920, 1080, 60.0};

	std::vector<Recording> recs;
	recs.push_back({"game", Kind::Video, "01_game.mov", "h264", 0.0, 8123.4,
			Status::Complete, "", "the game, on its own"});
	recs.push_back({"ofes", Kind::Video, "", "", std::nullopt, std::nullopt,
			Status::Failed, "encoder refused: too many sessions", "ofes"});

	const std::string json = buildSessionJson(info, recs);
	const std::string want =
		"{\n"
		"  \"version\": 1,\n"
		"  \"session\": {\n"
		"    \"started\": \"2026-09-12T14:32:05+03:00\",\n"
		"    \"ended\": \"2026-09-12T16:47:28+03:00\",\n"
		"    \"duration\": 8123.4,\n"
		"    \"obs\": \"32.0.1\",\n"
		"    \"platform\": \"macos\",\n"
		"    \"video\": { \"width\": 1920, \"height\": 1080, \"fps\": 60 }\n"
		"  },\n"
		"  \"recordings\": [\n"
		"    { \"source\": \"game\", \"kind\": \"video\", \"file\": "
		"\"01_game.mov\", \"codec\": \"h264\", \"startOffset\": 0.0, "
		"\"duration\": 8123.4, \"status\": \"complete\" },\n"
		"    { \"source\": \"ofes\", \"kind\": \"video\", \"file\": null, "
		"\"codec\": null, \"startOffset\": null, \"duration\": null, "
		"\"status\": \"failed\", \"error\": \"encoder refused: too many sessions\" }\n"
		"  ]\n"
		"}\n";
	CHECK_EQ(json, want);

	/* the README lists every recording, in order, with its start time */
	const std::string readme = buildReadme(info, recs);
	CHECK(readme.find("  01_game.mov       0:00.0        the game, on its own") !=
	      std::string::npos);
	const size_t gamePos = readme.find("01_game.mov");
	const size_t ofesPos = readme.find("ofes");
	CHECK(gamePos != std::string::npos && ofesPos != std::string::npos && gamePos < ofesPos);
	CHECK(readme.find("not recorded") != std::string::npos);

	if (failures == 0)
		std::printf("session-writer: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build iso-recorder/build-mac && ctest --test-dir iso-recorder/build-mac -R session_writer --output-on-failure`
Expected: link error — `sanitizeName`, `buildSessionJson`, etc. are undefined. (The test compiles
against the header but cannot link without the `.cpp`.)

- [ ] **Step 3: Implement `session-writer.cpp`**

```cpp
#include "session-writer.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace iso {

const char *kindName(Kind kind) { return kind == Kind::Audio ? "audio" : "video"; }

const char *statusName(Status status)
{
	switch (status) {
	case Status::Complete: return "complete";
	case Status::Aborted: return "aborted";
	case Status::Failed: return "failed";
	case Status::Recording: break;
	}
	return "recording";
}

const char *extensionFor(Kind kind) { return kind == Kind::Audio ? "wav" : "mov"; }

std::string sanitizeName(const std::string &source)
{
	std::string out;
	bool pending = false;
	for (unsigned char c : source) {
		if (std::isalnum(c) || c == '.' || c == '-') {
			out.push_back(static_cast<char>(c));
			pending = false;
		} else if (!out.empty()) {
			pending = true;
		}
		if (pending && !out.empty() && out.back() != '_')
			out.push_back('_');
	}
	while (!out.empty() && out.back() == '_')
		out.pop_back();
	return out.empty() ? "source" : out;
}

std::string makeFilename(Kind kind, int index, const std::string &source, int segment)
{
	char num[8];
	std::snprintf(num, sizeof num, "%02d", index);
	std::string name = std::string(num) + "_" + sanitizeName(source);
	if (segment > 1)
		name += "_" + std::to_string(segment);
	name += ".";
	name += extensionFor(kind);
	return name;
}

std::string formatSeconds(double seconds)
{
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.4f", seconds);
	std::string t = buf;
	while (t.size() > 1 && t.back() == '0')
		t.pop_back();
	if (!t.empty() && t.back() == '.')
		t.push_back('0');
	return t;
}

std::string formatClock(double seconds)
{
	if (seconds < 0)
		seconds = 0;
	int total = static_cast<int>(std::floor(seconds));
	int tenths = static_cast<int>(std::llround((seconds - total) * 10.0));
	if (tenths == 10) {
		++total;
		tenths = 0;
	}
	const int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	char buf[32];
	if (h > 0)
		std::snprintf(buf, sizeof buf, "%d:%02d:%02d.%d", h, m, s, tenths);
	else
		std::snprintf(buf, sizeof buf, "%d:%02d.%d", m, s, tenths);
	return buf;
}

static void jsonString(std::string &o, const std::string &s)
{
	o.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				o += b;
			} else {
				o.push_back(static_cast<char>(c));
			}
		}
	}
	o.push_back('"');
}

static void jsonNum(std::string &o, const std::optional<double> &v)
{
	o += v ? formatSeconds(*v) : "null";
}

static void jsonStr(std::string &o, const std::string &v)
{
	if (v.empty())
		o += "null";
	else
		jsonString(o, v);
}

std::string buildSessionJson(const SessionInfo &info, const std::vector<Recording> &recs)
{
	std::string o;
	o += "{\n  \"version\": 1,\n  \"session\": {\n";
	o += "    \"started\": ";
	jsonString(o, info.started);
	o += ",\n    \"ended\": ";
	if (info.ended)
		jsonString(o, *info.ended);
	else
		o += "null";
	o += ",\n    \"duration\": ";
	jsonNum(o, info.duration);
	o += ",\n    \"obs\": ";
	jsonString(o, info.obsVersion);
	o += ",\n    \"platform\": ";
	jsonString(o, info.platform);
	char v[128];
	std::snprintf(v, sizeof v,
		      ",\n    \"video\": { \"width\": %d, \"height\": %d, \"fps\": %.0f }\n  },\n",
		      info.video.width, info.video.height, info.video.fps);
	o += v;
	o += "  \"recordings\": [\n";
	for (size_t i = 0; i < recs.size(); ++i) {
		const Recording &r = recs[i];
		o += "    { \"source\": ";
		jsonString(o, r.source);
		o += ", \"kind\": ";
		jsonString(o, kindName(r.kind));
		o += ", \"file\": ";
		jsonStr(o, r.file);
		o += ", \"codec\": ";
		jsonStr(o, r.codec);
		o += ", \"startOffset\": ";
		jsonNum(o, r.startOffset);
		o += ", \"duration\": ";
		jsonNum(o, r.duration);
		o += ", \"status\": ";
		jsonString(o, statusName(r.status));
		if (r.status == Status::Failed) {
			o += ", \"error\": ";
			jsonString(o, r.error);
		}
		o += " }";
		o += (i + 1 < recs.size()) ? ",\n" : "\n";
	}
	o += "  ]\n}\n";
	return o;
}

std::string buildReadme(const SessionInfo &info, const std::vector<Recording> &recs)
{
	std::string o;
	o += "Separate recordings from the stream on " + info.dateLine + ".\n\n";
	o += "Every file is timed from the same clock — the moment the recording session\n";
	o += "began. Most start right at the beginning; a track switched on later starts\n";
	o += "later, and its start time is listed below. Place each clip at its start time\n";
	o += "and they all line up.\n\n";
	for (const Recording &r : recs) {
		const std::string file = r.file.empty() ? "--" : r.file;
		const std::string clock = r.startOffset ? formatClock(*r.startOffset) : "--";
		std::string desc = r.label.empty() ? r.source : r.label;
		if (r.status == Status::Failed)
			desc += " (not recorded: " + r.error + ")";
		char line[512];
		std::snprintf(line, sizeof line, "  %-18s%-14s%s\n", file.c_str(), clock.c_str(),
			      desc.c_str());
		o += line;
	}
	o += "\nNothing is cut, mixed, or ducked — the tracks are each source exactly as\n";
	o += "OBS saw it. session.json has the exact start of every file if you ever\n";
	o += "need to nudge one.\n";
	return o;
}

} // namespace iso
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build iso-recorder/build-mac && ctest --test-dir iso-recorder/build-mac -R session_writer --output-on-failure`
Expected: `session-writer: all checks passed` and `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add iso-recorder/src/session-writer.hpp iso-recorder/src/session-writer.cpp iso-recorder/tests/test_session_writer.cpp
git commit -m "Add session-writer: filenames, session.json and README, all pure and tested"
```

---

## Task 3: `wav-writer` — the pure header builder and the file writer

No OBS output accepts an audio-only stream, so audio-only sources (the mic, the Discord tap) are
written by us: a private `audio_output` callback delivers PCM, and we write a WAV. The byte layout
is pure and tested; the file I/O is verified by hand in Task 5.

**Files:**
- Create: `iso-recorder/src/wav-writer.hpp`
- Create: `iso-recorder/src/wav-writer.cpp`
- Test: `iso-recorder/tests/test_wav_writer.cpp`

**Interfaces:**
- Produces: `iso::buildWavHeader(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels,
  uint16_t bitsPerSample) -> std::vector<uint8_t>`; `class iso::WavWriter` with
  `bool open(const std::string &path, uint32_t sampleRate, uint16_t channels, uint16_t
  bitsPerSample)`, `void write(const float *const *planar, size_t frames)`, `void close()`.
- Consumes: nothing.

- [ ] **Step 1: Write the failing test**

`iso-recorder/tests/test_wav_writer.cpp`:

```cpp
#include "wav-writer.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace iso;

static int failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
			++failures;                                                      \
		}                                                                        \
	} while (0)

int main()
{
	const auto h = buildWavHeader(96000, 48000, 2, 24);
	CHECK(h.size() == 44);
	CHECK(std::memcmp(h.data(), "RIFF", 4) == 0);
	CHECK(std::memcmp(h.data() + 8, "WAVE", 4) == 0);
	CHECK(std::memcmp(h.data() + 12, "fmt ", 4) == 0);
	CHECK(std::memcmp(h.data() + 36, "data", 4) == 0);

	auto u32 = [&](size_t at) {
		return (uint32_t)h[at] | ((uint32_t)h[at + 1] << 8) | ((uint32_t)h[at + 2] << 16) |
		       ((uint32_t)h[at + 3] << 24);
	};
	auto u16 = [&](size_t at) {
		return (uint16_t)((uint16_t)h[at] | ((uint16_t)h[at + 1] << 8));
	};
	CHECK(u32(4) == 36u + 96000u);  // RIFF chunk size
	CHECK(u16(20) == 1);            // PCM
	CHECK(u16(22) == 2);            // channels
	CHECK(u32(24) == 48000u);       // sample rate
	CHECK(u32(28) == 48000u * 2u * 3u); // byte rate
	CHECK(u16(32) == 2u * 3u);      // block align
	CHECK(u16(34) == 24);           // bits per sample
	CHECK(u32(40) == 96000u);       // data size

	if (failures == 0)
		std::printf("wav-writer: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build iso-recorder/build-mac && ctest --test-dir iso-recorder/build-mac -R wav_writer --output-on-failure`
Expected: link error — `buildWavHeader` undefined.

- [ ] **Step 3: Implement the header and the writer**

`iso-recorder/src/wav-writer.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace iso {

std::vector<uint8_t> buildWavHeader(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels,
				    uint16_t bitsPerSample);

class WavWriter {
public:
	bool open(const std::string &path, uint32_t sampleRate, uint16_t channels,
		  uint16_t bitsPerSample);
	void write(const float *const *planar, size_t frames);
	void close();

private:
	std::FILE *file_ = nullptr;
	uint64_t dataBytes_ = 0;
	uint32_t sampleRate_ = 48000;
	uint16_t channels_ = 2;
	uint16_t bits_ = 24;
};

} // namespace iso
```

`iso-recorder/src/wav-writer.cpp`:

```cpp
#include "wav-writer.hpp"

#include <cmath>
#include <cstring>

namespace iso {

static void putU32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)(x & 0xff));
	v.push_back((uint8_t)((x >> 8) & 0xff));
	v.push_back((uint8_t)((x >> 16) & 0xff));
	v.push_back((uint8_t)((x >> 24) & 0xff));
}

static void putU16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back((uint8_t)(x & 0xff));
	v.push_back((uint8_t)((x >> 8) & 0xff));
}

static void putTag(std::vector<uint8_t> &v, const char *tag)
{
	v.insert(v.end(), tag, tag + 4);
}

std::vector<uint8_t> buildWavHeader(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels,
				    uint16_t bitsPerSample)
{
	const uint16_t blockAlign = (uint16_t)(channels * bitsPerSample / 8);
	std::vector<uint8_t> v;
	v.reserve(44);
	putTag(v, "RIFF");
	putU32(v, 36u + dataBytes);
	putTag(v, "WAVE");
	putTag(v, "fmt ");
	putU32(v, 16);
	putU16(v, 1); // PCM
	putU16(v, channels);
	putU32(v, sampleRate);
	putU32(v, sampleRate * blockAlign);
	putU16(v, blockAlign);
	putU16(v, bitsPerSample);
	putTag(v, "data");
	putU32(v, dataBytes);
	return v;
}

bool WavWriter::open(const std::string &path, uint32_t sampleRate, uint16_t channels,
		     uint16_t bitsPerSample)
{
	sampleRate_ = sampleRate;
	channels_ = channels;
	bits_ = bitsPerSample;
	dataBytes_ = 0;
	file_ = std::fopen(path.c_str(), "wb");
	if (!file_)
		return false;
	const auto header = buildWavHeader(0, sampleRate, channels, bits);
	return std::fwrite(header.data(), 1, header.size(), file_) == header.size();
}

void WavWriter::write(const float *const *planar, size_t frames)
{
	if (!file_)
		return;
	/* interleave to 24-bit little-endian PCM */
	std::vector<uint8_t> out(frames * channels_ * 3);
	size_t k = 0;
	for (size_t f = 0; f < frames; ++f) {
		for (uint16_t c = 0; c < channels_; ++c, k += 3) {
			int32_t s = (int32_t)std::lrint(std::fmin(1.0, std::fmax(-1.0, planar[c][f])) *
						       8388607.0);
			out[k] = (uint8_t)(s & 0xff);
			out[k + 1] = (uint8_t)((s >> 8) & 0xff);
			out[k + 2] = (uint8_t)((s >> 16) & 0xff);
		}
	}
	dataBytes_ += out.size();
	std::fwrite(out.data(), 1, out.size(), file_);
}

void WavWriter::close()
{
	if (!file_)
		return;
	const auto header = buildWavHeader((uint32_t)dataBytes_, sampleRate_, channels_, bits_);
	std::fseek(file_, 0, SEEK_SET);
	std::fwrite(header.data(), 1, header.size(), file_);
	std::fclose(file_);
	file_ = nullptr;
}

} // namespace iso
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build iso-recorder/build-mac && ctest --test-dir iso-recorder/build-mac -R wav_writer --output-on-failure`
Expected: `wav-writer: all checks passed`, `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add iso-recorder/src/wav-writer.hpp iso-recorder/src/wav-writer.cpp iso-recorder/tests/test_wav_writer.cpp
git commit -m "Add wav-writer: 24-bit PCM header and file writer, header tested"
```

## Task 4: `SourceRecorder` — one video source to one file, plus the thinnest dock that triggers it

This is the prototype the spec warns about. The goal is one `.mov` from one source, with the
pre-composite picture, playable. Everything else grows from here.

**Files:**
- Create: `iso-recorder/src/source-recorder.hpp`
- Create: `iso-recorder/src/source-recorder.cpp`
- Create: `iso-recorder/src/iso-session.hpp`
- Create: `iso-recorder/src/iso-session.cpp`
- Create: `iso-recorder/src/iso-dock.hpp`
- Create: `iso-recorder/src/iso-dock.cpp`
- Modify: `iso-recorder/src/plugin-main.cpp` (register the dock)

**Interfaces:**
- Produces: `iso::SourceRecorder` (constructor takes source, name, output path, video encoder id
  and settings, `Kind`, audio codec; `start(std::string *error)`, `stop()`, `active()`,
  `startOffsetNs()`, `durationSeconds()`, `sourceName()`, `path()`, `codec()`).
  `iso::IsoSession` skeleton (`active()`, `start()`, `stop()`, `arm()`, `disarm()`, `epochNs()`,
  `folder()`, `recordings()`). `iso::IsoDock(QWidget *)` exposing `refreshSources()`.

- [ ] **Step 1: Write `source-recorder.hpp`**

```cpp
#pragma once

#include <obs.h>

#include <cstdint>
#include <string>

#include "session-writer.hpp"

namespace iso {

class SourceRecorder {
public:
	SourceRecorder(obs_source_t *source, std::string name, std::string path,
		       std::string videoEncoderId, obs_data_t *videoSettings, Kind kind,
		       std::string audioCodec);
	~SourceRecorder();

	bool start(std::string *error);
	void stop();

	bool active() const { return started_; }
	bool failed() const { return failed_; }
	const std::string &error() const { return error_; }
	const std::string &sourceName() const { return name_; }
	const std::string &path() const { return path_; }
	const std::string &codec() const { return codec_; }
	uint64_t startOffsetNs() const { return startOffsetNs_; }
	double durationSeconds() const;

	void noteStart(uint64_t epochNs);

private:
	static void onPcm(void *param, size_t mixIdx, struct audio_data *data);
	static bool onAudioInput(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
				 uint32_t activeMixers, struct audio_output_data *mixes);
	void fillAudio(uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
		       struct audio_output_data *mixes);
	void mixComposite(struct audio_output_data *mixes, size_t mixIdx);
	void mixPlain(struct audio_output_data *mixes, size_t mixIdx);

	obs_source_t *source_ = nullptr; // strong ref, released in dtor
	std::string name_;
	std::string path_;
	std::string codec_;
	Kind kind_;
	bool hasVideo_ = false;
	bool hasAudio_ = false;
	std::string videoEncoderId_;
	obs_data_t *videoSettings_ = nullptr; // owned copy
	std::string audioCodec_;

	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	obs_encoder_t *videoEnc_ = nullptr;
	obs_encoder_t *audioEnc_ = nullptr;
	obs_output_t *output_ = nullptr;

	uint64_t startNs_ = 0;
	uint64_t epochNs_ = 0;
	uint64_t startOffsetNs_ = 0;
	bool started_ = false;
	bool failed_ = false;
	std::string error_;
};

} // namespace iso
```

- [ ] **Step 2: Write `source-recorder.cpp` — video path first**

```cpp
#include "source-recorder.hpp"

#include <cmath>
#include <utility>

namespace iso {

SourceRecorder::SourceRecorder(obs_source_t *source, std::string name, std::string path,
			       std::string videoEncoderId, obs_data_t *videoSettings, Kind kind,
			       std::string audioCodec)
	: source_(obs_source_get_ref(source)), name_(std::move(name)), path_(std::move(path)),
	  kind_(kind), videoEncoderId_(std::move(videoEncoderId)),
	  audioCodec_(std::move(audioCodec))
{
	const uint32_t flags = obs_source_get_output_flags(source_);
	hasVideo_ = (flags & OBS_SOURCE_VIDEO) != 0;
	hasAudio_ = (flags & OBS_SOURCE_AUDIO) != 0;
	if (videoSettings) {
		const char *json = obs_data_get_json(videoSettings);
		if (json)
			videoSettings_ = obs_data_create_from_json(json);
	}
}

SourceRecorder::~SourceRecorder()
{
	stop();
	if (videoSettings_)
		obs_data_release(videoSettings_);
	if (source_)
		obs_source_release(source_);
}

double SourceRecorder::durationSeconds() const
{
	if (!started_ || startNs_ == 0)
		return 0.0;
	return (double)(os_gettime_ns() - startNs_) / 1e9;
}

void SourceRecorder::noteStart(uint64_t epochNs)
{
	epochNs_ = epochNs;
	startNs_ = os_gettime_ns();
	startOffsetNs_ = startNs_ - epochNs_;
}

bool SourceRecorder::start(std::string *error)
{
	if (!hasVideo_) {
		*error = "audio-only start is added in Task 5";
		return false;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		*error = "OBS has no video yet";
		return false;
	}
	uint32_t w = obs_source_get_width(source_);
	uint32_t h = obs_source_get_height(source_);
	if (w == 0 || h == 0) {
		*error = "the source has no picture yet";
		return false;
	}
	w += (w & 1);
	h += (h & 1);
	ovi.base_width = w;
	ovi.base_height = h;
	ovi.output_width = w;
	ovi.output_height = h;

	view_ = obs_view_create();
	video_ = obs_view_add2(view_, &ovi);
	if (!video_) {
		*error = "could not make a private video output";
		return false;
	}
	obs_view_set_source(view_, 0, source_);
	obs_source_inc_showing(source_);

	videoEnc_ = obs_video_encoder_create(videoEncoderId_.c_str(), name_.c_str(),
					     videoSettings_, nullptr);
	if (!videoEnc_) {
		*error = "the encoder did not open";
		return false;
	}
	obs_encoder_set_video(videoEnc_, video_);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "path", path_.c_str());
	output_ = obs_output_create("mov_output", name_.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!output_) {
		*error = "the container did not open";
		return false;
	}
	obs_output_set_video_encoder(output_, videoEnc_);
	if (!obs_output_start(output_)) {
		*error = obs_output_get_last_error(output_);
		if (error->empty())
			*error = "the output refused to start";
		return false;
	}
	codec_ = "h264";
	started_ = true;
	return true;
}

void SourceRecorder::stop()
{
	if (!started_ && !view_)
		return;
	started_ = false;
	if (output_) {
		obs_output_force_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (audioEnc_)
		obs_encoder_release(audioEnc_);
	if (videoEnc_)
		obs_encoder_release(videoEnc_);
	audioEnc_ = nullptr;
	videoEnc_ = nullptr;
	if (view_) {
		obs_source_dec_showing(source_);
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
}

void SourceRecorder::onPcm(void *, size_t, struct audio_data *) {}

bool SourceRecorder::onAudioInput(void *, uint64_t, uint64_t, uint64_t *, uint32_t,
				  struct audio_output_data *)
{
	return false;
}

void SourceRecorder::fillAudio(uint64_t, uint64_t, uint64_t *, uint32_t,
			       struct audio_output_data *)
{
}

void SourceRecorder::mixComposite(struct audio_output_data *, size_t) {}
void SourceRecorder::mixPlain(struct audio_output_data *, size_t) {}

} // namespace iso
```

The audio stubs are filled in Task 5. Build now to prove the video path compiles.

- [ ] **Step 3: Write the session skeleton and the dock**

`iso-recorder/src/iso-session.hpp`:

```cpp
#pragma once

#include <obs.h>

#include <memory>
#include <string>
#include <vector>

#include "session-writer.hpp"

namespace iso {

struct SessionConfig {
	std::string basePath;
	std::string videoEncoderId;
	obs_data_t *videoSettings = nullptr;            // borrowed for the call
	std::string audioCodec = "pcm_s24le";
	uint64_t bitrateBitsPerSec = 6000ull * 1000ull; // drives the disk check in Task 9
	bool recordComposite = false;
	bool withStream = true;
};

class IsoSession {
public:
	bool active() const { return active_; }
	bool start(const SessionConfig &cfg, std::string *error);
	void stop();
	bool arm(obs_source_t *source, std::string *error);
	void disarm(obs_source_t *source);
	void onSourceRemoved(obs_source_t *source);
	uint64_t epochNs() const { return epochNs_; }
	const std::string &folder() const { return folder_; }
	std::vector<Recording> recordings() const;

private:
	struct Entry {
		obs_source_t *source = nullptr;
		std::string name;
		std::string label;
		Kind kind = Kind::Video;
		int index = 1;
		int segment = 1;
		std::unique_ptr<class SourceRecorder> recorder;
	};
	Entry *find(obs_source_t *source);
	void startEntry(Entry &e);

	bool active_ = false;
	std::string folder_;
	uint64_t epochNs_ = 0;
	std::vector<Entry> entries_;
	std::vector<Recording> finished_;
};

} // namespace iso
```

`iso-recorder/src/iso-session.cpp` — enough to arm, start one recorder, stop, and produce a
manifest for the Task 4 test:

```cpp
#include "iso-session.hpp"

#include "source-recorder.hpp"

#include <ctime>

namespace iso {

static std::tm localTime(std::time_t t)
{
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	return tm;
}

static std::string nowStamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[32];
	std::strftime(b, sizeof b, "%Y-%m-%d %H-%M-%S", &tm);
	return b;
}

IsoSession::Entry *IsoSession::find(obs_source_t *source)
{
	for (auto &e : entries_)
		if (e.source == source)
			return &e;
	return nullptr;
}

bool IsoSession::arm(obs_source_t *source, std::string *error)
{
	if (find(source))
		return true;
	Entry e;
	e.source = obs_source_get_ref(source);
	e.name = obs_source_get_name(source);
	e.kind = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) ? Kind::Video : Kind::Audio;
	int index = 1;
	for (auto &o : entries_)
		if (o.kind == e.kind)
			++index;
	e.index = index;
	entries_.push_back(std::move(e));
	if (active_)
		startEntry(entries_.back());
	(void)error;
	return true;
}

void IsoSession::disarm(obs_source_t *source)
{
	if (Entry *e = find(source)) {
		if (e->recorder)
			e->recorder->stop();
		obs_source_release(e->source);
		entries_.erase(entries_.begin() + (e - entries_.data()));
	}
}

void IsoSession::startEntry(Entry &e)
{
	e.recorder = std::make_unique<SourceRecorder>(e.source, e.name, folder_ + "/pending.mov",
						      std::string("obs_x264"), nullptr, e.kind,
						      "pcm_s24le");
	std::string err;
	if (!e.recorder->start(&err)) {
		blog(LOG_WARNING, "[iso-recorder] %s: %s", e.name.c_str(), err.c_str());
	}
}

bool IsoSession::start(const SessionConfig &cfg, std::string *error)
{
	folder_ = cfg.basePath;
	os_mkdirs(folder_.c_str());
	epochNs_ = os_gettime_ns();
	active_ = true;
	for (auto &e : entries_)
		startEntry(e);
	(void)error;
	return true;
}

void IsoSession::stop()
{
	for (auto &e : entries_)
		if (e.recorder)
			e.recorder->stop();
	active_ = false;
}

std::vector<Recording> IsoSession::recordings() const { return finished_; }

void IsoSession::onSourceRemoved(obs_source_t *source) { disarm(source); }

} // namespace iso
```

File naming, offsets, the manifest and re-arm are Task 6; this skeleton exists only so the dock
can start one recorder now.

`iso-recorder/src/iso-dock.hpp`:

```cpp
#pragma once

#include <QWidget>

#include "iso-session.hpp"

class QListWidget;
class QLabel;

namespace iso {

class IsoDock : public QWidget {
	Q_OBJECT
public:
	explicit IsoDock(IsoSession *session, QWidget *parent = nullptr);
	void refreshSources();

signals:
	void sourceAdded(obs_source_t *source);
	void sourceRemoved(obs_source_t *source);

private slots:
	void onRecordClicked();

private:
	IsoSession *session_;
	QListWidget *sources_ = nullptr;
	QLabel *status_ = nullptr;
};

} // namespace iso
```

`iso-recorder/src/iso-dock.cpp`:

```cpp
#include "iso-dock.hpp"

#include <obs.h>

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace iso {

IsoDock::IsoDock(IsoSession *session, QWidget *parent) : QWidget(parent), session_(session)
{
	auto *layout = new QVBoxLayout(this);
	sources_ = new QListWidget(this);
	auto *button = new QPushButton(tr("Record"), this);
	status_ = new QLabel(this);
	layout->addWidget(sources_);
	layout->addWidget(button);
	layout->addWidget(status_);
	connect(button, &QPushButton::clicked, this, &IsoDock::onRecordClicked);
	refreshSources();
}

void IsoDock::refreshSources()
{
	sources_->clear();
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *self = static_cast<IsoDock *>(param);
			const uint32_t flags = obs_source_get_output_flags(source);
			if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
				return true;
			auto *item = new QListWidgetItem(obs_source_get_name(source), self->sources_);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(Qt::Unchecked);
			return true;
		},
		this);
}

void IsoDock::onRecordClicked()
{
	if (session_->active()) {
		session_->stop();
		status_->setText(tr("stopped"));
		return;
	}
	SessionConfig cfg;
	cfg.basePath = "/tmp/iso-test";
	std::string err;
	if (!session_->start(cfg, &err))
		status_->setText(QString::fromStdString(err));
	else
		status_->setText(tr("recording"));
}

} // namespace iso
```

- [ ] **Step 4: Register the dock in `plugin-main.cpp`**

```cpp
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>

#include "iso-dock.hpp"
#include "iso-session.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ISO Recorder — one file per source while streaming";
}

static iso::IsoSession *g_session = nullptr;
static iso::IsoDock *g_dock = nullptr;

bool obs_module_load(void)
{
	g_session = new iso::IsoSession();
	g_dock = new iso::IsoDock(g_session);
	obs_frontend_add_dock_by_id("iso-recorder-dock", "ISO Recorder", g_dock);
	blog(LOG_INFO, "[iso-recorder] loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_dock("iso-recorder-dock");
	delete g_dock;
	delete g_session;
	g_dock = nullptr;
	g_session = nullptr;
	blog(LOG_INFO, "[iso-recorder] unloaded");
}
```

- [ ] **Step 5: Build, install, and record one source by hand**

```bash
iso-recorder/build-mac.sh
rm -rf "$HOME/Library/Application Support/obs-studio/plugins/iso-recorder.plugin"
cp -R iso-recorder/build-mac/iso-recorder.plugin "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS. Expected: an **ISO Recorder** dock. Tick one video source, click Record for a few
seconds, click Stop. Expected: `/tmp/iso-test/pending.mov` exists and plays in QuickTime, showing
that source's picture alone (not the composite), at the source's own size.

- [ ] **Step 6: Commit**

```bash
git add iso-recorder/src
git commit -m "Record one video source on its own through a private obs_view"
```

---

## Task 5: The audio path

Two jobs, one callback: an audio-only source becomes a `.wav` we write, and a video source's audio
is muxed into its `.mov` as AAC. Both are fed by the same private `audio_output` and
`onAudioInput`, which must handle a plain source and a composite one (a scene, or anything with
children).

**Files:**
- Modify: `iso-recorder/src/source-recorder.hpp`
- Modify: `iso-recorder/src/source-recorder.cpp`
- Modify: `iso-recorder/src/wav-writer.hpp` (add the `WavWriter` member usage; no signature change)
- Modify: `iso-recorder/src/iso-session.cpp` (choose `.wav` for audio entries)

**Interfaces:**
- Consumes: `iso::WavWriter` (Task 3), `iso::Kind`, `iso::extensionFor` (Task 2).
- Produces: `SourceRecorder` now starts audio-only recorders and muxes audio for video ones.

- [ ] **Step 1: Fill in the callback and the start/stop for audio**

In `source-recorder.hpp`, add the members the callback needs:

```cpp
	bool isComposite() const
	{
		return hasVideo_ && (obs_source_get_output_flags(source_) & OBS_SOURCE_COMPOSITE);
	}
	WavWriter wav_;
	std::string pathForKind;
```

In `source-recorder.cpp`, include `"wav-writer.hpp"` and replace the stubs:

```cpp
void SourceRecorder::onPcm(void *param, size_t, struct audio_data *data)
{
	auto *self = static_cast<SourceRecorder *>(param);
	self->wav_.write(reinterpret_cast<const float *const *>(data->data), data->frames);
}

bool SourceRecorder::onAudioInput(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs,
				  uint32_t activeMixers, struct audio_output_data *mixes)
{
	auto *self = static_cast<SourceRecorder *>(param);
	self->fillAudio(startTs, endTs, newTs, activeMixers, mixes);
	return true;
}

void SourceRecorder::mixPlain(struct audio_output_data *mixes, size_t mixIdx)
{
	struct obs_source_audio_mix audio {};
	obs_source_get_audio_mix(source_, &audio);
	for (size_t ch = 0; ch < audio_output_get_channels(obs_get_audio()); ++ch) {
		float *out = mixes[mixIdx].data[ch];
		const float *in = audio.output[0].data[ch];
		for (size_t f = 0; f < AUDIO_OUTPUT_FRAMES; ++f)
			out[f] = std::fmax(-1.0f, std::fmin(1.0f, out[f] + in[f]));
	}
}

struct MixCtx {
	audio_t *audio;
	struct obs_source_audio_mix *mixed;
	size_t mixIdx;
};

static bool mixChild(void *param, obs_source_t *child);

void SourceRecorder::mixComposite(struct audio_output_data *mixes, size_t mixIdx)
{
	struct obs_source_audio_mix mixed {};
	for (size_t ch = 0; ch < audio_output_get_channels(obs_get_audio()); ++ch)
		mixed.output[0].data[ch] = mixes[mixIdx].data[ch];
	MixCtx ctx {audio_, &mixed, mixIdx};
	obs_source_enum_active_tree(source_, mixChild, &ctx);
}

static bool mixChild(void *param, obs_source_t *child)
{
	auto *ctx = static_cast<MixCtx *>(param);
	struct obs_source_audio_mix childMix {};
	obs_source_get_audio_mix(child, &childMix);
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	for (size_t ch = 0; ch < audio_output_get_channels(obs_get_audio()); ++ch) {
		const int64_t off = (int64_t)ns_to_audio_frames(
			audio_output_get_sample_rate(ctx->audio), ts);
		const float *in = childMix.output[0].data[ch];
		float *out = ctx->mixed->output[0].data[ch];
		for (size_t f = 0; f < AUDIO_OUTPUT_FRAMES; ++f) {
			const int64_t dst = (int64_t)f + off;
			if (dst < 0 || dst >= (int64_t)AUDIO_OUTPUT_FRAMES)
				continue;
			out[dst] = std::fmax(-1.0f, std::fmin(1.0f, out[dst] + in[f]));
		}
	}
	return true;
}

void SourceRecorder::fillAudio(uint64_t, uint64_t, uint64_t *newTs, uint32_t activeMixers,
			       struct audio_output_data *mixes)
{
	if (obs_source_audio_pending(source_))
		return;
	*newTs = obs_source_get_audio_timestamp(source_);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
		if (!(activeMixers & (1u << mix)))
			continue;
		if (isComposite())
			mixComposite(mixes, mix);
		else
			mixPlain(mixes, mix);
	}
}
```

`obs_source_get_audio_timestamp` is not in the verified list; use
`obs_source_get_audio_mix`'s timestamp via the source's audio mix — the verified Source Record
callback uses `obs_source_get_audio_timestamp`. Confirm it exists in `obs.h`; if not, use the
`startTs` passed into `onAudioInput` as `*newTs`. (Source Record's own build uses
`obs_source_get_audio_timestamp`.)

- [ ] **Step 2: Start an audio-only recorder**

In `source-recorder.cpp`, replace the `if (!hasVideo_)` guard at the top of `start()` with the
audio-only branch:

```cpp
	if (!hasVideo_) {
		struct audio_output_info oi {};
		oi.name = "iso-recorder";
		oi.speakers = SPEAKERS_STEREO;
		oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = this;
		oi.input_callback = onAudioInput;
		if (audio_output_open(&audio_, &oi) != AUDIO_OUTPUT_SUCCESS) {
			*error = "the audio device did not open";
			return false;
		}
		if (!wav_.open(path_, oi.samples_per_sec, 2, 24)) {
			*error = "could not write the WAV file";
			return false;
		}
		audio_output_connect(audio_, 0, nullptr, onPcm, this);
		codec_ = "pcm_s24le";
		started_ = true;
		return true;
	}
```

And add the audio-encoder branch to the video path, after `obs_encoder_set_video` and before the
output is created:

```cpp
	if (hasAudio_) {
		struct audio_output_info oi {};
		oi.name = "iso-recorder";
		oi.speakers = SPEAKERS_STEREO;
		oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = this;
		oi.input_callback = onAudioInput;
		if (audio_output_open(&audio_, &oi) == AUDIO_OUTPUT_SUCCESS) {
			obs_data_t *as = obs_data_create();
			audioEnc_ = obs_audio_encoder_create("ffmpeg_aac", name_.c_str(), as, 0,
							     nullptr);
			obs_data_release(as);
			if (audioEnc_) {
				obs_encoder_set_audio(audioEnc_, audio_);
				obs_output_set_audio_encoder(output_, audioEnc_, 0);
			}
		}
	}
```

- [ ] **Step 3: Close the audio in `stop()`**

Add before releasing the output (so the encoder consumes the last frames), and after the output
release for the audio-only path:

```cpp
	if (audio_) {
		audio_output_disconnect(audio_, 0, onPcm, this);
		audio_output_close(audio_);
		audio_ = nullptr;
	}
	wav_.close();
```

- [ ] **Step 4: Build and test both audio paths by hand**

```bash
iso-recorder/build-mac.sh && cp -R iso-recorder/build-mac/iso-recorder.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS. Record a video source (a `.mov` comes out) and an audio-only source (a `.wav` comes
out). Expected: the `.mov` has sound when played back; the `.wav` opens in QuickTime and in
Audacity, is 48 kHz / 24-bit / stereo, and contains that source's sound.

- [ ] **Step 5: Commit**

```bash
git add iso-recorder/src
git commit -m "Record audio-only sources as WAV and mux video-source audio into the MOV"
```

---

## Task 6: The session — epoch, live arming, offsets, manifest

Now the session becomes the spec's session: one epoch, one queued task per arm event, real
filenames and numbering, start offsets, a `session.json` written at start and rewritten at stop,
and a re-arm that produces a second file. This is also where the spec's drift measurement runs.

**Files:**
- Modify: `iso-recorder/src/iso-session.hpp`
- Modify: `iso-recorder/src/iso-session.cpp`
- Modify: `iso-recorder/src/source-recorder.cpp` (record the real start offset)

**Interfaces:**
- Consumes: `iso::makeFilename`, `iso::buildSessionJson`, `iso::buildReadme` (Task 2).
- Produces: `IsoSession` with `arm`/`disarm` live, `recordings()` returning the finished list;
  `SourceRecorder::path()` now the real `NN_<source>.<ext>`.

- [ ] **Step 1: Give the recorder its real filename and offset**

`SourceRecorder` gains a `static` factory decision in `IsoSession::startEntry`, which now computes
the filename through `session-writer` and passes it as `path_`. When the recorder starts, set its
offset from the epoch in the same queued call:

```cpp
void IsoSession::startEntry(Entry &e)
{
	const std::string file = makeFilename(e.kind, e.index, e.name, e.segment);
	const std::string path = folder_ + "/" + file;
	e.recorder = std::make_unique<SourceRecorder>(
		e.source, e.name, path, videoEncoderId_, videoSettings_, e.kind, audioCodec_);
	std::string err;
	if (!e.recorder->start(&err)) {
		e.recorder.reset();
		finished_.push_back({e.name, e.kind, file, "", std::nullopt, std::nullopt,
				     Status::Failed, err, e.label});
		return;
	}
	e.recorder->noteStart(epochNs_);
}
```

`noteStart` already stores `startOffsetNs_ = startNs_ - epochNs_` (written in Task 4).

The time, IO and label helpers the session leans on (add near the top of `iso-session.cpp`, after the
`#include`s; `#include <cstdio>` joins `<ctime>`; `localTime` is the portable one written in Task 4):

```cpp
static std::string isoNow()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[32];
	std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &tm);
	return b;
}

static std::string humanDate()
{
	std::time_t t = std::time(nullptr);
	std::tm tm = localTime(t);
	char b[64];
	std::strftime(b, sizeof b, "%d %B %Y", &tm);
	return b;
}

static std::string platformName()
{
#ifdef __APPLE__
	return "macos";
#else
	return "windows";
#endif
}

static void writeFile(const std::string &path, const std::string &text)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) {
		blog(LOG_WARNING, "[iso-recorder] could not write %s", path.c_str());
		return;
	}
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
}

static std::string labelFor(const std::string &name, Kind kind)
{
	if (name == "Mic")
		return "microphone";
	if (name == "Discord")
		return "Discord call";
	return kind == Kind::Video ? name + ", on its own" : name;
}
```

`labelFor` is where the README's human words come from; anything unmapped falls back to the source
name, matching `session-writer`'s own fallback.

- [ ] **Step 2: Live arm and disarm**

`arm()` gains the re-arm rule: each source keeps a running segment count per kind, so the first
arm is segment 1 and every later arm of the same source is segment 2, 3, … — which is what makes
`makeFilename` produce `_2`. Add `#include <map>` and `#include <utility>` to the header, plus the
new members:

```cpp
	std::map<std::pair<obs_source_t *, Kind>, int> segmentCount_;
	std::map<Kind, int> nextIndex_{{Kind::Video, 1}, {Kind::Audio, 1}};
	std::string videoEncoderId_ = "obs_x264";
	obs_data_t *videoSettings_ = nullptr; // borrowed from SessionConfig for the session's life
	std::string audioCodec_ = "pcm_s24le";
	SessionInfo info_;
```

```cpp
bool IsoSession::arm(obs_source_t *source, std::string *error)
{
	if (findLive(source))
		return true; // already armed
	Entry e;
	e.source = obs_source_get_ref(source);
	e.name = obs_source_get_name(source);
	e.kind = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) ? Kind::Video
									: Kind::Audio;
	e.segment = ++segmentCount_[std::make_pair(source, e.kind)];
	e.index = nextIndex_[e.kind]++;
	e.label = labelFor(e.name, e.kind);
	entries_.push_back(std::move(e));
	if (active_)
		startEntry(entries_.back());
	rewriteManifest();
	(void)error;
	return true;
}
```

`findLive()` is `find()` restricted to entries that are still armed; `find()` is kept for the
`source_remove` path. `labelFor()` is a small map in `iso-session.cpp` giving the README its human
words (`Mic` → `microphone`, `Discord` → `Discord call`, a video source → `<name>, on its own`);
anything unmapped falls back to the source name, as `session-writer` does.

`disarm()` stops and finalizes the recorder, appends a completed `Recording` to `finished_`, and
releases the source:

```cpp
void IsoSession::disarm(obs_source_t *source)
{
	Entry *e = findLive(source);
	if (!e)
		return;
	if (e->recorder) {
		e->recorder->stop();
		finished_.push_back({e->name, e->kind, makeFilename(e->kind, e->index, e->name, e->segment),
				     e->recorder->codec(), e->recorder->startOffsetNs() / 1e9,
				     e->recorder->durationSeconds(), Status::Aborted, "", e->label});
	}
	obs_source_release(e->source);
	entries_.erase(entries_.begin() + (e - entries_.data()));
	rewriteManifest();
}
```

Add the maps to the header: `std::map<std::pair<obs_source_t *, Kind>, int> segmentCount_;`,
`std::map<Kind, int> nextIndex_{{Kind::Video, 1}, {Kind::Audio, 1}};`, and the config fields
`videoEncoderId_`, `videoSettings_`, `audioCodec_` stored at `start()`.

- [ ] **Step 3: The epoch, the folder and the manifest**

`start()` builds the timestamped folder, stamps the epoch, writes the first manifest with every
entry `recording`, and starts every armed recorder. It runs on the OBS UI thread: both callers —
the frontend event callback and the dock's Qt slot — are already on it, so no `obs_queue_task` is
needed here. A recorder armed *later*, from a Qt slot, is likewise on the UI thread. The queue is
only needed when the caller is not (for example a hotkey bound on another thread); Task 8 routes
those through `obs_queue_task(OBS_TASK_UI, …)`.

```cpp
bool IsoSession::start(const SessionConfig &cfg, std::string *error)
{
	folder_ = cfg.basePath + "/" + nowStamp();
	if (os_mkdirs(folder_.c_str()) != 0 && !os_file_exists(folder_.c_str())) {
		*error = "could not create the session folder";
		return false;
	}
	videoEncoderId_ = cfg.videoEncoderId;
	videoSettings_ = cfg.videoSettings;
	audioCodec_ = cfg.audioCodec;
	active_ = true;
	epochNs_ = os_gettime_ns();
	info_ = {};
	info_.started = isoNow();
	info_.dateLine = humanDate();
	info_.obsVersion = obs_get_version_string();
	info_.platform = platformName();
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		info_.video.width = (int)ovi.output_width;
		info_.video.height = (int)ovi.output_height;
		info_.video.fps = (double)ovi.fps_num / (double)ovi.fps_den;
	}
	rewriteManifest();
	for (auto &e : entries_)
		startEntry(e); // appends a failed entry if one does not start
	rewriteManifest();
	return true;
}
```

`rewriteManifest()` gathers `recording` entries plus `finished_` and writes both files:

```cpp
void IsoSession::rewriteManifest()
{
	std::vector<Recording> all = finished_;
	for (auto &e : entries_) {
		Recording r;
		r.source = e.name;
		r.kind = e.kind;
		r.label = e.label;
		if (e.recorder) {
			r.file = makeFilename(e.kind, e.index, e.name, e.segment);
			r.codec = e.recorder->codec();
			r.startOffset = e.recorder->startOffsetNs() / 1e9;
			r.status = Status::Recording;
		} else {
			continue; // failed entries were appended to finished_ already
		}
		all.push_back(r);
	}
	writeFile(folder_ + "/session.json", buildSessionJson(info_, all));
	writeFile(folder_ + "/README.txt", buildReadme(info_, all));
}
```

`stop()` stops every recorder, moves each live entry to `finished_` with `Status::Complete` and its
duration, stamps `info_.ended` and `info_.duration`, then rewrites the manifest.

- [ ] **Step 4: Build and verify the session by hand**

```bash
iso-recorder/build-mac.sh && cp -R iso-recorder/build-mac/iso-recorder.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS. Arm two sources, record 10 s, stop. Expected: a timestamped folder under
`~/Movies/Mist ISO/`, files `01_<source>.mov` and `01_<source>.wav` (numbered per category),
`session.json` listing both with `startOffset` near `0.0` and `status: complete`, and a
`README.txt` that lists each file with `0:00.0`. Then arm one source, record 5 s, disarm it, re-arm
it, record 5 s more, stop. Expected: `01_<source>.mov` and `01_<source>_2.mov`, two manifest
entries, the second with a non-zero `startOffset`.

- [ ] **Step 5: Run the measurements the spec asks for and write them down**

Record one video source plus one audio source for **three hours** (a real one, not a stub). In
Resolve, check that a clap at the top stays lined up at the end. Then, with the stream running,
arm video sources one at a time until frames drop, on both the Mac and the Windows box. Write the
results into `README.md` under `## Measurements` (drift per hour; the number of
1080p60 encodes each machine sustains). **If audio drifts against video, stop and redesign the
clock before Task 7** — the fix changes `SourceRecorder`, not the dock. If it does not drift,
proceed.

- [ ] **Step 6: Commit**

```bash
git add iso-recorder/src README.md
git commit -m "Make the session real: epoch, live arming, offsets, two files per re-arm, manifest"
```

## Task 7: The dock

The dock grows from "a list and a button" into the spec's control surface: sources split into
**Picture** and **Sound**, a live list, a checkbox that is an arm, session settings, and per-source
status. It holds no recorder state — it asks `IsoSession`.

**Files:**
- Modify: `iso-recorder/src/iso-dock.hpp`
- Modify: `iso-recorder/src/iso-dock.cpp`
- Modify: `iso-recorder/src/iso-session.hpp` / `.cpp` (add `isArmed`, `statusFor`, `setConfig`)
- Modify: `iso-recorder/data/locale/en-US.ini`

**Interfaces:**
- Consumes: `IsoSession::arm`, `disarm`, `start`, `stop`, `recordings`, `active`, `folder`;
  `iso::extensionFor`, `iso::formatClock`.
- Produces: `IsoDock` with `refreshSources()`, `setStatus(const std::string &)`,
  `configFromWidgets()` returning a `SessionConfig`.

- [ ] **Step 1: Extend `IsoSession` for the dock**

Add to the header and implement:

```cpp
	bool isArmed(obs_source_t *source) const;
	bool isArmedAny() const;                           // true when any entry is still armed
	size_t armedVisualCount() const;                   // armed entries of Kind::Video
	std::string statusFor(obs_source_t *source) const; // filename, or "recording", or the error
	void setConfig(const SessionConfig &cfg);          // remembered while no session runs
	const SessionConfig &config() const { return config_; }
```

`statusFor` returns the in-flight filename from the live entry, `""` when not armed, and the
`failed_` entry's `error` when one exists for that source.

- [ ] **Step 2: Write the dock**

`iso-dock.hpp` gains the widgets and the arm path:

```cpp
#pragma once

#include <QWidget>

#include "iso-session.hpp"

class QListWidget;
class QListWidgetItem;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;

namespace iso {

class IsoDock : public QWidget {
	Q_OBJECT
public:
	explicit IsoDock(IsoSession *session, QWidget *parent = nullptr);
	void refreshSources();
	void setStatus(const QString &text);

private slots:
	void onRecordClicked();
	void onItemChanged(QListWidgetItem *item);
	void onBrowse();
	void onTick();

private:
	void addSource(QListWidget *list, obs_source_t *source, Kind kind);
	SessionConfig configFromWidgets() const;

	IsoSession *session_;
	QListWidget *picture_ = nullptr;
	QListWidget *sound_ = nullptr;
	QLineEdit *path_ = nullptr;
	QComboBox *encoder_ = nullptr;
	QComboBox *audio_ = nullptr;
	QCheckBox *withStream_ = nullptr;
	QCheckBox *recordScene_ = nullptr;
	QPushButton *record_ = nullptr;
	QLabel *status_ = nullptr;
	QTimer *timer_ = nullptr;
	bool updating_ = false;
};

} // namespace iso
```

`iso-dock.cpp` — the working implementation:

```cpp
#include "iso-dock.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace iso {

static QString defaultBasePath()
{
#ifdef __APPLE__
	return QDir::homePath() + "/Movies/Mist ISO";
#else
	return "E:/Stream/Mist/ISO";
#endif
}

static void fillEncoders(QComboBox *box)
{
	box->addItem("Same as the stream", "");
#ifdef __APPLE__
	box->addItem("Apple (VideoToolbox)", "apple_h264");
#else
	box->addItem("NVIDIA (NVENC)", "jim_nvenc");
	box->addItem("Intel (Quick Sync)", "obs_qsv11_v2");
	box->addItem("AMD (AMF)", "h264_texture_amf");
#endif
	box->addItem("CPU (x264)", "obs_x264");
}

IsoDock::IsoDock(IsoSession *session, QWidget *parent) : QWidget(parent), session_(session)
{
	auto *layout = new QVBoxLayout(this);
	auto *pictureLabel = new QLabel(tr("Picture"), this);
	picture_ = new QListWidget(this);
	auto *soundLabel = new QLabel(tr("Sound"), this);
	sound_ = new QListWidget(this);

	auto *pathRow = new QHBoxLayout();
	path_ = new QLineEdit(defaultBasePath(), this);
	auto *browse = new QPushButton(tr("Browse…"), this);
	pathRow->addWidget(new QLabel(tr("Save to"), this));
	pathRow->addWidget(path_);
	pathRow->addWidget(browse);

	encoder_ = new QComboBox(this);
	fillEncoders(encoder_);
	audio_ = new QComboBox(this);
	audio_->addItem("WAV 24-bit", "pcm_s24le");
	withStream_ = new QCheckBox(tr("Start and stop with the stream"), this);
	withStream_->setChecked(true);
	recordScene_ = new QCheckBox(tr("Also record the live scene"), this);

	record_ = new QPushButton(tr("Record"), this);
	status_ = new QLabel(tr("Pick the sources to keep."), this);

	layout->addWidget(pictureLabel);
	layout->addWidget(picture_);
	layout->addWidget(soundLabel);
	layout->addWidget(sound_);
	layout->addLayout(pathRow);
	layout->addWidget(encoder_);
	layout->addWidget(audio_);
	layout->addWidget(withStream_);
	layout->addWidget(recordScene_);
	layout->addWidget(record_);
	layout->addWidget(status_);

	connect(record_, &QPushButton::clicked, this, &IsoDock::onRecordClicked);
	connect(picture_, &QListWidget::itemChanged, this, &IsoDock::onItemChanged);
	connect(sound_, &QListWidget::itemChanged, this, &IsoDock::onItemChanged);
	connect(browse, &QPushButton::clicked, this, &IsoDock::onBrowse);
	connect(recordScene_, &QCheckBox::toggled, this,
		[this](bool on) { session_->setConfig(configFromWidgets()); (void)on; });

	timer_ = new QTimer(this);
	timer_->setInterval(2000);
	connect(timer_, &QTimer::timeout, this, &IsoDock::onTick);
	timer_->start();
	refreshSources();
}

void IsoDock::addSource(QListWidget *list, obs_source_t *source, Kind kind)
{
	const uint32_t flags = obs_source_get_output_flags(source);
	auto *item = new QListWidgetItem(obs_source_get_name(source), list);
	item->setData(Qt::UserRole, QVariant::fromValue<void *>(source));
	item->setData(Qt::UserRole + 1, kind == Kind::Video ? 0 : 1);
	item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
	item->setCheckState(session_->isArmed(source) ? Qt::Checked : Qt::Unchecked);
	if (kind == Kind::Video) {
		item->setToolTip(QString::number(obs_source_get_width(source)) + "×" +
				 QString::number(obs_source_get_height(source)));
	}
	(void)flags;
}

void IsoDock::refreshSources()
{
	updating_ = true;
	picture_->clear();
	sound_->clear();
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *self = static_cast<IsoDock *>(param);
			const uint32_t flags = obs_source_get_output_flags(source);
			if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
				return true;
			if (flags & OBS_SOURCE_VIDEO)
				self->addSource(self->picture_, source, Kind::Video);
			else
				self->addSource(self->sound_, source, Kind::Audio);
			return true;
		},
		this);
	updating_ = false;
	const bool any = session_->isArmedAny();
	record_->setEnabled(session_->active() || any);
	record_->setText(session_->active() ? tr("Stop") : tr("Record"));
}

void IsoDock::onItemChanged(QListWidgetItem *item)
{
	if (updating_)
		return;
	auto *source = static_cast<obs_source_t *>(item->data(Qt::UserRole).value<void *>());
	if (!source)
		return;
	if (item->checkState() == Qt::Checked) {
		std::string err;
		if (!session_->arm(source, &err))
			setStatus(QString::fromStdString(err));
	} else {
		session_->disarm(source);
	}
	refreshSources();
}

void IsoDock::onBrowse()
{
	const QString dir = QFileDialog::getExistingDirectory(this, tr("Save recordings in"),
							      path_->text());
	if (!dir.isEmpty())
		path_->setText(dir);
}

void IsoDock::onRecordClicked()
{
	if (session_->active()) {
		session_->stop();
		setStatus(tr("Stopped."));
	} else {
		SessionConfig cfg = configFromWidgets();
		std::string err;
		if (!session_->start(cfg, &err))
			setStatus(QString::fromStdString(err));
		else
			setStatus(tr("Recording."));
	}
	refreshSources();
}

void IsoDock::onTick()
{
	if (session_->active() && session_->isArmedAny())
		refreshSources();
}

void IsoDock::setStatus(const QString &text) { status_->setText(text); }

SessionConfig IsoDock::configFromWidgets() const
{
	SessionConfig cfg;
	cfg.basePath = path_->text().toStdString();
	cfg.videoEncoderId = encoder_->currentData().toString().toStdString();
	cfg.audioCodec = audio_->currentData().toString().toStdString();
	cfg.withStream = withStream_->isChecked();
	cfg.recordComposite = recordScene_->isChecked();
	return cfg;
}

} // namespace iso
```

`IsoSession` needs `isArmedAny()`: true when any entry is still armed.

- [ ] **Step 3: Persist the settings**

`iso-recorder/src/iso-settings.hpp` / `.cpp` wrap the profile config so the dock remembers the
base path, encoder, audio format and both toggles between runs. It reads and writes a section
named `iso-recorder` via `obs_frontend_get_profile_config()`, and `obs_frontend_save()` is called
on change (debounced by the same 2s timer, not every keystroke).

```cpp
namespace iso {
void loadSettings(IsoDock *dock);
void saveSettings(IsoDock *dock);
}
```

- [ ] **Step 4: Build and check the dock by hand**

```bash
iso-recorder/build-mac.sh && cp -R iso-recorder/build-mac/iso-recorder.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS. Expected: the dock shows video sources under **Picture** and audio-only sources under
**Sound**; ticking a source while recording starts its file and unticking ends it; the file shown
per source matches what appears in the folder; a source added to the collection appears within two
seconds and can be armed; the settings survive an OBS restart.

- [ ] **Step 5: Commit**

```bash
git add iso-recorder/src iso-recorder/data
git commit -m "Give the dock a live source list, arms and session settings"
```

**Deferred from the spec's format list:** the audio format control offers WAV only. FLAC and AAC
were listed as alternatives, but the audio-only path writes WAV directly (no OBS output accepts
audio-only), and offering the others needs its own FFmpeg muxer. That is v2, alongside the
timeline export — noted in the spec's Further Notes when this plan is finished.

---

## Task 8: Wire the plugin lifecycle

Frontend events, the source-remove signal, the program-scene recorder, and config save.

**Files:**
- Modify: `iso-recorder/src/plugin-main.cpp`
- Modify: `iso-recorder/src/iso-session.hpp` / `.cpp` (composite entry, `onSceneChanged`)
- Modify: `iso-recorder/src/source-recorder.hpp` / `.cpp` (`setSource` for the composite)

**Interfaces:**
- Consumes: everything from Tasks 4–7.
- Produces: `IsoSession::onSceneChanged(obs_source_t *scene)`; `SourceRecorder::setSource`.

- [ ] **Step 1: Register events and signals**

```cpp
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QWidget>

#include "iso-dock.hpp"
#include "iso-session.hpp"
#include "iso-settings.hpp"

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "ISO Recorder — one file per source while streaming";
}

static iso::IsoSession *g_session = nullptr;
static iso::IsoDock *g_dock = nullptr;

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (g_session->config().withStream && g_session->isArmedAny() &&
		    !g_session->active()) {
			std::string err;
			if (!g_session->start(g_session->config(), &err))
				g_dock->setStatus(QString::fromStdString(err));
		}
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (g_session->active())
			g_session->stop();
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		g_session->onSceneChanged(obs_frontend_get_current_scene());
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_dock->refreshSources();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_session->active())
			g_session->stop();
		iso::saveSettings(g_dock);
		break;
	default:
		break;
	}
}

static void onSourceRemoveSignal(void *, calldata_t *calldata)
{
	auto *source = static_cast<obs_source_t *>(calldata_get_ptr(calldata, "source"));
	if (source)
		g_session->onSourceRemoved(source);
}

bool obs_module_load(void)
{
	g_session = new iso::IsoSession();
	g_dock = new iso::IsoDock(g_session);
	iso::loadSettings(g_dock);
	obs_frontend_add_dock_by_id("iso-recorder-dock", "ISO Recorder", g_dock);
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_remove", onSourceRemoveSignal,
			       nullptr);
	blog(LOG_INFO, "[iso-recorder] loaded");
	return true;
}

void obs_module_unload(void)
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_remove", onSourceRemoveSignal,
				  nullptr);
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_frontend_remove_dock("iso-recorder-dock");
	iso::saveSettings(g_dock);
	delete g_dock;
	delete g_session;
	g_dock = nullptr;
	g_session = nullptr;
	blog(LOG_INFO, "[iso-recorder] unloaded");
}
```

- [ ] **Step 2: The composite recorder**

Add `Entry::isComposite` and an `armComposite()` the dock calls when "Also record the live scene"
is ticked. Its recorder follows the program scene: `SourceRecorder::setSource(obs_source_t *)`
calls `obs_view_set_source(view_, 0, newScene)` and swaps the `inc_showing`/`dec_showing` pair. It
is always `00_Session.mov`, so its index is 0 and its name is `Session`. `onSceneChanged` calls
`setSource` on the composite recorder when one is live.

```cpp
void SourceRecorder::setSource(obs_source_t *next)
{
	if (!view_ || !next || next == source_)
		return;
	obs_source_dec_showing(source_);
	obs_source_release(source_);
	source_ = obs_source_get_ref(next);
	obs_view_set_source(view_, 0, source_);
	obs_source_inc_showing(source_);
}
```

- [ ] **Step 3: Build and check the lifecycle by hand**

```bash
iso-recorder/build-mac.sh && cp -R iso-recorder/build-mac/iso-recorder.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS. Expected: with "Start and stop with the stream" on and at least one source armed,
going live starts every armed file; ending the stream finalizes them and rewrites the manifest;
with nothing armed the stream starts no session; switching scenes mid-record keeps every armed
source writing and, with "Also record the live scene" on, `00_Session.mov` follows the scene;
deleting a source mid-record stops only its recorder and marks it `aborted`; quitting OBS while
recording stops cleanly and leaves a complete manifest.

- [ ] **Step 4: Commit**

```bash
git add iso-recorder/src
git commit -m "Drive sessions from OBS events, follow the scene for the composite, persist settings"
```

---

## Task 9: Failure handling

**Files:**
- Modify: `iso-recorder/src/iso-session.cpp` (preflight, per-source isolation already present)
- Modify: `iso-recorder/src/source-recorder.cpp` (output stop signal → failure)
- Modify: `iso-recorder/src/iso-dock.cpp` (the cap warning and the disk message)

**Interfaces:**
- Produces: `IsoSession::preflight(const SessionConfig &, std::string *why)`;
  `SourceRecorder::failed()`/`error()` populated from the output's own `stop` signal.

- [ ] **Step 1: Catch an output that stops on its own**

In `SourceRecorder::start`, after a successful `obs_output_start`, connect its stop signal:

```cpp
static void onOutputStopped(void *param, calldata_t *)
{
	auto *self = static_cast<SourceRecorder *>(param);
	if (self->started_) { // stopped without us asking
		self->failed_ = true;
		self->error_ = obs_output_get_last_error(self->output_);
		if (self->error_.empty())
			self->error_ = "the recorder stopped early";
		self->started_ = false;
	}
}

	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", onOutputStopped, this);
```

`IsoSession::rewriteManifest` reads `failed()` and records the entry as `failed` with the reason
instead of `recording`.

- [ ] **Step 2: Preflight the folder and the disk**

```cpp
static uint64_t freeBytes(const std::string &path);
static uint64_t requiredBytes(size_t armedVisuals, uint64_t bitrateBitsPerSec);

bool IsoSession::preflight(const SessionConfig &cfg, std::string *why)
{
	if (cfg.basePath.empty()) {
		*why = "Choose a folder to save in.";
		return false;
	}
	if (os_mkdirs(cfg.basePath.c_str()) != 0 && !os_file_exists(cfg.basePath.c_str())) {
		*why = "That folder cannot be created.";
		return false;
	}
	const uint64_t need = requiredBytes(armedVisualCount(), cfg.bitrateBitsPerSec);
	const uint64_t free = freeBytes(cfg.basePath);
	if (free < need) {
		*why = "Not enough space: the selection needs about " +
		       humanBytes(need) + " and only " + humanBytes(free) + " is free.";
		return false;
	}
	return true;
}
```

The three helpers are small; add them next to `preflight` (`os_get_free_space` and `<cstdio>`'s
`snprintf` are already available to the plugin):

```cpp
static uint64_t freeBytes(const std::string &path)
{
	const int64_t free = os_get_free_space(path.c_str());
	return free > 0 ? (uint64_t)free : 0;
}

static uint64_t requiredBytes(size_t armedVisuals, uint64_t bitrateBitsPerSec)
{
	const uint64_t oneHour = bitrateBitsPerSec / 8ull * 3600ull;
	return (uint64_t)armedVisuals * oneHour * 11ull / 10ull; // + 10% headroom
}

static std::string humanBytes(uint64_t bytes)
{
	static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	double v = (double)bytes;
	size_t u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		++u;
	}
	char b[32];
	snprintf(b, sizeof b, "%.1f %s", v, units[u]);
	return b;
}
```

`requiredBytes` is one hour of encode per armed visual at the chosen bitrate plus 10% headroom (the
audio path is negligible next to video). The dock calls `preflight` before `start` and shows `why`
when it refuses.

- [ ] **Step 3: The encoder-cap warning**

`constexpr size_t kEncoderWarnThreshold = 3;` (replace with the measured number from Task 6 once
it exists). In the dock's `onItemChanged`, arm first, then if `session_->armedVisualCount() >
kEncoderWarnThreshold`, show a `QMessageBox::warning` with Yes/No — "That is more video recordings
than this machine is known to keep up with. Record anyway?" — and disarm on No. The check also
runs before a stream-driven start, where it is a status line rather than a modal.

- [ ] **Step 4: Build and test the failure paths by hand**

```bash
iso-recorder/build-mac.sh && cp -R iso-recorder/build-mac/iso-recorder.plugin \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Expected, one at a time: a folder that does not exist and cannot be created refuses the start with
a readable reason; a full disk (fill a small volume) refuses or stops with a readable reason; an
encoder that will not open marks only that source `failed` and the others keep recording; deleting
a source mid-record marks it `aborted` and leaves the rest; a forced quit leaves playable `.mov`
files and a manifest that still says `recording`.

- [ ] **Step 5: Commit**

```bash
git add iso-recorder/src
git commit -m "Refuse bad starts, isolate a dead source, warn before the encoder ceiling"
```

---

## Task 10: Packaging and handoff

**Files:**
- Modify: `README.md`
- Create: `docs/adr/0001-iso-recording-is-pre-composite-in-process-capture.md`
- Modify: `CONTEXT.md`
- Modify: `iso-recorder/build-mac.sh` / `build-win.ps1` (install step, SDK path override)
- Modify: `iso-recorder/data/locale/en-US.ini`

**Interfaces:**
- Produces: the finished `README.md` carrying the spec's 15-item manual checklist
  under `## Manual checklist`, and the measurements under `## Measurements`.

- [ ] **Step 1: Finish the README**

It carries: what the plugin is in two sentences; build and install per platform, including the
`OBS_SRC` override ("headers come from an `obs-studio` checkout; the build links the OBS you
stream with"); how to use the dock; the **15-item manual checklist** copied from the spec's Testing
Decisions; and `## Measurements` with the numbers from Task 6 and the warnings they set.

- [ ] **Step 2: Add the install to the build scripts**

`build-mac.sh` ends by copying `iso-recorder.plugin` into
`~/Library/Application Support/obs-studio/plugins/` when passed `--install`. `build-win.ps1`
copies `iso-recorder.dll` and `data/` into
`%ProgramData%\obs-studio\plugins\iso-recorder\{bin\64bit,data}` when passed `-Install`.

- [ ] **Step 3: Write the ADR**

`docs/adr/0001-iso-recording-is-pre-composite-in-process-capture.md`, matching the format of the
existing six: records that capture happens pre-composite, in-process, per source, rather than
splitting a composited recording after the fact; that the recorder is dock-owned rather than a
filter, because a filter cannot attach to an audio-only source; and the consequences (N encodes
for N visual sources; audio-only files written by us).

- [ ] **Step 4: Add ISO to the glossary**

`CONTEXT.md` gains a short section making clear this is **not** Toast vocabulary but OBS capture
vocabulary, with one term:

```markdown
## OBS capture

This is a different tool from the pipeline above, and its words do not cross over.

**ISO** — one isolated recording of one source: its own picture, its own audio, its own file.
```

- [ ] **Step 5: Confirm the build is clean and the tests pass on both platforms**

```bash
iso-recorder/build-mac.sh
ctest --test-dir iso-recorder/build-mac --output-on-failure
```

Expected: the plugin builds with no warnings about the code this plan wrote, and both tests pass.
On Windows, run `build-win.ps1` and confirm the DLL loads.

- [ ] **Step 6: Commit**

```bash
git add docs/adr/0001-iso-recording-is-pre-composite-in-process-capture.md
git commit -m "Package the ISO Recorder: install, checklist, measurements, ADR"
```

---

## Done means

| Spec section | Task |
|---|---|
| Where the code lives; build and install | 1, 10 |
| Why a dock and not a filter | 8 (dock registration, band the `OBS_SOURCE_COMPOSITE` path) |
| The one seam | 2 |
| The recorder per source — video | 4 |
| The recorder per source — audio, composite sources | 5 |
| The recorder per source — encoders and output | 4, 5 |
| Containers and formats | 4, 5, 7 (WAV only; FLAC/AAC deferred) |
| Sync — epoch and offsets | 6 |
| Output layout and naming; re-arm suffix | 6 |
| `session.json`; `README.txt` | 2, 6 |
| The dock | 7 |
| Triggers and lifecycle | 8 |
| Failure handling | 9 |
| Encoder choice and the cost ceiling | 7, 9 |
| Automated tests | 2, 3 |
| Manual checklist (15 items) | 10, run throughout |
| Out of scope (timeline export; FLAC/AAC alternatives) | not built; recorded here and in the spec |
| ADR; glossary | 10 |

**Two deviations from the spec text, both deliberate and to be reconciled in it when this plan is
accepted:**

1. **Audio-only files are written by us, not by `ffmpeg_muxer`.** No OBS output accepts an
   audio-only stream (verified in `obs-output.c`), so a WAV is written by our own `WavWriter` fed
   by the private `audio_output`. The spec's "`ffmpeg_muxer` + `ffmpeg_pcm_s24le`" line is
   replaced.
2. **FLAC and AAC are deferred.** They need an FFmpeg muxer for the audio-only path. WAV is the
   default and the editor's choice; the alternatives wait for v2 with the timeline export.


