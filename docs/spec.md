# Spec — ISO Recorder

Status: ready-for-agent. No issue tracker in this repo, so this file is the ticket.

This is an **OBS Studio plugin**, not part of the Toast pipeline. It shares the machine and the
stream with `studio/`, `emote-forge/` and the skills, and nothing else. The short-form vocabulary
in `CONTEXT.md` does not apply here; **ISO** means one isolated recording of one source.

## Problem Statement

A stream comes out of OBS as a single composited picture. Handing that to an editor is handing
them the wrong thing: the game and the character are baked together, every voice is summed into
one or two tracks, and none of it can be pulled apart again. The editor cannot move the character off the
gameplay, repair a bad capture crop, or rebalance one person's mic, because those parts no longer
exist as separate signals. Today the only way back is to re-download the VOD and rebuild by hand —
the exact work the editor is paid to avoid.

Nothing in the stream is currently kept on its own. The Twitch VOD keeps one mixed picture; the
local recording keeps one mixed picture. Both are dead ends for an editor.

The rig can be a Mac and a Windows box, so a tool that only works on one of them only works half
the time.

## Solution

An OBS Studio plugin, **ISO Recorder**, with one dock.

The dock lists every source in the current scene collection, live. You tick the ones worth
keeping — and keeps that control *while recording*: tick a source at any moment to start its file,
untick it to end it, tick a source that did not exist when the session began. Nothing is fixed at
the start button. Each ticked source is recorded **by itself**: its own picture, its own audio, its
own file, timed from one session clock.

When streaming stops the plugin writes a `session.json` describing what is in the folder and a
`README.txt` telling the editor what to do with it. The editor opens one folder in Resolve, places
each clip at the start time the README lists, and every part is there and in sync. Nothing needs
re-exporting, splitting, or guessing.

The normal stream and the normal local recording are untouched. This runs alongside them.

## User Stories

### Choosing what to keep

1. As a streamer, I want a list of every source in the collection, split into ones with a picture and ones with only sound, so that I can see what is recordable without knowing OBS internals.
2. As a streamer, I want a checkbox per source, so that I choose what gets kept rather than the plugin deciding.
3. As a streamer, I want the game, the character, and a hidden source to all be selectable, so that overlapping and off-screen sources are my choice, not a limitation.
4. As a streamer, I want each audio channel — mic, game, Discord — to be a separate tick, so that voices and game sound come out as separate files.
5. As a streamer, I want the list to stay live, so that a source I add to the collection shows up without restarting OBS.
6. As a streamer, I want to tick or untick any source **while recording**, so that I can start capturing something new or drop something I am done with without stopping the session.
7. As a streamer, I want to arm a source that did not exist when the session started, so that opening a new app or adding a new capture mid-stream is not locked out.
8. As a streamer, I want my selection to persist between sessions, so that I start from the same set and then change it as the stream goes.
9. As a streamer, I want the dock to say, per source, whether it is recording and where the file is going, so that I know before the stream, not after, that something is wrong.

### Recording

10. As a streamer, I want recording to start and stop with the stream by default, so that I never forget to press record.
11. As a streamer, I want a manual record button too, so that I can capture a session that is not a stream.
12. As a streamer, I want every file timed from one session clock, so that the editor never has to sync by hand — each file's start is written down.
13. As a streamer, I want switching scenes mid-stream to keep recording every armed source, so that a scene change does not silently drop a feed.
14. As a streamer, I want a source I delete mid-stream to stop cleanly and be noted, so that the rest of the recording survives.
15. As a streamer, I want the hardware encoder I already stream with used by default, so that the extra recordings do not lean on the CPU.
16. As a streamer, I want to be warned before I start — or the moment I arm one more — that the selection is more than the machine can encode, so that I do not throttle my own stream discovering it live.
17. As a streamer, I want the plugin to refuse to start and tell me why when the disk is full or the folder is unwritable, so that I never lose a stream to a silent failure.

### After the stream

18. As the editor, I want one folder per stream, so that I know exactly which files belong together.
19. As the editor, I want one video file per visual source and one audio file per channel, named after the source, so that I do not have to guess what each file is.
20. As the editor, I want lossless, unprocessed audio, so that I can balance and repair it myself.
21. As the editor, I want the raw picture of each source before anything was composited over it, so that I can reframe and reorder freely.
22. As the editor, I want the exact start time of every file, in the manifest and in the README, so that a clip that was armed later is placed exactly where it belongs.
23. As the editor, I want a plain-language `README.txt`, so that I know how the files line up without being told by the streamer.
24. As the editor, I want the files to survive OBS crashing, so that a three-hour session is not lost to a hard crash.
25. As the editor, I want no re-encode, no trimming and no effects baked in, so that I start from the cleanest possible source.

### Building and owning it

26. As a streamer, I want the same plugin on the Mac and the Windows box, so that both streaming setups produce the same folder.
27. As a streamer, I want to build it from the repo with one command per platform, so that I can change it without remembering how it was made.

## Implementation Decisions

### Where the code lives

This repository, a CMake project in C++ against **libobs** and
**obs-frontend-api**, modeled on OBS's own plugin template. It is a plugin, not a program: OBS
loads it in-process, and it is built once per platform.

```
./
  CMakeLists.txt
  build-mac.sh / build-win.ps1
  src/
    plugin-main.cpp        module load/unload, dock registration, frontend events
    iso-dock.{hpp,cpp}     the Qt dock
    iso-session.{hpp,cpp}  one session: the recorders and their lifecycle
    source-recorder.{hpp,cpp}  one source's capture + encode + mux
    session-writer.{hpp,cpp}   pure: filenames, session.json, README.txt
    iso-settings.{hpp,cpp}  persisted dock settings
  tests/                   session-writer, run without OBS
  data/locale/en-US.ini
```

Targets **OBS 32.x** on both platforms — the version already installed. The plugin links
`OBS::libobs` and `OBS::obs-frontend-api` and builds its dock with **Qt6**, the same toolkit OBS 32
uses; the build resolves all three against an installed OBS or the OBS SDK.

It is deliberately **not** inside `studio/` or `emote-forge/`. Those are Tauri and Swift; this is a
native OBS plugin with a different lifecycle, and mixing it in would make both harder to build.

### Why a dock plugin and not a filter

The obvious precedent, Source Record, implements capture as an **OBS filter** on a source. We do
not, for one reason: a filter needs a parent with a picture, so a pure audio source — the mic, the
Discord tap — cannot take one. A dock-owned recorder can hold a reference to *any* source,
audio-only or not, and record it on its own terms.

The mechanism is the same one Source Record proved; the ownership is different. We keep an
`obs_source_t *` for each armed source (strong ref, and `obs_source_inc_showing` while active so
a source that is not in the active scene still produces frames), and drive our own encoders from
it. We write our own clean implementation; we do not fork GPL code.

### The one seam

`session-writer` is pure. Given the session's start time, the video format, and a list of finished
recordings — source name, kind, filename, codec, start offset, duration, status, error — it returns
the exact bytes of `session.json` and `README.txt`, and the exact filename for a source. It touches
no OBS object, no filesystem, no clock.

Everything else in the plugin either produces that list or consumes what `session-writer` emits.
This is the seam the automated tests sit on. The rest is verified by hand.

### The recorder per source

`SourceRecorder` owns one source's whole pipeline. Created when a source is **armed** — at session
start or later — and destroyed when it is disarmed, when the source goes away, or when the session
ends. Arming again later creates a new recorder and a new file.

**Video path.** Read the source's current size, create an `obs_view_t` with `obs_view_create()`, and
add a video output with `obs_view_add2()`, using the source's dimensions as base size and the
recording-size cap as output size — the source's own dimensions when no cap applies. Set the source
into the view (`obs_view_set_source`), on the channel above a private `color_source` filled with
**chroma green** (`0xFF00FF00`), so a source's transparent regions record green instead of black
(`docs/adr/0002`). The view's `video_t *` is timestamped by
OBS's graphics thread, so the encoder receives correctly-timed frames without us touching pixels.
This is the crucial difference from a naive texrender grab: the frames carry OBS's clock, which is
what makes A/V sync across independent files hold.

**Audio path.** Open a private `audio_output` with an `audio_output_info` whose `input_callback`
reads the source (`obs_source_get_audio_mix`). For a source with `OBS_SOURCE_COMPOSITE` — a scene,
or anything holding children — the callback enumerates the active tree
(`obs_source_enum_active_tree`), sums the children, and clamps to `[-1, 1]`, so a nested scene
records as one signal. Audio-only sources take this same path and simply have no video encoder; the
output becomes an audio-only file.

The tap is the source's **output**: its own filters — a mic's noise gate, EQ, compressor — are
included, and the scene's volume, ducking and limiter are not. That is the point. The editor gets
the cleanest stem the source can already produce, not the stream's mix decisions.

**Encoders.** `obs_video_encoder_create()` and `obs_audio_encoder_create()` per recorder, then
`obs_encoder_set_video(enc, view_video)` and `obs_encoder_set_audio(enc, private_audio)`.

**Output.** `obs_output_create()` with the hybrid container (below), path and filename set from
`session-writer`, encoders attached with `obs_output_set_video_encoder()` /
`obs_output_set_audio_encoder()`. Start with `obs_output_start()`; stop with
`obs_output_force_stop()`, which finalizes the container; release the encoders from the output's
`stop` signal, never before.

All create/start/stop happens on the OBS main thread, queued exactly as Source Record queues it
(`obs_queue_task`), never from the Qt thread directly.

### Containers and formats

- **Video:** the crash-tolerant hybrid MOV OBS's own recording uses (`mov_output` /
  `hybrid_mov`), so a hard crash leaves a playable file rather than a broken one. Extension `.mov`,
  codec H.264 by default.
- **Audio:** `ffmpeg_muxer` with the `ffmpeg_pcm_s24le` encoder and a `.wav` extension, giving
  true **WAV, 24-bit PCM at 48 kHz**. This encoder ships in OBS (verified against
  `obs-ffmpeg-audio-encoders.c`), so no custom writer and no lossy fallback are needed. FLAC
  (`ffmpeg_flac`) and AAC (`ffmpeg_aac`) are offered as alternatives; WAV is the default.
- **What gets a file:** a selected source is recorded as what it is. A source with a picture
  becomes a `.mov`, and any audio it carries is muxed into that same file. A source with only
  sound becomes a `.wav`. So the mic, the Discord tap and a desktop-audio source each get a
  `.wav`, and the game capture and the character each get a `.mov` — the visual sources are the
  ones that already have a picture of their own.

### Sync

- Every recorder starts inside **one** queued main-thread task per arm event, so arming several
  sources together begins their outputs within the same render tick.
- The session records an **epoch** — the OBS timestamp at session start.
- Each recorder records the timestamp of its first encoded video frame (or first audio sample for
  audio-only), and `session-writer` writes the difference from the epoch as a **start offset in
  seconds**, frame-quantized for video.
- A source armed when the session starts has an offset of zero, give or take a frame of encoder
  startup. A source armed mid-stream has the offset of the moment it was armed, which may be
  minutes. Either way the offset is written down, so the editor places the clip exactly rather than
  by eye.

### Output layout and naming

One folder per session, under a base path set in the dock:

```
Movies/ISO Recorder/2026-09-12 14-32-05/          # macOS default
Videos\ISO Recorder\2026-09-12 14-32-05\          # Windows default
  00_Session.mov        # only when "also record the live scene" is on
  01_game.mov           # the game, pre-composite
  02_veado.mov          # the character, pre-composite
  01_game.wav           # the game's sound, on its own source
  02_Discord.wav        # Discord, on its own
  03_Mic.wav            # the mic, on its own
  session.json
  README.txt
```

`NN_<source>.<ext>`, numbered in dock order within each category: video, then audio. Video and
audio are numbered independently, so a `01_game.mov` and a `01_game.wav` can both exist. Source
names are sanitized to filename-safe characters.

Arming a source, ending it, then arming it again in the same session produces a **second file**,
suffixed `_2` (then `_3`) — `03_Mic.wav`, `03_Mic_2.wav`. Each segment is its own entry in the
manifest with its own start offset and duration, so a mid-stream switch is visible rather than
silently joined.

The composite is opt-in ("Also record the live scene"). When it is on, its recorder is the one
exception to "bound to one source": it follows the current program scene, re-pointing on a scene
switch, and is written as `00_Session.mov`. A scene switch can cost it a frame; that is acceptable
for a reference file and is why it is off by default.

### `session.json`

```json
{
  "version": 1,
  "session": {
    "started": "2026-09-12T14:32:05+03:00",
    "ended":   "2026-09-12T16:47:28+03:00",
    "duration": 8123.4,
    "obs": "32.0.1",
    "platform": "macos",
    "video": { "width": 1920, "height": 1080, "fps": 60 }
  },
  "recordings": [
    { "source": "game",    "kind": "video", "file": "01_game.mov",    "codec": "h264",       "startOffset": 0.0,    "duration": 8123.4, "status": "complete" },
    { "source": "veado",   "kind": "video", "file": "02_veado.mov",   "codec": "h264",       "startOffset": 0.017,  "duration": 8123.3, "status": "complete" },
    { "source": "Discord", "kind": "audio", "file": "02_Discord.wav", "codec": "pcm_s24le",  "startOffset": 0.0,    "duration": 8123.4, "status": "complete" },
    { "source": "Mic",     "kind": "audio", "file": "03_Mic.wav",     "codec": "pcm_s24le",  "startOffset": 0.0,    "duration": 4210.5, "status": "complete" },
    { "source": "Mic",     "kind": "audio", "file": "03_Mic_2.wav",   "codec": "pcm_s24le",  "startOffset": 4210.5, "duration": 3912.9, "status": "complete" },
    { "source": "ofes",    "kind": "video", "file": null,             "codec": null,         "startOffset": null,   "duration": null,    "status": "failed",   "error": "encoder refused: too many sessions" }
  ]
}
```

`startOffset` is seconds from the session epoch to that file's first frame/sample. It is zero for
anything armed when the session started (plus at most a frame of encoder startup) and grows with
each later arm; `null` when the file never recorded. `status` is `recording`, `complete`, `aborted`
(stopped early — the source was deleted, or it was disarmed) or `failed` (never recorded; `error`
says why in plain words). A source armed more than once in a session appears several times, once
per file, each with its own offset. A failed recording keeps its entry so the editor can see it was
meant to exist.

The manifest is written **when the session starts**, with every recording listed as `recording`,
and rewritten when it stops with final values. A session that crashed therefore leaves a manifest
that still says `recording` — that is how the plugin knows, and tells you, that a folder was cut
short.

### `README.txt`

Written for a person, not a parser:

```
Separate recordings from the stream on 12 September 2026.

Every file is timed from the same clock — the moment the recording session
began. Most start right at the beginning; a track switched on later starts
later, and its start time is listed below. Place each clip at its start time
and they all line up.

  00_Session.mov    0:00.0        the stream as viewers saw it (reference)
  01_game.mov       0:00.0        the game, on its own
  02_veado.mov      0:00.0        the character, on its own
  01_game.wav       0:00.0        game sound
  02_Discord.wav    0:00.0        Discord call
  03_Mic.wav        0:00.0        microphone
  03_Mic_2.wav      1:10:10.5     microphone (switched on later)

Nothing is cut, mixed, or ducked — the tracks are each source exactly as
OBS saw it. session.json has the exact start of every file if you ever
need to nudge one.
```

### The dock

A Qt dock, registered with `obs_frontend_add_dock_by_id`. One window:

- **Sources** — every source in the collection, video-capable ones under **Picture**, audio-only
  ones under **Sound**, each with a checkbox, its current state and, while it is recording, the
  file it is writing. The list is live: a source added to the collection appears here without a
  restart, and can be armed at once.
- **Session** — output base folder (with a browse button), "start and stop with the stream"
  (default on), "also record the live scene" (default off), the video encoder, the audio format,
  and the **recording size**: *same as the stream*, *original size*, or a cap of 2160p/1440p/1080p/
  720p/480p. The cap shrinks the convert, the readback and the encode in proportion to pixel count;
  it never upscales a smaller source.
- **Controls** — one Record/Stop button for the session, and per-source status text showing the
  file being written or the failure reason.

A checkbox is an **arm**, not a setting that waits for the next session. Ticking it starts that
source's file immediately when a session is running (and remembers the arm, without starting
anything, when one is not). Unticking it ends and finalizes that file. The session's Record button
starts or stops every armed source at once; the stream's own start and stop do the same when the
toggle is on.

The dock holds arming and settings only. It asks `IsoSession` to arm, disarm, start or stop; it
never touches an encoder. Arm state and settings persist in the plugin's own config section; the
running session does not.

### Triggers and lifecycle

- **Start:** `OBS_FRONTEND_EVENT_STREAMING_STARTED` when "with the stream" is on, or the dock
  button. Not `STREAMING_STARTING`: the stream must be away first.
- **Stop:** `STREAMING_STOPPED`, the dock button, or `OBS_FRONTEND_EVENT_EXIT` (stop cleanly, write
  the manifest, then unload).
- **Arming before start:** ticks made while no session is running are remembered and applied the
  moment one starts.
- **Arming mid-session:** ticking a source creates its recorder, starts its file at that moment, and
  records the offset from the epoch. Unticking stops and finalizes it. Arming, ending, then arming
  again produces a second file rather than reopening the first.
- **Nothing armed:** the Record button is disabled and says why, and the stream's start does not
  open an empty session.
- Scene changes are ignored by source recorders — each is bound to its source, not the scene. The
  optional composite recorder is the exception; it follows the program scene.
- A source deleted mid-session (`source_remove` signal) stops that one recorder, marks it `aborted`,
  and leaves the rest running.
- If OBS is force-quit, the hybrid containers are still playable and the manifest is left as it was
  written at start (every entry `recording`); the next start of the plugin flags that folder as
  unfinished rather than pretending it is whole.

### Failure handling

- **Before start:** the base path is checked — exists, writable, free space above a floor (the
  estimated bytes for the armed selection at the chosen bitrate, plus headroom). Anything wrong refuses
  the start and names the problem in the dock. Never a silent partial start.
- **Encoder cap:** the dock computes a warning when the armed selection exceeds what the machine is
  expected to encode (see Further Notes) and requires a confirmation, but does not refuse — the
  operator may know better. The same warning appears the moment arming one more source mid-session
  would cross the line.
- **Per-source failure is isolated.** If one encoder will not open, or a source has no frames, that
  recorder is marked `failed` with a reason and the others run. One bad source never takes the
  session down.
- **Disk fills mid-session:** the failing output is stopped and marked, the dock shows it; the
  others continue.

### Encoder choice and the cost ceiling

Default to the **same hardware encoder the stream uses**, discovered from the streaming output —
its **id and its settings**, so the ISO files match the stream's bitrate and rate control. (Before
this change, "same as the stream" shared only the encoder id, and every ISO encoder ran the
encoder's registered defaults.) Fall back to the platform's preferred hardware encoder
(VideoToolbox on macOS, NVENC/QSV/AMF on Windows) and to `obs_x264` last. Overridable in the dock.
Each visual source is an independent encode: **N armed visuals cost N hardware encodes at once.**
The warning threshold is **3**, set from the measured ceiling on the Windows box (Further Notes),
not a guess. The measured cost is published in `README.md` §Measurements and
`docs/plan-background-and-performance.md`; the remaining duplicate per-source render is accepted
as-is because it is not a measurable share of frame time (`docs/adr/0003`).

### Build and install

- **macOS:** `./build-mac.sh` → `iso-recorder.plugin`, installed to
  `~/Library/Application Support/obs-studio/plugins/` (or the OBS app bundle). Restart OBS.
- **Windows:** `build-win.ps1` → `iso-recorder.dll`, installed to
  `%ProgramData%\obs-studio\plugins\iso-recorder\bin\64bit\` with its `data/`. Restart OBS.
- Both scripts take the OBS install/SDK path if it is not in the default location, and are
  documented in `README.md`.

## Testing Decisions

The automated tests cover `session-writer` only, because it is the one part with no OBS in it and
the part whose output the editor actually reads. A plain C++ test executable in `tests/`, built by
CMake and run with `ctest`, asserts:

- a source name becomes the filename the layout prescribes, and unsafe characters are handled
- numbering is per category and in dock order, and a second segment of one source is suffixed `_2`
- `session.json` is stable byte-for-byte for a fixed input, and a failed recording serializes with
  its error and a null file
- `README.txt` lists every recording in the same order as the manifest, with each start time
- start offsets are rounded to the frame, a zero-offset session reads as all zeros, and one armed
  mid-session serializes its non-zero offset and start time

Everything that needs a running OBS is a **manual checklist** in `README.md`, run on
both platforms:

1. one visual source → one file, plays, correct length
2. two overlapping visual sources → two files, each the pre-composite picture
3. one video + one audio-only source → a `.mov` and a `.wav`
4. all sources at once → all files
5. arm a source after the session has started → its file starts then, `startOffset` is non-zero, and
   it still lines up in Resolve
6. disarm and re-arm one source in a session → two files, both entries in the manifest
7. add a source to the collection mid-session → it appears in the dock and can be armed
8. scene switch mid-record → every armed source keeps recording
9. source deleted mid-record → recorder stops, marked `aborted`, others continue
10. mic unplugged mid-record → that file ends, others continue
11. stream start/stop drives the session with the toggle on
12. manual button works with no stream
13. **sync:** a visible flash and an audible clap at the start; files armed at the start line up at
    zero in DaVinci Resolve, a later file sits at its listed offset, and `startOffset` matches
14. **handoff:** the whole folder imports into Resolve; the editor confirms the stems are useful
15. forced quit mid-record → the `.mov` files still play; WAV is checked (see Further Notes)

## Out of Scope

- **Timeline export.** No `.otio`, FCPXML or EDL in v1. The files and the manifest are the
  contract; a ready-made timeline is v2.
- **The composite and the stream.** OBS already makes those. The optional `00_Session.mov` is a
  convenience, not a replacement.
- **Editing, trimming, retiming, effects.** Files come out as encoded, nothing baked in.
- **Re-encoding or normalising the stems.** The editor does that.
- **Audio that is not already on a source.** We record what the selected source emits, nothing
  else. Summing mic and game into one "voice" track is not offered — separate is the point.
- **A Linux build.** Mac and Windows only.
- **Adding or editing capture sources.** Splitting Discord from game on Windows may need per-app
  capture sources added to that machine's scene collection; that is OBS setup, not plugin code.
- **Publishing or uploading anything.** The folder is the end of the line.

## Further Notes

**An ADR is warranted.** Recording each source pre-composite, in-process, rather than splitting a
composited recording after the fact is a surprising, hard-to-reverse decision that a future reader
will question. It belongs in `docs/adr/`. So does the choice to be a dock-owned
recorder rather than a filter, for the audio-only reason above.

**Staggered starts are the editor's job, but a mechanical one.** Because a source can be armed
mid-session, not every file begins at zero. The README prints each file's start time next to its
name, so alignment is arithmetic rather than guesswork; the v2 timeline export would remove even
that step.

**Things to measure on the real machines before trusting the numbers:**
1. **Drift over a long session.** One video + one audio source recorded for three hours: does the
   `startOffset` stay constant, and does audio stay sample-aligned to video? This is the single
   assumption the whole handoff rests on. If independent audio clocks drift against the video
   clock, the fix is a shared clock or periodic resync, and it changes the design.
2. **The real encoder ceiling.** How many simultaneous 1080p60 hardware encodes the Mac and the
   Windows box sustain before frames drop, with the stream running. Set the warning threshold from
   this, not from a guess.
3. **WAV after a hard crash.** The hybrid MOV survives a crash; a WAV being written by
   `ffmpeg_muxer` may not, because its header is finalized on stop. Test a forced kill. If the file
   is unreadable, either write WAV with a rewriteable header or accept and document it — the
   manifest already tells the editor a session was cut short.
4. **The Windows source inventory.** That machine was not inspected. If Discord and game sound
   currently share one Desktop Audio source, per-app capture sources are needed there before the
   split is possible. Confirm before promising the Windows folder matches the Mac one.

**Terminology.** If "ISO" is adopted as the word for these files, add it to `CONTEXT.md`; it is not
there today.

**This spec has one prototype's worth of risk in it.** The mechanism is proven — Source Record does
exactly this capture — but the multi-recorder session, the manifest, and long-session sync have not
been run. The sync measurement above is the first thing to build, before the dock is finished.
