# ISO Recorder

An OBS Studio plugin that records one file per source while you stream. Each armed
source is captured on its own — video sources to a per-source file, audio-only
sources to a WAV — independent of the composite program output.

The design record lives in `docs/`: the spec, the task-by-task build plan, and the
ADR for the one decision that is hard to reverse (capture happens pre-composite, in
process, per source, and a dock owns it rather than a filter).

## Build

Headers come from an `obs-studio` source checkout (`OBS_SRC` on macOS, `-ObsSrc` on
Windows); the build links the OBS app you stream with, which ships no headers. The
checkout tag must match the installed OBS (32.2.2 on this machine). `build-mac.sh`
also fetches the header-only SIMDe library into `deps/simde`, which libobs headers
require.

```
git clone --depth 1 --branch 32.2.2 https://github.com/obsproject/obs-studio.git \
  "$HOME/Developer/obs-studio"
```

Build with Qt6 (Homebrew `qtbase`), CMake and Ninja on PATH.

### macOS

```
OBS_SRC="$HOME/Developer/obs-studio" ./iso-recorder/build-mac.sh
```

The result is `iso-recorder/build-mac/iso-recorder.plugin`. Pass `--install` to copy
it into `$HOME/Library/Application Support/obs-studio/plugins/`:

```
OBS_SRC="$HOME/Developer/obs-studio" ./iso-recorder/build-mac.sh --install
```

Restart OBS afterwards. Without `--install`, nothing is installed.

The build rewrites the plugin's Qt load commands to OBS's `@rpath` form so it loads against
the Qt already inside OBS.app at runtime rather than Homebrew's, then re-signs ad-hoc.

### Windows

Headers in `C:\obs-studio`, Qt6 + CMake + Ninja installed, then:

```
powershell -File iso-recorder\build-win.ps1 -ObsSrc C:\obs-studio
```

The result is `iso-recorder/build-win/iso-recorder.dll`. Pass `-Install` to copy the
DLL into `%ProgramData%\obs-studio\plugins\iso-recorder\bin\64bit\` and the `data\`
folder into `%ProgramData%\obs-studio\plugins\iso-recorder\data\`:

```
powershell -File iso-recorder\build-win.ps1 -ObsSrc C:\obs-studio -Install
```

Restart OBS afterwards. Without `-Install`, nothing is installed.

## Using the dock

Open it from OBS's **Docks** menu → **ISO Recorder**. One window holds everything.

- **Picture / Sound** — every source in the collection, listed under Picture when it
  has a picture and under Sound when it is audio-only. Each row has a checkbox and
  shows what it is doing: a red `●` while it is writing a file, `not recorded` if it
  stopped early, nothing while it is idle. Hover a row for its size and file name.
- **Tick a source to record it live.** With a session running, ticking starts that
  source's file immediately; unticking ends and finalizes it. With no session
  running, the tick is remembered and applies the moment one starts. A source added
  to the collection appears in the list on its own, and can be ticked mid-session.
- **Your ticks are remembered**, per scene collection, so you tick once and not
  again — they come back when you reopen OBS and again after you stop a recording.
  Untick everything and the slate is cleared: nothing is restored by itself. If
  **Start and stop with the stream** is on and a stream starts with nothing ticked,
  the log warns and the dock says so, because otherwise the stream passes with no
  files at all.
- **Record / Stop recording** starts or stops every ticked source at once, and a red
  `● Recording · 0:12` line shows how long the session has been running. **Start and
  stop with the stream** (on by default) does the same from the stream's own start
  and stop, so you never forget to press record. **Also record the live scene** (off
  by default) adds a reference recording of the program, written as `00_Session.mov`.
- **Session** holds the output folder (with Browse…) and the video encoder — "Same as
  the stream" by default. Audio is always saved as 24-bit WAV. A hardware encoder can
  refuse a source it cannot handle (most often a picture smaller than its minimum frame
  size, such as a small avatar or overlay). That source falls back to `obs_x264` on its
  own rather than failing — the log says so when it happens.
- **New game** (blue) splits the session into game folders without stopping it. Each
  press closes every running file and opens the next folder — `01_Game`, `02_Game`,
  and so on — restarting the same ticked sources inside it. The clock does **not**
  reset, so files in a later folder are simply later on the timeline and everything
  still lines up. Press it as many times as you like.
- **Where files land** — one folder per session under the base path, named by start
  time. The default is a folder of its own in your videos folder, e.g.
  `~/Movies/ISO Recorder/2026-09-12 14-32-05/` on macOS or
  `C:\Users\you\Videos\ISO Recorder\2026-09-12 14-32-05\` on Windows. Inside it sits a
  folder per game (`01_Game/`, `02_Game/`, …) holding the files for that game. Video
  and audio are numbered independently in dock order as `NN_<source>.<ext>`, so
  `01_game.mov` and `01_game.wav` can both exist. Ticking a source a second time
  writes `_2` rather than reopening the first file. `session.json` and `README.txt`
  live in the session folder and list every file's exact start time.
- **Encoder warning** — ticking more visual sources than the machine is expected to
  keep up with shows a confirmation, but does not refuse; you may know better. See
  [Measurements](#measurements) for how that ceiling is being measured.

## Manual checklist

Run this on both platforms with a running OBS:

1. one visual source → one file, plays, correct length
2. two overlapping visual sources → two files, each the pre-composite picture
3. one video + one audio-only source → a `.mov` and a `.wav`
4. all sources at once → all files
5. tick a source after the session has started → its file starts then, `startOffset` is non-zero, and
   it still lines up in Resolve
6. untick and re-tick one source in a session → two files, both entries in the manifest
7. add a source to the collection mid-session → it appears in the dock and can be ticked
8. scene switch mid-record → every armed source keeps recording
9. source deleted mid-record → recorder stops, marked `aborted`, others continue
10. mic unplugged mid-record → that file ends, others continue
11. stream start/stop drives the session with the toggle on
12. manual button works with no stream
13. **sync:** a visible flash and an audible clap at the start; files armed at the start line up at
    zero in DaVinci Resolve, a later file sits at its listed offset, and `startOffset` matches
14. **handoff:** the whole folder imports into Resolve; the editor confirms the stems are useful
15. **new game:** press it mid-record → a `02_Game/` folder appears, its files start again at
    `01_`, and each one still carries its true offset from the session clock
15. forced quit mid-record → the `.mov` files still play; WAV is checked (see Further Notes)

## Measurements

The encoder ceiling has been measured on the Windows box. Sync drift over a long
session was attempted and is still not conclusive, so that one value remains an
assumption rather than a result.

### Sync drift over a long session

The whole handoff rests on this: that a video clock and an independent audio clock do
not drift apart over a session.

**How to measure.** Record one video source and one audio-only source for three hours.
At the start, put a visible flash and an audible clap in front of both. Check that the
later file's `startOffset` stays constant, and that audio stays sample-aligned to video
for the whole run. If the two clocks drift, the fix is a shared clock or periodic
resync, and it changes the design.

**Not yet settled.** The build writes each file's start offset from one session epoch
(`session.json`, `README.txt`) and assumes the clock holds for the whole session. That
is still an assumption.

**A 30-minute run was made and it is not conclusive.** The test media carried a
full-frame flash and a 1 kHz beep on the same 60-second boundary, so the gap between the
two measures audio-against-video skew inside one file. Measured through the plugin, that
gap grew from about 30 ms after the first minute to about 300 ms after the last —
roughly 10 ms per minute, which would be about 1.8 s across a three-hour stream.

The number cannot be trusted yet, because the flash and the beep come from the same
media file and are therefore locked together on disk — the source's own beep-to-flash
gap stays at 10 ms from the first minute to the last. Anything that grows between them
in a recording is playback timing, not content.

The plugin stamps a video frame when it is rendered and carries the source's own
timestamp for audio, so a source whose playback runs late shows up as exactly this kind
of growing gap. This media source was late by about 1.5 s over the 28 minutes, which is
what makes it a poor ruler. A live source — a game, a screen, a microphone — has no
playback clock to fall behind, which is why the real measurement has to be made with
those.

The container itself came back clean — video 107,972 frames and audio 84,353 AAC frames,
both 1799.53 s, both starting at zero — so nothing was dropped or truncated, and the
two files of that run differed in length by only 78 ms (video 1800.0025 s, audio
1800.0805 s).

**What is still owed.** Repeat it with real sources — a game or window capture for
picture and a microphone for sound, three hours, on the machine that actually streams.
Until then treat per-file audio-against-video alignment as unproven.

### Simultaneous 1080p60 hardware encodes

Each visual source is its own encode, so N armed visuals cost N hardware encodes at
once, alongside the stream.

**How to measure.** On the Mac and on the Windows box, with a normal stream running,
arm one 1080p60 visual source at a time and check for dropped frames, until the machine
can no longer keep up. The count it sustains before frames drop is the ceiling.

**Measured on the Windows box.** NVIDIA GeForce RTX 2050, OBS 32.2.2, NVENC, recording
N 1080p60 sources at once with the stream idle:

| visuals | result |
|---|---|
| 4 | all four clean, each 60.0 fps |
| 6 | five clean; one lost about a third of its frames |
| 8 | every file lost about half its frames |

The loss is silent. `session.json` still says `complete` for every file and each file's
own frame rate still reads 60 — it is the file that comes out short, not its rate. So a
machine that has quietly stopped keeping up looks exactly like one that is fine.

**The threshold is 3** (`kEncoderWarnThreshold` in `src/iso-session.hpp`) — one below the
four this box sustained, because the stream's own encode shares the same hardware and the
measurement was taken with it idle. Tick more visual sources than that and the dock warns
and asks to confirm; it never refuses, because the operator may know better.
