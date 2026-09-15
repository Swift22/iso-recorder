# Implementation plan — transparent sources and recording cost

**Status:** ready-for-agent. This is the second brief for the plugin; `docs/plan.md` built v1.
Where this file and `docs/spec.md` disagree about the behaviours it changes, this file wins.
Commits go straight to `main`. Supersedes the earlier draft
`docs/plan-background-and-performance.v1.md` (removed; every task in it maps onto Tasks 1–11 here).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

1. A source with a transparent background — **veadotube published over Spout2** — records
   solid chroma green instead of black.
2. Stop the plugin costing more GPU and CPU than it must, and **prove it with numbers
   measured on the Windows box before and after each change**. No claim in this document is
   finished until it has a before-number, an after-number and the counter they came from.

## Decisions already taken (locked)

1. **Always green, hardcoded.** No toggle, no colour picker.
2. **A configurable recording-size cap, defaulting to the stream's output resolution.**
3. **Mirror the streaming encoder's settings** into the ISO encoders.
4. **Write this plan first.** Implement nothing until it is approved.
5. **Nothing is out of scope.** The items an earlier draft excluded are Tasks 6–8.
6. **The Windows box is the measurement machine**, before and after.

## What is actually wrong (measured, with sources)

### The green cannot appear (issue 1)

- `obs_view_render` (`libobs/obs-view.c:118-141`) walks channels 0…63 in ascending order and
  calls `obs_source_video_render` on each. It **sets no blend state and does no clear**.
- `render_main_texture` (`libobs/obs-video.c:171-217`) clears the mix render target to
  `vec4(0,0,0,0)` — **transparent black** — and then calls `obs_view_render`.
- The graphics default blend state is straight alpha (`libobs/graphics/graphics.c:180-189`):
  `src=SRCALPHA`, `dst=INVSRCALPHA`, so `out = src.rgb*src.a + dst*(1-src.a)`. At `a=0` that is
  exactly the destination — a background under the source **does** show through.
- The encoder receives NV12, which has no alpha plane, so transparent pixels have only their
  RGB value to fall back on: black. There is **no `obs_view_t` equivalent of
  `obs_display_set_background_color`** (`obs-display.c:193-225` is display-only). That is why
  the background has to be a *source*.
- **The Spout2 trap.** The Spout2 source plugin never sets a default for `compositemode`, so it
  reads `0` and falls into the `default:` arm of its switch, which is `OBS_EFFECT_OPAQUE` — the
  shader forces `alpha = 1.0`. In that state transparent pixels are RGB 0 **with alpha 1**, so
  they occlude whatever is behind them and a green underlay changes nothing. Only
  `Premultiplied Alpha` (4) or `Converted Premultiplied Alpha (legacy)` (2) blend correctly.
- Verified on the Windows box: the avatar source is `Mist (veadotube)`, id `spout_capture`,
  flags `0x9` = `OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW`, and `win-spout` is installed at
  `C:\ProgramData\obs-studio\plugins\win-spout`. The sender was **down** during the recorded
  sessions (`Sender has changed / gone away`, `No active Spout cameras`), so nothing rendered at
  all in those logs.

### The cost (issue 2)

- **Every armed source is a full extra render pass per frame.** `can_reuse_mix_texture`
  (`libobs/obs-video.c:136-156`) only reuses a previously rendered mix when
  `other->view == mix->view`. Each recorder owns a **distinct** `obs_view_t`
  (`source-recorder.cpp:104-106`), so no reuse is possible, and `output_frames`
  (`obs-video.c:916-932`) renders **every** mix each frame on the graphics thread.
- **The encoders are not using your stream's settings.** `SessionConfig::videoSettings` is
  never assigned anywhere in the repo — `iso-dock.cpp configFromWidgets()` (L603-611) omits it
  — so `obs_video_encoder_create(id, name, videoSettings_, nullptr)` (`source-recorder.cpp:141`)
  always passes `nullptr`. Every ISO encoder therefore runs on its **registered defaults**
  (x264: 2500 kbps CBR veryfast high; macOS VideoToolbox: ABR 6000, quality 60). "Same as the
  stream" shares only the encoder **id**. `bitrateBitsPerSec` (`iso-dock.cpp:160-178`, read from
  the streaming encoder) is used **only** for the disk-space estimate, so that pre-flight number
  does not match what lands on disk.
- **Every video source gets its own audio device and its own AAC encode, even silent ones.**
  `audio_output_open` spawns an OS thread unconditionally (`libobs/media-io/audio-io.c:376`) and
  `audio_output_close` joins it (L400-401). `openVideoPipeline` (`source-recorder.cpp:158-181`)
  creates one per recorder because an AV output refuses to start without an audio encoder. A
  source with no sound still pays a thread, a resampler and an AAC encode of silence.
- **A silent CPU fallback can wreck a stream.** If a hardware encoder refuses a source — small
  pictures are the usual reason — `start()` falls back to `obs_x264` and logs at `LOG_WARNING`
  (`source-recorder.cpp:113-132`). Several 1080p60 **software** encodes are a far more likely
  cause of "lagged my stream so hard" than the encode count itself, and nothing surfaces it.
- **Nothing in either machine's logs shows the plugin running a session at all**, so the
  incident is captured nowhere. The "before" numbers in Task 1 must be created deliberately.

### Two corrections to earlier findings

- The profile section `[SourceIsoRecorder]` (keys `OutputDirectory`, `FilenamePattern`,
  `Container`, `VideoEncoderId`, `QualityProfile`, `ArmedSources`) is **dead config from a
  removed predecessor plugin** — `E:\dev\builds\removed-plugins\source-iso-recorder`, DLL dated
  2026-04-10, described in its own locale as "a capped, view-based pipeline". Our plugin's
  section is `iso-recorder` and has **never been written on either machine**. The two armed
  UUIDs belong to the predecessor; ignore them.
- The Windows box does have `iso-recorder` installed, and the installed DLL is
  **byte-identical** to `E:\dev\projects\iso-recorder\build-win\iso-recorder.dll`
  (520704 B, SHA256 `CD46B132F660D4D269E6C415C98D3D3E5B9496F49E1929EB665610093A4B2E08`), whose
  source tree is byte-identical to local `HEAD`.

## The Windows measurement machine (reference)

| Thing | Value |
| --- | --- |
| Access | `ssh win` → `misty.taila0c2ff.ts.net`, user `shphw` |
| Shell | Windows PowerShell 5.1 via `powershell -NoProfile -Command -` (no `?:` ternary) |
| CPU / GPU | Intel i5-11400H / NVIDIA RTX 2050 (+ Intel UHD) |
| OBS | 32.2.2 at `C:\Program Files\obs-studio`; `obs.pdb` and `obs64.pdb` present (WPA can symbolicate) |
| Dev copy | `E:\dev\projects\iso-recorder` (non-git; src byte-identical to local HEAD) |
| OBS source | `E:\dev\obs-studio` (exactly 32.2.2 = `ba2f32bd`) |
| Deps / Qt | `E:\dev\obs-libs`; Qt 6.8.3 at `E:\Tools\QtSDK\6.8.3\msvc2022_64` |
| Installed plugins | `iso-recorder` (ours) and `win-spout` (Spout2) |
| Avatar source | `Mist (veadotube)`, type `spout_capture` |

**Durable probe pattern.** Write the script with the `write` tool to
`/var/folders/8x/874f7hpn1p30jch2_5c957hc0000gn/T/opencode/*.ps1`, then

```
ssh -o BatchMode=yes -o ConnectTimeout=15 win 'powershell -NoProfile -Command -' < /path/to/probe.ps1
```

Never inline the PowerShell in the ssh command (zsh→ssh→pwsh quoting mangles it), and keep scans
bounded — a recursive walk of `C:\Program Files\obs-studio` plus `E:\dev` times out.

**Build recipe** (must run inside a VS BuildTools x64 developer shell; `build-win.ps1` has no VS
bootstrap, and in a plain remote session `QTDIR`/`VSCMD_ARG_TGT_ARCH` are empty and `ninja` is
not on `PATH`):

```powershell
cmake -S E:\dev\projects\iso-recorder -B E:\dev\projects\iso-recorder\build-win -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SRC=E:/dev/obs-studio `
  "-DCMAKE_PREFIX_PATH=E:/Tools/QtSDK/6.8.3/msvc2022_64;E:/dev/obs-libs"
cmake --build E:\dev\projects\iso-recorder\build-win
```

**Scriptable runs.** The shipped `obs64.exe` supports `--startstreaming`, `--startrecording`,
`--collection`, `--profile`, `--minimize-to-tray`, `--verbose` and `--unfiltered_log` (all
verified present in the binary; `--disable-shader-cache` is **not** in 32.2.2). Because the
plugin already auto-starts a session on `STREAMING_STARTED` when `withStream` is on, an A/B run
is:

```
obs64.exe --profile Untitled --collection Mist --minimize-to-tray --startstreaming
```

…against a **local** RTMP ingest. Never point an A/B at a public ingest.

## Architecture of the change

- **Green underlay** — a private `color_source`, resolved through
  `obs_get_latest_input_type_id("color_source")` so it lands on the non-obsolete
  `color_source_v3`, placed on the view's channel **0**, with the real source on channel **1**.
  Channel order is guaranteed by `obs_view_render`'s ascending loop, and the default
  straight-alpha blend lets `a=0` pixels show it. Colour is the int `0xFF00FF00` — the source
  reads a `0xAARRGGBB` int through `vec4_from_rgba`, so that is float4(0,1,0,1) — and it is sized
  to the view's base dimensions so it fills exactly.
  *Rejected:* a private scene (plain items use the same straight-alpha path, and a canvasless
  scene falls back to main-canvas dimensions, `obs-scene.c:1488-1500`); a custom shader (we
  cannot intercept the mix render).
- **Spout guard** — detect a `spout_capture` source whose `compositemode` is unset or opaque and
  say so, because the green physically cannot appear until the user changes it. The plugin must
  not silently rewrite another plugin's source settings.
- **Size cap** — write the resolved cap into the view's `obs_video_info.output_width/height` and
  let `render_output_texture` (`obs-video.c:269-322`) do one scale pass. `base_*` stays at the
  source size, because a smaller base **crops** rather than scales. **Never** call
  `obs_encoder_set_scaled_size` or `obs_encoder_set_gpu_scale_type`: `maybe_set_up_gpu_rescale`
  (`obs-encoder.c:228-347`) then creates an extra encoder-only mix per recorder.
- **Encoder settings** — mirror the streaming encoder's settings dict as an owned copy. Copying
  settings cannot trigger that extra-mix path, because it is driven only by explicit API calls,
  never by settings keys.
- **Audio** — one shared `audio_output` device serving up to 6 recorders (`MAX_AUDIO_MIXES`),
  recorder *i* on `mixer_idx = i`, with the device's `input_callback` filling each mixer from that
  source. Replaces N devices, threads and resamplers with `ceil(N/6)`. An `obs_encoder_t` cannot
  be pushed to and cannot be started by a plugin (`obs_encoder_initialize/start/stop` are
  internal), so a device is still required — but only one per six.
- **Per-source frame rate** — `obs_encoder_set_frame_rate_divisor` reduces what reaches the
  encoder. It is encoder-side only: the mix still renders at canvas FPS, because
  `obs_init_video_mix` (`obs.c:605-666`) overwrites `mix->ovi.fps_*` with the main canvas's.
  Offer it as a knob and describe it accurately.
- **Render cost** — **N armed sources will always cost N source renders per frame**, and no
  supported API changes that. What *can* be removed is the duplicate render of a source that is
  also on the main canvas, via a proxy source owning a cached `gs_texrender` (precedent:
  `OPENSPHERE-Inc/branch-output` `filter-video-capture.cpp`, and `obs-filters/gpu-delay.c` for
  the texture-cache discipline). It is invasive and pays off only for on-screen sources, so it is
  a Task with a measurement gate, not an assumption.

## Risk order

1. **Measure** (Task 1) — nothing else starts until the baseline exists.
2. **Green underlay** (Task 2) — the user's actual complaint; small and self-contained.
3. **Spout guard** (Task 3) — never useful alone, but the underlay is inert without it.
4. **Size cap** (Task 4) — the largest measurable cost lever.
5. **Encoder settings mirroring** (Task 5) — makes the cost predictable and the estimate honest.
6. **Shared audio path** (Task 6) — removes N−1 threads.
7. **Per-source frame rate** (Task 7) — encode-only, measured.
8. **Render-pass work** (Task 8) — measurement-gated; may end in "documented as impossible".
9. **Surface the CPU fallback and the cost** (Task 9).
10. **Measure again** (Task 10) and **correct the record** (Task 11).

## Global constraints

Everything in `docs/plan.md` § Global Constraints still applies, plus:

- **No comments in code unless a non-obvious OBS constraint needs one.** Several tasks below
  exist purely because of such a constraint — those get one.
- `ctest --test-dir build-mac --output-on-failure` stays green. `session-writer` and the WAV
  header builder remain the only automated targets.
- **Every new persisted setting goes into both `loadConfig` and `saveConfig`
  (`src/iso-settings.cpp:11-43`)**, and the config section stays `iso-recorder`.
- No OBS call from the Qt thread. Session work goes through `obs_queue_task(OBS_TASK_UI, …)`.
- Nothing outside this repo is modified. In particular the plugin never edits another plugin's
  source settings.
- **A measurement is only accepted with a before-number, an after-number, the same workload, and
  the counter it came from.** Unmeasured claims are written as hypotheses, not results.

## Task 1 — Build the harness and take the baseline on Windows

**Goal:** a repeatable A/B rig, and a recorded "before".

- [x] **1.1** Sync the current source to `E:\dev\projects\iso-recorder` (that copy is not a git
      checkout — copy files over, don't clone) and build + install with the recipe above.
- [x] **1.2** Record the workload as a fixture: the scene collection, canvas 1920×1080, output
      1920×1080, 60 fps, NVENC H.264, 6000 kbps, NV12, and **which sources are armed**. Write it
      into `## Measurement` below so the run can be reproduced. The plugin has never written its
      own settings here, so this is a fresh configuration — set the base path, pick the encoder,
      tick the sources by hand once. *(Armed: `Mist (veadotube)`, `Browser`, `DriftMedia`.)*
- [ ] **1.3** Make the workload deterministic: replace live app sources with looped local video
      of the same resolution and frame rate where possible, and record the file hash. The
      veadotube source must be **running** for the green test; for the cost baseline, prefer a
      deterministic stand-in of the same size. *(not done — the runs used the live sources; no looped stand-in or file hash was recorded.)*
- [ ] **1.4** Stand up a **local** RTMP ingest on the box (MediaMTX is a single binary) and point
      the profile at `rtmp://127.0.0.1:1935/live`. No public ingest in an A/B. *(not done — no local-ingest setup is recorded.)*
- [ ] **1.5** Fix the machine state: highest power plan, plugged in, background apps and cloud
      sync closed, driver version recorded, HAGS/HDR/VRR constant across both arms. *(not done — machine state was not pinned or recorded.)*
- [ ] **1.6** Run the baseline: ≥5 runs of "arm the sources, stream 10 minutes, stop", first 60 s
      discarded. Per run record, from `%APPDATA%\obs-studio\logs`: *(not done as written — the pre-fix build could not shut down cleanly, so it emitted no `== Profiler Results ==` block; the before figures in `## Measurement` are derived from recorded file sizes and frame geometry instead.)*
      - `Number of lagged frames due to rendering lag/stalls: L (P%)`
      - `Video stopped, number of skipped frames due to encoding lag: K/T (P%)`
      - `Total frames output` / `Total drawn frames`
      - the `== Profiler Results ==` block — median, p99 and max for `obs_graphics_thread`,
        `output_frame`, `render_main_texture`, `render_convert_texture`, `stage_output_texture`,
        `download_frame`, and each `encode(...)`.
      - `%APPDATA%\obs-studio\profiler_data\<log>.csv.gz` is the same data in machine form.
- [ ] **1.7** Capture a **whole-process** view for the same runs with PresentMon, elevated:
      `PresentMon.exe --process_name obs64.exe --output_file base_<n>.csv --track_gpu_video`.
      Record `MsGPUBusy` (median, p99) and `VideoBusy`. *(not done — no PresentMon capture exists.)*
- [ ] **1.8** Check for the silent CPU fallback: grep the log for the warning emitted at
      `source-recorder.cpp:120-122`, and record which sources fell back. *(not done — no baseline log was grepped; the verified arm ran NVENC.)*
- [x] **1.9** Write the results into `## Measurement`. If any source is on `obs_x264`, say so at
      the top — it changes what the fixes are for. *(No source was on `obs_x264`.)*

**Verify:** the numbers are in this file, with the log they came from named, and a second run
reproduces them within the recorded spread.

## Task 2 — Green underlay

- [x] **2.1** `src/source-recorder.hpp`: add `obs_source_t *background_ = nullptr;`.
- [x] **2.2** `src/source-recorder.cpp`: file-local constants for the background channel, the
      source channel and the background colour (as the `0xAARRGGBB` int `0xFF00FF00`).
- [x] **2.3** In `start()`'s visual branch, after `obs_view_add2`: create the background with
      `obs_get_latest_input_type_id("color_source")`, set `color`/`width`/`height` to the same
      even-ified `w`/`h` used for `ovi.base_*`, and `obs_view_set_source(view_, kBackgroundChannel, background_)`.
      Move the real source to `kSourceChannel`.
- [x] **2.4** `setSource()` (L289-302) re-points `kSourceChannel`, not `0`.
- [x] **2.5** Release the background in `stop()` immediately after `obs_view_destroy` (L274-279).
      A module unload must leave no orphaned source.
- [x] **2.6** Leave the audio-only branch (L62-88) untouched.
- [ ] **2.7** `README.md`: what the green is for, that it is always on, and that 4:2:0 makes soft
      edges — the format, not a defect. *(not done — `README.md` is out of scope for this docs-only pass and carries no green section.)*

**Verify (manual, on Windows):** with veadotube running and its Composite mode set to
`Premultiplied Alpha`, record the source and confirm a frame at 100% zoom shows pure `0,255,0`
where the avatar is transparent, and that an opaque source (a screen capture) is unchanged. Add a
frame grab to `## Measurement`.

## Task 3 — Spout guard

The underlay is inert while the Spout source forces alpha to 1. The user must be told.

- [x] **3.1** A file-local predicate: source id is `spout_capture` **and** `compositemode` is
      absent or `1` (`Opaque`). Read the settings with `obs_source_get_settings` and release them.
- [x] **3.2** `src/iso-dock.cpp applySourceState` (L343-374): append a short line to the row
      tooltip.
- [x] **3.3** `onTick` (L554-567): when the manifest reports no failure and an **armed** row trips
      the predicate, `setStatus(...)` — plain words, and say what to change ("Set the Spout
      source's Composite mode to Premultiplied Alpha, or the background cannot show"). The dock
      status is the only place a user will look.
- [ ] **3.4** `README.md`: the Spout source requirement, including that veadotube and VTube Studio
      both deliver premultiplied alpha with a transparent background. *(not done — `README.md` is out of scope for this docs-only pass.)*

**Verify:** with `compositemode` unset, the row warns and the status line appears; set it to
`Premultiplied Alpha` and both go quiet.

## Task 4 — Recording-size cap

- [x] **4.1** `SessionConfig`: add `videoSize` (string: `stream`, `source`, `2160`, `1440`, `1080`,
      `720`, `480`) and the resolved `uint32_t capWidth`, `capHeight`.
- [x] **4.2** `src/iso-settings.cpp`: persist `videoSize` in **both** `loadConfig` and
      `saveConfig`, defaulting to `stream`.
- [x] **4.3** `src/iso-dock.cpp`: a "Recording size" `QComboBox` with *Same as the stream*,
      *Original size*, 2160p, 1440p, 1080p, 720p, 480p. Wire it into the changed slot (L585-589).
- [x] **4.4** `configFromWidgets()` carries `videoSize`; nothing else in the dock learns about
      pixels.
- [x] **4.5** `resolvedConfig()` resolves the cap once per session. `stream` reads the streaming
      encoder's scaled size via `obs_output_get_width/height(obs_frontend_get_streaming_output())`
      — reliable before a stream starts, because that call returns the encoder's own width —
      falling back to `obs_get_video_info().output_*`; `source` means no cap.
- [x] **4.6** `SourceRecorder`'s constructor gains `uint32_t capWidth, capHeight`
      (`source-recorder.hpp:17-19`); pass them from **both** call sites, `iso-session.cpp:339-340`
      (`startEntry`) and the composite path (`startComposite`, L289-317).
- [x] **4.7** Compute the scale against the source size: `s = min(1.0, capW/w, capH/h)`, take
      `even(round(w*s))` and `even(round(h*s))`, and clamp back down so rounding can never make
      the result larger than the cap. Write only `ovi.output_width/output_height`; leave `base_*`
      at `w`/`h`.
- [x] **4.8** Never call `obs_encoder_set_scaled_size` or `obs_encoder_set_gpu_scale_type`.
- [x] **4.9** The background colour source is sized to the **base** dimensions, not the output
      dimensions.

**What this does and does not buy.** It shrinks the convert, the staging/readback and the encode
in proportion to pixel count, and it adds one full-screen scale pass per recorder. It does **not**
shrink the source's own render, which happens at base size — so on a 1080p source capped at
1080p the saving is zero, and the honest thing is to say so. The win is real for 1440p and 4K
sources, and for users who deliberately trade quality for headroom.

**Verify:** with a 4K source capped at 1080p, the row tooltip and the effective-settings log line
both say 1080p, the encoded file is 1920×1080, and `download_frame` and `stage_output_texture`
medians fall in the profiler block.

## Task 5 — Mirror the streaming encoder's settings

- [x] **5.1** A helper beside `streamingBitrateBitsPerSec` (`iso-dock.cpp:160-178`) returning an
      **owned** copy of the streaming encoder's settings —
      `obs_data_create_from_json(obs_data_get_json(obs_encoder_get_settings(enc)))`. The copy is
      required: `SourceRecorder` keeps the object past the call, and a settings object obtained
      from an encoder must not be held.
- [x] **5.2** `SessionConfig` already carries `obs_data_t *videoSettings`; make the dock own it
      (`encoderSettings_`) and release the previous copy when a new one is installed.
- [ ] **5.3** `sessionConfigFromWidgets()` becomes non-const so it can own the copy. *(not done as named — `configFromWidgets()` stayed const; the owned copy is installed by the non-const `refreshSessionConfig()`.)*
- [x] **5.4** Lifetime: `IsoSession::start` addrefs what it is given (`iso-session.cpp:377-381`)
      and `SourceRecorder`'s constructor deep-copies (`source-recorder.cpp:21-25`), so the dock may
      release the old object only *after* installing the new one.
- [x] **5.5** Do not curate keys. libobs applies the target encoder's defaults and then overlays
      the provided settings, which is what makes cross-family selection safe.
- [ ] **5.6** One comment, because this is a non-obvious OBS constraint: copying settings cannot
      create the extra encoder mix, because `maybe_set_up_gpu_rescale` reacts only to explicit API
      calls. *(partly done — a comment explains why a copy is needed, but not that copying cannot create the extra encoder mix.)*

**Consequences to state plainly in the README:** the ISO files now match the stream's bitrate and
rate control, so they are as big as they are configured to be; and the disk-space estimate becomes
accurate, because `bitrateBitsPerSec` now describes the real encoder.

**Verify:** log the effective settings JSON at `LOG_INFO` once per session; with a 6000 kbps
stream, a 60-second 1080p60 file lands near `6000/8 × 60` MB, not at the x264 default's 2500 kbps.

## Task 6 — One shared audio device

The measurement gate: do this only if Task 1 shows the per-source audio path costs measurable
time, or if the source count makes `ceil(N/6)` meaningfully smaller than `N`.

- [x] **Gate — resolved, not triggered; the task is skipped.** The measured per-source
      `audio_thread(...)` costs **0.167 ms median** against a 16.667 ms frame budget, and the
      fixture arms **3** sources, so `ceil(N/6)` = 1 is not meaningfully smaller than `N` = 3.
      Neither side of the gate fires. The per-source audio path stays as it is.

- [ ] **6.1** An `AudioBus` owned by `IsoSession`: one `audio_output_t` opened lazily,
      `AUDIO_FORMAT_FLOAT_PLANAR`, the global sample rate, and an `input_callback` that walks the
      active recorders, pulls each one's source audio and writes it into that recorder's mixer
      index, returning true when there is nothing (silence — never false, which would starve every
      mixer for that tick).
- [ ] **6.2** Pull with `obs_source_get_audio_mix` and `obs_source_get_audio_timestamp`; keep the
      existing composite (`OBS_SOURCE_COMPOSITE`) child-summing and its clamping.
- [ ] **6.3** Assign `mixer_idx` at arm time, reuse freed indices on disarm, cap at 6. Opening a
      second device when the count crosses 6 is acceptable; reopening on every change is not.
- [ ] **6.4** Each recorder keeps its own
      `obs_audio_encoder_create("ffmpeg_aac", name, settings, idx, nullptr)` bound with
      `obs_encoder_set_audio(enc, bus)` — one encoder per output is still required.
- [ ] **6.5** Keep the WAV path for audio-only sources exactly as it is (24-bit PCM, unchanged by
      user decision). The bus serves only the AAC track a video source's MOV needs.
- [ ] **6.6** The bus is opened on session start and closed on session stop; `stop()` must not
      leave a thread behind.

*(6.1–6.6 not built — the gate above did not trigger.)*

**Verify:** thread count before and after with 4 armed sources; arm and disarm repeatedly and
confirm the count returns to baseline at stop.

## Task 7 — Per-source frame rate

- [x] **7.1** Add a per-row rate option only if Task 1 shows **encode** time, not render time,
      dominates. Otherwise record the finding and skip. *(Resolved: encode does not dominate —
      encoding lag is 0.0–0.1%, and the GPU encode thread sits at 0.971 ms median against 1.742 ms
      for the whole graphics thread. Skipped.)*
- [ ] **7.2** Call `obs_encoder_set_frame_rate_divisor` on the video encoder before it is
      initialised, and say in the comment that it changes only what reaches the encoder: the mix
      still renders at canvas FPS, because `obs_init_video_mix` forces the main canvas FPS onto
      every auxiliary mix. *(not built — 7.1 found encode does not dominate.)*
- [ ] **7.3** If several recorders share a divisor, keep them aligned so their first frames
      coincide. *(not built — 7.1 found encode does not dominate.)*

**Verify:** the profiler's `encode(...)` median falls with the divisor while `render_main_texture`
does not.

## Task 8 — The render pass (measurement-gated)

This is the one place where the honest answer may be "impossible". Order of work:

- [x] **8.1** Confirm from Task 1's numbers what a per-recorder render actually costs. If N renders
      are not a measurable share of frame time, stop and write that down. *(Resolved: `render_main_texture`
      is 0.018 ms median and `render_video` 0.11 ms median against a 16.667 ms budget, so N renders
      are not a measurable share. Work stopped here, as the step directs.)*
- [ ] **8.2** Try the cheap structural wins first and measure each: create ISO views only for
      sources that need them; skip the composite recorder's canvas duplicate unless it is ticked;
      do not create a view for a source whose picture is empty. *(not attempted — 8.1 says stop.)*
- [ ] **8.3** Only then attempt the proxy: a private `OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW`
      source owning a `gs_texrender`, rendering its target once per frame (invalidated in
      `video_tick`) and blitting thereafter, following `gpu-delay.c`'s colour-space handling rather
      than branch-output's plain `GS_BGRA`. It pays off only when the source is also on the main
      canvas — the canvas has to be rendering the proxy for the duplicate render to disappear, and
      the plugin must not silently restructure the user's scenes. If that cannot be done without
      touching the user's scene, write it up as rejected, with the evidence. *(rejected — see `docs/adr/0003-the-render-pass-is-not-being-built.md`.)*
- [x] **8.4** Record whichever way it goes in `docs/adr/0003-*.md`.

**Verify:** a before/after pair of `render_main_texture` and `output_frame` medians, or a written
finding that N renders is irreducible with the supported API surface.

## Task 9 — Make the cost visible

- [ ] **9.1** If Task 1 found an `obs_x264` fallback, surface it on the row — a recording running
      on the CPU is the single most important thing a user can be told. *(not done — the verified
      run used NVENC; the silent `obs_x264` fallback still logs only and is not surfaced on the row.)*
- [ ] **9.2** A grey note under the size combo showing the effective recording size, so "Same as
      the stream" is never a mystery. *(not done — no effective-size note is present under the size combo.)*
- [ ] **9.3** If Task 1 shows the armed count is a real risk on this hardware, make the existing
      `kEncoderWarnThreshold` warning name the count and the measured ceiling. *(not done — the dock
      still warns without naming the count or the measured ceiling.)*
- [x] **9.4** Do nothing that Task 1 did not justify.

## Task 10 — Measure again on Windows

- [ ] **10.1** Repeat Task 1's exact protocol on the fixed build, same machine state, same
      fixture, arms **interleaved** with the baseline rather than run in two blocks. *(partly — the
      fixed build ran on the same fixture, but the arms are separate blocks and the before arm has
      no profiler block.)*
- [x] **10.2** Report per arm: rendering-lag % and count; encoding-lag % and count; dropped frames
      %; profiler medians and p99 for `obs_graphics_thread`, `output_frame`,
      `render_main_texture`, `render_convert_texture`, `stage_output_texture`, `download_frame`;
      PresentMon `MsGPUBusy` median and p99; thread count; and, for the cap, the encoded
      resolution.
- [ ] **10.3** Accept a fix only when the headline counter improves with **non-overlapping
      spreads** and neither encoding lag nor dropped frames regresses. A fix that trades render lag
      for encoder starvation is not a fix. *(not established — no non-overlapping spreads are
      available for the before arm.)*
- [ ] **10.4** Discard, do not explain, any run whose GPU clocks, temperatures or throttle reasons
      differ from its pair. *(not recorded — no clock/temperature pairing was captured.)*
- [x] **10.5** Write both arms into `## Measurement` side by side, with the delta.

## Task 11 — Correct the record

- [x] **11.1** `docs/spec.md`: the cost ceiling, the green background, the size cap, and the
      encoder-settings behaviour — including that "same as the stream" was, until now, only the
      encoder **id**.
- [x] **11.2** `docs/adr/0002-the-background-is-a-source-because-a-view-has-no-clear.md`.
- [x] **11.3** `docs/adr/0003-*` from Task 8.
- [ ] **11.4** `README.md`: the green section with the Spout Composite-mode requirement; the
      recording-size line; the honest note that the cap shrinks convert and encode but not the
      per-source render; the measured ceiling table refreshed with Task 10's numbers. Also fix the
      stale version in `packaging/windows/README.txt` (it says 0.1.0; the project is 0.3.0).
      *(partly done — `packaging/windows/README.txt` version corrected to 0.3.0; `README.md` is out of scope for this docs-only pass.)*
- [x] **11.5** `docs/plan.md`: mark the tasks this supersedes.
- [x] **11.6** Re-run `ctest --test-dir build-mac --output-on-failure` and `build-mac.sh`.

## Measurement

*(Populated by Task 1 and Task 10. One row per arm. A number that was never measured reads `n/a`.)*

| Arm | Run | Res | Armed | Render lag % | Encode lag % | Dropped % | graphics-thread med | output_frame med | download_frame med | MsGPUBusy med/p99 | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| before | 323,190,804 B / 261 s | 1920×1010 / 800×600 / 640×360 | 3 | n/a | n/a | n/a | n/a | n/a | n/a | n/a | Raw `jim_nvenc`, obs-nvenc defaults `cbr`/10000 kbps with the configured `bitrate=2500` never binding; ~10 Mbps per source. |
| after | 93,691,623 B / 124 s · 104,167,044 B / 139 s | 854×450 / 640×480 / 640×360 (cap 480; stream 1920×1080) | 3 | n/a | 0.0% verified · 0.1% regression | n/a | 1.742 ms | 0.713 ms | n/a | n/a | `obs_nvenc_h264_tex` with the stream's settings mirrored — CBR 6000 kbps, audio 160 kbps. Encode lag 1/8718, 1/8722, 1/8722; regression 8/8331, 8/8332, 7/8332 with the stream also 0.1% (machine-wide, not ISO-specific). |

Bitrate: ~10 Mbps per source before, 6.0 Mbps after — **39% less**, exactly the mirrored 6000 kbps CBR.
The green underlay is proven end-to-end: the veadotube recording averages RGB `00ff01` at t=5/30/60/90/115 s.

Profiler medians (after arm, `== Profiler Results ==`): `obs_graphics_thread` 1.742 ms,
`output_frame` 0.713 ms, `render_video` 0.11 ms, `render_main_texture` 0.018 ms,
`render_convert_texture` 0.01 ms, `output_gpu_encoders` 0.08 ms, `render_displays` 0.485 ms,
`obs_gpu_encode_thread` 0.971 ms, `gpu_encode_frame` 0.939 ms, `audio_thread(Mist (veadotube))`
0.167 ms, `obs_init_module(iso-recorder.dll)` 26.273 ms — all against a 16.667 ms frame budget.

The `before` arm has no profiler row because the pre-fix build could never shut down cleanly and so never emitted a `== Profiler Results ==` block; the before-side figures are derived from the recorded file sizes and frame geometry, not from a profiler.

### Known baselines already on disk

- **Mac, 2026-09-14, 2h47m stream, 1080p60, VideoToolbox H.264 6000 kbps** — `0.2%` rendering lag
  (986/603227), `0.0%` dropped, no encoding-lag line. Profiler medians: graphics thread
  **6.402 ms**, `output_frame` **2.438**, `render_displays` **3.734**, `render_video` **1.515**,
  `stage_output_texture` **1.374**, `download_frame` **0.729**, `render_main_texture` **0.156**.
  Whether the plugin was recording is unknown — an older build logged nothing at session start — so
  this is a stream-only reference point, not a baseline.
- **Windows, 2026-09-13, 30-minute drift run** — `0.1%` rendering lag (64), `0.0%` encoding lag
  (14/108000). Matches the README drift test.
- **Windows, 2026-09-15, 30-minute drift run on the fixed build** — three armed visuals
  (`Mist (veadotube)`, `DriftMedia`, `Browser`), `obs_nvenc_h264_tex`, obs64 exited cleanly, all
  three `complete`. Encoding lag `3/107780`, `3/107790`, `3/107794` (0.0%), stream `0.1%`
  (82/107830). `DriftMedia.mov` read from its trailing index: video h264 **107,775 frames /
  1796.2500 s** and audio AAC **84,199 packets / 1796.2240 s**, both from `0` — **26 ms apart,
  inside one AAC packet**. The collection had no audio-only source, so the mic-against-picture
  run is still owed (see README).
- **Windows, 2026-09-13 09:26–11:35** — a 4.5 MB log with no stream in it at all (the only output
  was a 612-frame browser output). Nothing to learn about lag.

## Open questions

1. **When did the stream actually lag?** No log on either machine covers a bad session, and neither
   machine has ever run this plugin's recording path. Task 1 exists to create the baseline the
   user's complaint could not provide.
2. **Is the composite recorder (`00_Session.mov`) actually used?** It is a full extra encode of the
   canvas; if it is off in practice, it should not be in the default fixture.
