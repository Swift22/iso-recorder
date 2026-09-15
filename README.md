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
checkout tag must match the installed OBS (32.2.2 was used for the current release).
`build-mac.sh`
also fetches the header-only SIMDe library into `deps/simde`, which libobs headers
require.

```
git clone --depth 1 --branch 32.2.2 https://github.com/obsproject/obs-studio.git \
  "$HOME/Developer/obs-studio"
```

Build with Qt6 (Homebrew `qtbase`), CMake and Ninja on PATH.

### macOS

```
OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh
```

The result is `build-mac/iso-recorder.plugin`. Pass `--install` to copy
it into `$HOME/Library/Application Support/obs-studio/plugins/`:

```
OBS_SRC="$HOME/Developer/obs-studio" ./build-mac.sh --install
```

Restart OBS afterwards. Without `--install`, nothing is installed.

The build rewrites the plugin's Qt load commands to OBS's `@rpath` form so it loads against
the Qt already inside OBS.app at runtime rather than Homebrew's, then re-signs ad-hoc.

### Windows

Headers in `C:\obs-studio`, Qt6 + CMake + Ninja installed, then:

```
powershell -File build-win.ps1 -ObsSrc C:\obs-studio
```

The result is `build-win/iso-recorder.dll`. Pass `-Install` to copy the
DLL into `%ProgramData%\obs-studio\plugins\iso-recorder\bin\64bit\` and the `data\`
folder into `%ProgramData%\obs-studio\plugins\iso-recorder\data\`:

```
powershell -File build-win.ps1 -ObsSrc C:\obs-studio -Install
```

Restart OBS afterwards. Without `-Install`, nothing is installed.

## Using the dock

Open it from OBS's **Docks** menu → **ISO Recorder**. One window holds everything.

- **Picture / Sound** — every source in the collection, listed under Picture when it
  has a picture and under Sound when it is audio-only. Each row has a checkbox and
  shows what it is doing: a red `●` while it is writing a file, `not recorded` if it
  failed, `incomplete` if it finished but came out short of frames, nothing while it is
  idle. Hover a row for its size, its file name, and the reason when something went
  wrong.
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
- **Session** holds the output folder (with Browse…), the video encoder — "Same as the
  stream" by default — and **Recording size**, also "Same as the stream" by default.
  Recording size only ever scales a file down: pick 1080p and a source bigger than that is
  recorded at that height, keeping its shape, while one already smaller is left alone. It
  shrinks what is converted and encoded, not what each recording renders, so it buys back
  encoder work rather than render work. Audio is always saved as 24-bit WAV. A hardware
  encoder can refuse a source it cannot handle (most often a picture smaller than its
  minimum frame size, such as a small avatar or overlay). That source falls back to
  `obs_x264` on its own rather than failing — the log says so when it happens.
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

## Transparency and the green background

Every file is recorded per source, before the sources are composited. An OBS view has no
way to clear itself to transparent, so anything genuinely see-through in a source — a
VTuber avatar with no background, a logo with alpha — has nothing behind it, and would
otherwise record as black.

Instead the plugin puts a plain green panel behind every picture it records, so those
see-through areas come out as solid chroma green and can be keyed out in your editor the
same way a green screen is. It is always on and has no setting; a source that fills its
whole frame is unaffected either way.

Two things follow.

- **The green is not perfect green.** Video is encoded at 4:2:0, where colour is kept at
  half resolution, so a partly see-through pixel picks up a soft green fringe. Keying with
  a little tolerance removes it.
- **A Spout source can cover it.** Spout sources carry their own **Composite mode**. When
  it is *Opaque*, the source arrives with no transparency at all, so its background covers
  the green panel and records as black. The plugin does not change another plugin's
  settings. It warns — in the log and in the dock — and names the fix: set that source's
  Composite mode to **Premultiplied Alpha**. veadotube mini and VTube Studio both expose
  it.

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
16. forced quit mid-record → the `.mov` files still play; WAV is checked (see Further Notes)

## Measurements

The encoder ceiling has been measured on the Windows box, and so has the alignment of
the two streams inside a recording. What remains unmeasured is an independent audio
clock — a microphone alongside a picture — over a full three-hour session.

### Sync drift over a long session

The whole handoff rests on this: that a video clock and an independent audio clock do
not drift apart over a session.

**How to measure.** Record one video source and one audio-only source for three hours.
At the start, put a visible flash and an audible clap in front of both. Check that the
later file's `startOffset` stays constant, and that audio stays sample-aligned to video
for the whole run. If the two clocks drift, the fix is a shared clock or periodic
resync, and it changes the design.

**Not yet settled.** The build writes each file's start offset from one session epoch
(`session.json`, `README.txt`) and assumes the clock holds for the whole session.

**A 30-minute run says the recorder holds.** These files carry their index at the end, so
the finished `.mov` can be read without decoding it. In the 30-minute run's
`DriftMedia.mov`:

- video — h264, **107,775 frames** at 60 fps: **1796.2500 s**, starting at 0
- audio — AAC, **84,199 packets** at 48 kHz: **1796.2240 s**, starting at 0

They end **26 ms apart** after 1796.25 s, and 107,775 ÷ 60 = 1796.25 exactly, so the
video stream is frame-exact and the difference is inside a single AAC packet (1024
samples, 21.3 ms). The two timelines did not separate. Encoding lag on that run was 3
frames per file, 0.0%.

**The earlier ~10 ms/min figure was the ruler, not the recorder.** That run's test media
carried a full-frame flash and a 1 kHz beep on the same 60-second boundary, so the gap
between them measured audio-against-video skew inside one file; through the plugin it
grew from about 30 ms after the first minute to about 300 ms after the last. The flash
and the beep come from the same media file and are locked together on disk — the source's
own beep-to-flash gap stayed at 10 ms from the first minute to the last — so what grew
was the source's **playback** clock falling behind: that media source ran about 1.5 s
late over 28 minutes. The plugin stamps a video frame when it is rendered and carries the
source's own timestamp for audio, so a source whose playback runs late shows up as
exactly that kind of growing gap. It was a poor ruler.

**What is still owed.** That media file's audio and video come from one playback clock,
so this measures the plugin's two timelines rather than two independent clocks — and the
collection had no audio-only source to arm. The remaining run is a **microphone** against
a game or screen capture, three hours, on the machine that actually streams. Until then
treat independent-clock alignment as unproven.

### Simultaneous 1080p60 hardware encodes

Each visual source is its own encode, so N armed visuals cost N hardware encodes at
once, alongside the stream. Each encode also takes its own NVENC session — one per
armed source, plus one for the stream — and OBS never pools or shares them, so nothing
here runs you out of sessions.

The artificial session cap NVIDIA puts on consumer GeForce cards is now high enough
that it is no longer what stops you at this scale, and pro cards have no cap at all.
What does stop you is the GPU's encode throughput, which is what the numbers below
measure. (OBS 32.2.2 bundles NVIDIA's SDK 13, so it needs driver 570 or newer.)

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

A file that comes out short used to be silent: `session.json` said `complete` for every
file, and each file's own frame rate still read 60, so a machine that had quietly stopped
keeping up looked exactly like one that was fine. That is now caught. When a recording
ends, the frames the encoder actually delivered are compared against what the recording's
own length should have produced; a gap of more than about 2% marks that file `aborted`
instead of `complete`, and the dock, `session.json` and the session's `README.txt` all say
in plain words roughly how many frames went missing.

**Measured again on the fixed build** — three armed sources plus the stream, 1080p60, over
139 s: every ISO output ran at `0.0–0.1%` encoding lag, the same as the stream, and wrote
6.0 Mbps, exactly the bitrate the stream encoder was set to. The graphics thread spent a
median of **1.742 ms** of its 16.667 ms budget and the GPU encode thread **0.971 ms**;
handing frames to the encoders (`output_gpu_encoders`) cost **0.08 ms**. Copying the
drawing into a source's own view (`render_main_texture`) is **0.018 ms** a piece, so the
three of them together cost about 0.05 ms — which is why the per-source render pass is
left alone rather than optimised.

**The threshold is 3** (`kEncoderWarnThreshold` in `src/iso-session.hpp`) — one below the
four this box sustained, because the stream's own encode shares the same hardware and the
measurement was taken with it idle. Tick more visual sources than that and the dock warns
and asks to confirm; it never refuses, because the operator may know better.

NVIDIA's split-frame encoding is not an escape hatch: OBS only offers it for HEVC and
AV1, and only on a GPU with more than one NVENC engine. This plugin records H.264, and
the tested laptop has a single engine.

## Contributing

Issues and pull requests are welcome. This is a small plugin and the bar is simple:
a change that makes recording more reliable, or the UI easier to understand.

- **Bug reports** — use the bug template. The single most useful thing you can attach is
  the plugin's line in the OBS log (`[iso-recorder] loaded (v…)`) plus what you ticked and
  what came out. Windows paths in a log are fine; skim it first if you would rather not
  share your user name.
- **Pull requests** — keep them focused, one change per PR. Say what you tested and on
  which platform. `docs/spec.md` is the design authority and `docs/adr/` records the
  decisions that are expensive to reverse; if a change contradicts one of those, raise it
  in the PR rather than quietly diverging.
- **Tests** — `session-writer` and `wav-writer` are pure and covered. If you touch them,
  or add another pure helper, add or extend a test. Build the plugin once and then run:

  ```
  ctest --test-dir build-mac --output-on-failure
  ```

  `session-writer`'s tests assert exact bytes of `session.json` and `README.txt`; update
  the expected strings deliberately, not by pasting the new output over the old.
- **Style** — match the file you are editing: tabs, C++17, no comments unless a line
  encodes something non-obvious (an OBS quirk, a format constraint). Comments explaining
  *what* the code says are noise here; the design record carries the *why*.
- **Platforms** — Windows x64 and macOS arm64, against OBS 32.x. Anything that touches the
  OBS API should be checked against the matching `obs-studio` tag's headers.

By contributing you agree your work is licensed under the GPL-2.0, the same as the rest of
the project (see `LICENSE`).

## Licence

GPL-2.0. The plugin links libobs, which is GPL-2.0, so it has to be too.

