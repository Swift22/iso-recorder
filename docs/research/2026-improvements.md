# ISO Recorder — 2026 improvements and best practices

Status: research note · 2026-09-15
Scope: OBS Studio plugin `iso-recorder` (`docs/spec.md` is the design authority; `docs/adr/0001` is accepted).
Method: read against a local OBS Studio checkout as primary truth, cross-checked with vendor and upstream primary sources. Every recommendation names what to change, why, a primary-source citation, the expected benefit, and a priority for *this* plugin.

Citations of the form `path:line` are in `/Users/misty/Developer/obs-studio` unless the path starts with `src/`, which is this repo. OBS API names are linked to the header that declares them.

---

## Version verification

| Component | Exact version string found | Where |
|---|---|---|
| OBS Studio clone (primary truth) | **32.2.2**, HEAD `ba2f32bdf791005443988a4955e963663e16b1ed`, commit date 2026-08-14 | `git describe` / `git log -1` in `/Users/misty/Developer/obs-studio` |
| OBS bundled NVIDIA SDK | **13** — release notes state this raises the minimum driver to **570** | OBS 32.2 release notes |
| NVENC driver ceiling (consumer) | **12** concurrent sessions (SDK 13.x); was **8** (SDK 12.2, driver 551.76+, Jan 2024), **5** (SDK 12.1, driver 531.61+, Mar 2023), **3** before | NVIDIA NVENC Application Note (see “Unverified” — figure re-read, not re-fetched) |
| obs-plugintemplate (upstream template) | current trunk: VS 2022, Xcode 16 (CI `macos-15`, Xcode 16.1), CMake 3.30.5 / 3.28.3 | obs-plugintemplate repo |
| Exeldro `obs-source-record` (closest competitor) | **v0.4.6** (Apr 2025) | upstream repo |
| This plugin | **0.3.0** | `CMakeLists.txt:2` |

Everything below was verified against **OBS 32.2.2**. Because `mov_output`, `obs_view_*`, encoder caps and the manifest reader are ABI-stable across 31/32, the findings hold for OBS 31 and 32; anything that depends on 32.2-only behaviour is called out.

---

## Executive summary

The plugin’s core mechanism is sound and matches what OBS itself and Source Record do. The findings below are mostly hardening, one real correctness risk (a cross-thread stop on `source_remove`), and packaging gaps. The highest-value items are crash-safe file writes, honest encoder-cap messaging backed by dropped-frame detection, and shipping the plugin like a modern OBS plugin (manifest + CI + macOS signing).

**Top 5, ranked**

1. **Make the manifest crash-safe** — atomic temp+fsync+rename, and fix the WAV header so a force-kill leaves a readable file. `src/iso-session.cpp:79-88`, `src/wav-writer.cpp:56-109`. — High
2. **Detect and surface silent frame loss** — the measured ceiling (4 clean / 6 partial / 8 half) loses frames *silently*; count output frames vs expected render frames and mark the entry `aborted` with a plain reason instead of `complete`. — High
3. **Marshal `source_remove` onto the OBS main thread** — the recorder stop touches outputs/encoders, and that signal is not guaranteed to arrive on the main thread. `obs_queue_task(OBS_TASK_UI, …)`. — High
4. **Add the plugin manifest and CI** — `data/manifest.json` (read by `obs_module_load_metadata`) plus a `buildspec.json`-driven GitHub Actions build and macOS sign/notarize. No CI exists today. — High
5. **Correct the driver-cap documentation and warning copy** — consumer GeForce is capped at 12 sessions in 2026 (not 3); the real limit on an RTX 2050 is *throughput*, so the warning should be about sustained encodes, and the driver floor is 570 for OBS 32.2. — Medium

---

## 1. Concurrent NVENC sessions (highest risk)

### 1.1 Each recorder opens its own NVENC session — confirmed

Every `obs_video_encoder_create("obs_nvenc_h264_tex", …)` reaches `nvenc_create_base` → `init_session`, which issues exactly one `NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS` + `nvEncOpenEncodeSessionEx` per encoder instance (`plugins/obs-nvenc/nvenc.c:129-149`). There is **no session pooling or sharing**: N recorders + the stream = N + 1 NVENC sessions. The id is registered at `plugins/obs-nvenc/nvenc.c:1404` (`h264_nvenc_info`, `.id = "obs_nvenc_h264_tex"`), with `.caps = OBS_ENCODER_CAP_PASS_TEXTURE | DYN_BITRATE | MULTITRACK_DYN_BITRATE | ROI`.

- **What to change:** nothing in the code path — this is correct and unavoidable. But say it in the README: “one NVENC session per armed visual source, plus one for the stream.”
- **Why:** the session count is the thing a user can reason about; the plugin currently frames the limit only as “frames dropped”.
- **Citation:** `plugins/obs-nvenc/nvenc.c:129-149`, `plugins/obs-nvenc/nvenc.c:1404`.
- **Benefit:** accurate mental model; the plugin’s own warning becomes trustworthy.
- **Priority:** Medium (documentation only).

### 1.2 Does NVIDIA split-frame encode (one session, N encodes) help? Not for this plugin

OBS exposes NVENC split encoding only where the encoder supports it *and* the GPU has more than one engine. The property is gated in `plugins/obs-nvenc/nvenc-properties.c:236-256`:

```
is_codec_supported(CODEC_AV1) && caps->engines > 1 &&
!has_broken_split_encoding() && (codec == HEVC || codec == AV1)
```

`has_broken_split_encoding()` is `driver_version_major < 555` (`plugins/obs-nvenc/nvenc-helpers.c:255`), and `params->splitEncodeMode` is set at `plugins/obs-nvenc/nvenc.c:171`.

- **Why it does not apply:** (a) split encode is disabled for **H.264**, which is this plugin’s default and the only codec guaranteed to be `mov_output`-muxable; (b) it requires **more than one NVENC engine**, and an RTX 2050 (Ampere GA107) has a single engine. NVIDIA’s multi-encode-in-one-session feature is an Ada/multi-engine capability, not a way to multiply a laptop’s encode capacity.
- **Citation:** `plugins/obs-nvenc/nvenc-properties.c:236-256`, `plugins/obs-nvenc/nvenc-helpers.c:255`, `plugins/obs-nvenc/nvenc.c:171`.
- **Benefit:** avoids a tempting dead end.
- **Priority:** High confidence to **not do**.

### 1.3 Driver session limits in 2026, and what actually binds

NVIDIA’s NVENC Application Note defines the session count for **non-qualified** (consumer GeForce) GPUs and raises it over time; **qualified** (RTX/Quadro pro) cards have no artificial cap. The trajectory: ~3 sessions before 2023 → **5** with driver 531.61+ (SDK 12.1, Apr 2023) → **8** with driver 551.76+ (SDK 12.2, Jan 2024) → **12** per system in the SDK 13.x application note current in 2026. OBS 32.2.2 bundles SDK 13 and therefore requires **driver ≥ 570**.

What binds this plugin is **not** the session cap — 4 sessions is far below 12. It is **GPU encode throughput**. The plugin’s own measurements (README) on an RTX 2050, OBS 32.2.2, NVENC, 1080p60:

| Concurrent 1080p60 encodes | Result |
|---|---|
| 4 | clean |
| 6 | five clean; one lost ~a third of its frames |
| 8 | every file lost about half its frames |

The loss is **silent**: `session.json` still says `complete`. The threshold is `kEncoderWarnThreshold = 3` (`src/iso-session.hpp:19`) — one below the 4 sustained, leaving room for the stream.

- **What to change:**
  1. Update README/spec wording to “GeForce NVENC is capped at 12 sessions in 2026 (OBS 32.2 needs driver ≥ 570); the practical limit is encode throughput, not session count.”
  2. Keep `kEncoderWarnThreshold` as a *warning only* (the spec says “never refuse”), but stop presenting 3 as a driver limit.
- **Why:** the current framing (session count as the risk) misleads; the measured table is the real evidence.
- **Citation:** NVIDIA NVENC Application Note (https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-application-note/); OBS 32.2 release notes (driver 570 / SDK 13); `src/iso-session.hpp:19`; README “Simultaneous 1080p60 hardware encodes”.
- **Benefit:** users understand the real ceiling; documentation stops implying a hard cap that no longer exists.
- **Priority:** Medium.
- **Verification note:** the exact “12” figure was assembled from the application note and secondary sources; re-fetch the note before publishing a number in user-facing copy. See “Unverified”.

### 1.4 How OBS itself does many simultaneous encodes

OBS’s own many-encode case — Twitch Enhanced Broadcasting / multitrack video — sends N renditions from **N encoders**, one session each (OBS 32.2 added dynamic bitrate to that path). There is no single-session multiplexing in `obs-nvenc`. So the plugin is structurally the same as OBS’s own approach; there is no “OBS pattern” it is missing.

- **Citation:** `plugins/obs-nvenc/` (no session-sharing code); OBS 32.2 release notes (multitrack dynamic bitrate).
- **Priority:** informational.

---

## 2. OBS 31/32 API changes

### 2.1 The plugin uses no deprecated APIs

A sweep for the APIs the prompt called out found **no deprecated usage**: no `obs_properties_add_button` (deprecated in 32.2), no `obs_properties_add_int` misuse, no legacy output calls. The only “flagged” API is `obs_source_create_private` (`src/source-recorder.cpp:148`), which is **not deprecated** — it is the intended constructor for a source that must not be enumerated or saved (`libobs/obs.h:1048`; `docs/sphinx/reference-sources.rst:889`). OBS’s docs include an author note calling the situation that makes it necessary a design flaw, not the function itself.

- **Do:** keep using `obs_source_create_private` for the green underlay.
- **Priority:** informational.

### 2.2 `obs_view_set_source` already activates; the manual `inc_showing` is redundant but balanced

`obs_view_set_source` takes its own reference, calls `obs_source_activate(source, view->type)` on the new source and `obs_source_deactivate(prev, view->type)` + release on the old (`libobs/obs-view.c:91`). A view created by `obs_view_create()` has `view->type = AUX_VIEW` (`libobs/obs-view.c:29-31,41`), and `obs_source_inc_showing` is itself `obs_source_activate(source, AUX_VIEW)` (`libobs/obs-source.c`). `obs_view_free` deactivates and releases every channel (`libobs/obs-view.c:46-64`).

The plugin additionally calls `obs_source_inc_showing(bg)` and `obs_source_inc_showing(source)` (`src/source-recorder.cpp:155,157`) and balances them on stop (`:334,338,359`).

- **What to change:** nothing required. The manual pairs are the **same AUX_VIEW counter** as the view’s activation, so they are redundant — but they are balanced, so they are harmless. If you remove them, remove the matching `dec_showing` too and confirm the view keeps the source active.
- **Why:** a future reader will wonder whether they are a leak. Document the reason or drop them in one deliberate commit.
- **Citation:** `libobs/obs-view.c:29-31,41,46-64,91`; `libobs/obs.h:965,977`; `src/source-recorder.cpp:155,157,334,338,359`.
- **Priority:** Low.

### 2.3 Encoder/GPU scaling: the view-based scaling is the right call; encoder scaling is optional

The plugin scales by setting `ovi.output` to the capped size in `obs_view_add2` (`src/source-recorder.cpp:139-140`) and never calls `obs_encoder_set_scaled_size` or `obs_encoder_set_gpu_scale_type`. Source Record instead keeps the view at source size and calls those two encoder functions.

Two facts matter here:

- `OBS_ENCODER_CAP_SCALING` (`libobs/obs-encoder.h:40`) exists, but **`obs_nvenc_h264_tex` does not advertise it** (`plugins/obs-nvenc/nvenc.c:1404-1406`). Moreover an encoder that sets both `PASS_TEXTURE` and `SCALING` is **rejected at registration** (“Texture encoders cannot self-scale” — `libobs/obs-module.c:1135`). `SCALING` is for non-texture/self-scaling encoders.
- GPU rescaling via `obs_encoder_set_gpu_scale_type` is implemented through core video mixes (`libobs/obs-encoder.c:227+`, setter at `libobs/obs-encoder.c:966`), is a no-op while the encoder is active, and defaults to `OBS_SCALE_DISABLE` (`libobs/obs.h:2290-2295`).

- **What to change:** keep view-based scaling. Optionally adopt encoder-side scaling **only** as a refactor, behind a runtime symbol lookup (the Source Record pattern) so it remains a no-op on builds without it. Do **not** present it as a correctness fix.
- **Why:** the current approach works and carries OBS’s timestamp clock; encoder scaling offers no measured benefit here and is not the mechanism NVENC prefers (`SCALING` is unavailable to texture encoders by design).
- **Citation:** `src/source-recorder.cpp:139-140`; `libobs/obs-encoder.c:227,966`; `libobs/obs-encoder.h:40`; `libobs/obs-module.c:1135`; `plugins/obs-nvenc/nvenc.c:1404-1406`.
- **Priority:** Low (optional refactor, not a fix).

### 2.4 Ownership of `obs_encoder_get_settings` — already correct

`obs_encoder_get_settings` **addrefs and returns the encoder-owned object**, so the caller must release it (`libobs/obs.h:2410`, `libobs/obs-encoder.c`); the same is true of `obs_output_get_settings` (`libobs/obs.h:1981`). `resolveConfig()` reads the live stream encoder settings and releases them on both paths (`src/iso-dock.cpp:699-716`), copying via `obs_data_get_json` → `obs_data_create_from_json`. The in-code note (`src/iso-dock.cpp:693-697`) already explains why the whole blob is forwarded rather than cherry-picked keys (obs-nvenc needs `rate_control` alongside `bitrate`).

- **What to change:** nothing. Keep the note; it prevents a real regression.
- **Priority:** informational.

### 2.5 `obs_queue_task`, `OBS_ENCODER_CAP_*`, `mov_output` codecs

- `obs_queue_task`/`obs_in_task_thread` and `OBS_TASK_UI/GRAPHICS/AUDIO/DESTROY` exist (`libobs/obs.h:926-933`) but the plugin calls none of them. See §5.1 — that is a gap for one signal handler.
- `OBS_ENCODER_CAP_*` full set: `DEPRECATED(1<<0)`, `PASS_TEXTURE`, `DYN_BITRATE`, `INTERNAL`, `ROI`, `SCALING`, `MULTITRACK_DYN_BITRATE` (`libobs/obs-encoder.h:35-41`). Useful if you later gate features on caps.
- `mov_output` accepts **H.264, HEVC, ProRes only** (`plugins/obs-outputs/mp4-output.c:610`, `encoded_video_codecs = "h264;hevc;prores"`); the base `mp4_output_info` lists `h264;hevc;av1` but that is the plain MP4 output, not `mov_output`. The plugin’s `muxableEncoderId()` already allows exactly `h264/hevc/prores` (`src/iso-dock.cpp:41-44`), and audio `ffmpeg_aac` → `aac`, which `mov_output` accepts. **Do not add AV1.**
- **Citation:** `libobs/obs.h:926-933`; `libobs/obs-encoder.h:35-41`; `plugins/obs-outputs/mp4-output.c:596,610`; `src/iso-dock.cpp:41-44`.
- **Priority:** Low (keep as-is).

---

## 3. Muxing and durability

### 3.1 Hybrid MOV already writes incrementally and survives a crash

OBS 32.0 added Hybrid MOV (`[derrod]`) and made Hybrid MP4/MOV the default for new profiles. `mp4_output_start` logs “Writing Hybrid MP4/MOV file”; the muxer writes `ftyp` with the fragmented flag and emits `moof`/`mdat` fragments as it goes, then `mp4_mux_finalise` flushes the final fragment and — unless finalisation is skipped — writes a full non-fragmented `moov` (a “soft remux”) and overwrites the header (`plugins/obs-outputs/mp4-mux.c`; entry points `mp4_write_ftyp(..., fragmented)`, `mp4_write_moof`, `mp4_mux_finalise`). **So while recording, the on-disk file is a fragmented MP4 that a crash leaves playable; finalize converts it to a compact MOV.**

This already satisfies spec user-story 24 (“files survive OBS crashing”) for video. No change needed to container choice.

- **Citation:** `plugins/obs-outputs/mp4-output.c` (`mp4_output_start`), `plugins/obs-outputs/mp4-mux.c`; OBS 32.0 release notes.
- **Priority:** informational.

### 3.2 Crash-safe, atomic manifest writes — **do this**

`writeFile()` is `fopen(..., "wb")` → `fwrite` → `fclose` (`src/iso-session.cpp:79-88`). It truncates in place, so a crash *during* a `session.json`/`README.txt` rewrite leaves a truncated or torn file — the editor’s manifest is lost even though the MOVs survived. This is the one place the plugin can lose data that the container already protects.

- **What to change:** write to `session.json.tmp`, `fflush` + `fsync`, `rename()` over the target (same directory). Do the same for `README.txt`. On read, if the file is missing/corrupt, fall back to the previous good copy.
- **Why:** the design already trusts a half-written manifest to mean “crashed” (spec: entries left `recording`); a torn file breaks that contract entirely.
- **Citation:** `src/iso-session.cpp:79-88`, `src/iso-session.cpp:539,582-583`.
- **Benefit:** a force-quit at any instant still leaves a parseable manifest.
- **Priority:** High.

### 3.3 Crash-safe WAV — **do this**

`WavWriter` writes a header with data size 0 at open, then seeks back and rewrites the header on `close()` (`src/wav-writer.cpp:56-109`). A hard kill leaves a WAV whose header claims zero data — unreadable or truncated in most editors. This is exactly the open item in spec “Further Notes” #3.

- **What to change:** one of (a) write a WAV header with a large reserved data size and update only when closing, or (b) write RF64/`wavl` chunks, or (c) on session start, or (d) accept it and make the manifest say the WAV was cut short. A pragmatic middle path: keep the header patchable and additionally emit `session.json` entries as `recording`, which the spec already does.
- **Why:** the editor story (spec user-story 20/24) depends on WAV being usable after a crash; today only the MOV path is safe.
- **Citation:** `src/wav-writer.cpp:56-109`; `docs/spec.md` “Further Notes” #3.
- **Benefit:** the WAV path matches the MOV path’s crash guarantee.
- **Priority:** High.

### 3.4 Optional durability knobs

`mov_output`’s `muxer_settings` are parsed by `parse_custom_options` (`plugins/obs-outputs/mp4-output.c:240+`) and include `skip_soft_remux`, `write_encoder_info`, `use_metadata_tags`, `use_negative_cts`, `buffer_size` (MB), `chunk_size` (MB), and `bpm`. `MP4_SKIP_FINALISATION` (“Skip soft-remux and leave file in fragmented state”, `plugins/obs-outputs/mp4-mux.h:37`) makes the file *always* fragmented.

- **What to change (optional):** expose a “crash-safe mode” that appends `skip_soft_remux=1` to the output settings, and/or set `buffer_size`/`chunk_size` explicitly. Consider `bpm=1` for packet-level crash telemetry (`shared/bpm/bpm.c`, `obs_output_add_packet_callback(bpm_inject)`).
- **Why:** always-fragmented is maximally crash-tolerant at the cost of size/compatibility; letting the user choose is honest.
- **Citation:** `plugins/obs-outputs/mp4-output.c:240+`, `plugins/obs-outputs/mp4-mux.h:37`, `shared/bpm/bpm.c`.
- **Priority:** Low (the default Hybrid path is already crash-tolerant; this is a preference).

---

## 4. Plugin packaging and modern practice

### 4.1 Add `data/manifest.json` — **do this**

OBS reads `<data_path>/manifest.json` at module load via `obs_module_load_metadata` (`libobs/obs-module.c:99-138`), populating `struct obs_module_metadata` (`libobs/obs-internal.h:169`). Expected keys: `display_name`, `id`, `version`, `os_arch`, `name`, `description`, `long_description`, `urls{repository,website,support}`, `has_banner`, `has_icon`. The plugin currently ships none.

- **What to change:** add `data/manifest.json` with those keys and install it next to the binary.
- **Why:** makes the plugin show correctly in OBS’s plugin list and third-party registries; required by modern expectations.
- **Citation:** `libobs/obs-module.c:99-138`, `libobs/obs-internal.h:169`.
- **Priority:** High.

### 4.2 Adopt `buildspec.json` + CI + macOS signing/notarization — **do this**

The repo has `.github/` with issue/PR templates only — **no workflows**. The modern OBS plugin template is driven by `buildspec.json` (name/displayName/version, `platformConfig.macos.bundleId`, prebuilt dep downloads into `.deps`) and ships CI (`.github/workflows/build-project.yaml` plus reusable actions `setup-macos-codesigning`, `build-plugin`, `package-plugin`) that produce `.pkg`/`.deb`/`.tar.xz` **and sign + notarize macOS builds** (`security`/`codesign`, `productsign`, `xcrun notarytool … --wait`, `xcrun stapler staple`; secrets `MACOS_SIGNING_*`, `MACOS_NOTARIZATION_USERNAME/PASSWORD`; notarize on version tags only; target `macos-universal`).

- **What to change:** add `buildspec.json`, a macOS build workflow with universal (arm64+x86_64) output and notarization, and a Windows build/installer workflow. The plugin already has `packaging/windows/make-installer.ps1` to reuse.
- **Why:** OBS 32 forces Intel installs to Apple Silicon, so a non-notarized / Intel-only plugin is a support burden; signing is also what stops Gatekeeper from blocking the `.plugin` bundle.

### 4.3 Keep `obs_module_text` + `data/locale` — already done, extend it

The plugin already ships `data/locale/en-US.ini`. New user-facing strings (the warnings above, the “crash-safe mode” toggle) should go through `obs_module_text`, not hardcoded literals, to stay consistent with the existing localization path.

- **Citation:** `data/locale/en-US.ini`; OBS plugin template convention.
- **Priority:** Medium.

---

## 5. Correctness and robustness patterns

### 5.1 `source_remove` may not arrive on the OBS main thread — **do this**

The plugin connects to the global signal handler: `signal_handler_connect(obs_get_signal_handler(), "source_remove", onSourceRemoveSignal, nullptr)` (`src/plugin-main.cpp`). A source is destroyed/released from a variety of threads (including the graphics/destroy task), so this handler can run **off the main thread**, yet its job is to stop a recorder — touching outputs/encoders and rewriting the manifest. The spec’s stated rule (“All create/start/stop happens on the OBS main thread … `obs_queue_task`”) is also contradicted by the code: `obs_queue_task` appears **nowhere** in `src/`.

- **What to change:** in `onSourceRemoveSignal`, get the source’s name/id and hand the work to the main thread: `obs_queue_task(OBS_TASK_UI, …, false)` (or the frontend’s main-thread task), then stop/finalize the recorder there. Do not call `obs_output_force_stop`/release encoders from the signal handler directly.
- **Why:** releasing an encoder or output off the main thread races the graphics thread; the failure mode is a crash, not a slow frame.
- **Citation:** `src/plugin-main.cpp` (`source_remove` connect); `libobs/obs.h:926-933` (`obs_queue_task`, `OBS_TASK_*`); spec §“The recorder per source”.
- **Benefit:** removes a real crash window when a source is deleted mid-session (spec stories 14, 35).
- **Priority:** High.

### 5.2 Never release an encoder while it is active — already close; keep the invariant explicit

The stop path calls `obs_output_force_stop`, waits on `stopEvent_` (`os_event_timedwait(..., 5000)`), and on timeout calls `obs_output_end_data_capture` before releasing (`src/source-recorder.cpp:294` onward). Source Record’s hard-won rule is the same: **never release an active encoder — swap when idle**. Two hardening points:

- Guard the release with `obs_output_active()` / confirm the `stop` signal fired; if the 5 s wait timed out, finish teardown only after `end_data_capture` has run to completion.
- The spec says “release the encoders from the output’s `stop` signal, never before” — make sure the code actually does that rather than releasing unconditionally after the timed wait.
- **Citation:** `src/source-recorder.cpp:294`; Exeldro `obs-source-record` (`update_encoder` crash fix).
- **Priority:** Medium.

### 5.3 `obs_source_inc_showing` for a not-in-scene source — correct, keep it

The plugin holds a strong ref and `inc_showing` so a source that is not in the active scene still produces frames (spec §“Why a dock plugin and not a filter”). This is the documented purpose of `inc_showing` (`obs_source_activate(source, AUX_VIEW)` in `libobs/obs-source.c`). No change.

- **Priority:** informational.

### 5.4 Green underlay colour handling — verified correct

`0xFF00FF00` is parsed by the colour source via `vec4_from_rgba`, giving **opaque green** (alpha from the top byte), rendered with `OBS_EFFECT_SOLID` (`plugins/image-source/color-source.c`). The lowest-channel ordering (`kBackgroundChannel` 0, `kSourceChannel` 1, `src/source-recorder.cpp:153-154`) is correct for “fill behind a smaller source”. No change.

- **Priority:** informational.

### 5.5 What the competitors get right that this plugin should copy

- **Exeldro `obs-source-record`** (filter-based, v0.4.6): uses its own `obs_view_t` + private `audio_output` exactly like this plugin; **never releases an active encoder**; resolves optional encoder APIs through runtime function pointers so it can run on older OBS; registers **obs-websocket vendor requests**; its UI tells users `.mkv` is the crash-tolerant default. Copy: the encoder swap-when-idle rule, the runtime symbol resolution habit, and websocket vendor requests.
- **Aitum** — closed source; no primary source available to cite.
- **`obs-multi-rtmp`** — not inspected in this pass.
- **Citation:** Exeldro `obs-source-record` upstream source; see “Unverified”.

---

## 6. Anything else a 2026 plugin of this kind should do

### 6.1 obs-websocket vendor requests — consider

OBS’s WebSocket API lets a plugin register its own requests/events. `obs_websocket_register_vendor(name)` **must be called from `obs_module_post_load()`**; then `obs_websocket_vendor_register_request`, `obs_websocket_vendor_unregister_request`, `obs_websocket_vendor_emit_event`, with `obs_websocket_get_api_version`. Useful requests: `arm`/`disarm` a source, `start`/`stop` session, `get status`; and an event when a recording finalizes. Vendors are includable by WebSocket clients via `EventSubscription::Vendors`.

- **What to change:** in `obs_module_post_load`, register a vendor and expose the dock’s arm/start/stop and a status request.
- **Why:** lets stream decks and automation drive ISO recording without GUI clicks, and matches competitor feature sets.
- **Citation:** `lib/obs-websocket-api.h` (obs-websocket upstream).
- **Priority:** Medium (feature, not correctness). Note the plugin’s CMake does not currently link obs-websocket — the header is a light, optional dependency; see “Unverified”.

### 6.2 `source_profiler` instrumentation — optional

libobs ships a source profiler: `source_profiler_enable(bool)`, `source_profiler_gpu_enable(bool)`, `source_profiler_get_result()` (caller frees), `source_profiler_fill_result()`, and `profiler_result_t { tick_avg/max, render_avg/max, render_gpu_avg/max, render_sum, async_input/rendered, … }` (`libobs/util/source-profiler.h`).

- **What to change:** optionally sample the armed sources’ render cost into the session log so a user can see *why* frames dropped (a heavy source vs. too many encodes).
- **Why:** the current ceiling is stated as “N sources”; the real driver is per-source render cost plus encode throughput.
- **Citation:** `libobs/util/source-profiler.h`, `libobs/util/source-profiler.c`.
- **Priority:** Low.

### 6.3 Crash-safe manifest is the highest-leverage “anything else”

Covered as §3.2 — repeat it here because it is the item most likely to save an editor’s session.

---

## Do this

1. Atomic, fsynced, temp+rename writes for `session.json` and `README.txt` (§3.2) — High.
2. Crash-safe WAV header (§3.3) — High.
3. Marshal `source_remove` to the OBS main thread via `obs_queue_task(OBS_TASK_UI, …)` (§5.1) — High.
4. `data/manifest.json` (§4.1) and `buildspec.json`-driven CI with macOS sign/notarize (§4.2) — High.
5. Detect silent frame loss and surface it in `session.json`/README instead of `complete` (§1.3) — High.
6. Correct driver-cap documentation and warning copy; keep `kEncoderWarnThreshold` a warning, driver floor 570 for OBS 32.2 (§1.3) — Medium.
7. Confirm the “release encoders only after the stop signal” invariant holds even when the 5 s wait times out (§5.2) — Medium.
8. Route new UI strings through `obs_module_text` / `data/locale` (§4.3) — Medium.
9. obs-websocket vendor requests for arm/start/stop/status (§6.1) — Medium.
10. Optional: `skip_soft_remux`/`buffer_size` “crash-safe mode”, source-profiler instrumentation (§3.4, §6.2) — Low.

## Do NOT do this

1. **Do not enable NVENC split-frame encode for H.264, and do not expect it on an RTX 2050.** It is gated to HEVC/AV1 with `engines > 1` and is broken below driver 555 (§1.2). It cannot multiply this laptop’s encode capacity.
2. **Do not switch ISO video to AV1.** `mov_output` accepts only `h264;hevc;prores` (`plugins/obs-outputs/mp4-output.c:610`); AV1 belongs to plain `mp4_output`. The current `muxableEncoderId()` filter is correct — keep it (§2.5).
3. **Do not replace Hybrid MOV with a plain MP4 or with MKV.** MKV is the community crash-safe default, but it breaks the MOV-centric spec, the `.mov` naming contract, and the manifest’s codec story; Hybrid MOV already gives crash tolerance + a compact finalized file (§3.1). If you want stronger guarantees, add `skip_soft_remux` as an option, not a new container.
4. **Do not “fix” the redundant `obs_source_inc_showing` calls in isolation.** They are balanced and share the AUX_VIEW counter with `obs_view_set_source`; removing only the inc would unbalance the ref/activation count (§2.2).
5. **Do not add `obs_encoder_set_scaled_size` / `obs_encoder_set_gpu_scale_type` as a bug fix.** `obs_nvenc_h264_tex` does not advertise `OBS_ENCODER_CAP_SCALING` and texture encoders are rejected if they do (`libobs/obs-module.c:1135`); the view-based scaling already carries the correct timestamp clock (§2.3).
6. **Do not raise `kEncoderWarnThreshold` because the session cap rose to 12.** The binding limit is throughput (4 clean on the tested box), and the stream shares the GPU (§1.3).
7. **Do not touch encoders, outputs, or the view from the `source_remove` signal handler thread**, and do not assume `obs_encoder_get_settings` can be released late — it is addref’d and must be released by the caller (§5.1, §2.4).

---

## Unverified / open

- **Exact NVENC consumer session count in the SDK 13.x application note.** Assembled from the NVIDIA application note plus secondary sources; the specific integer (12) should be re-fetched from https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-application-note/ before it appears in user-facing copy.
- **RTX 2050 NVENC engine count.** Inferred as one engine from the GA107/Ampere architecture and the split-encode `engines > 1` gate; not read from NVIDIA’s support matrix. The conclusion (no split encode) holds regardless for H.264.
- **Whether any OBS code path shares one NVENC session across encodes.** None found in `plugins/obs-nvenc/`; absence of evidence, not proof.
- **obs-websocket API details.** `plugins/obs-websocket/` is an uninitialized submodule in this clone; the API was taken from the upstream `lib/obs-websocket-api.h`. Confirm the exact header, and that linking it is acceptable, before implementing §6.1.
- **Aitum internals.** Closed source; no primary source available.
- **`obs-multi-rtmp`.** Not inspected.
- **Long-session A/V drift.** Spec “Further Notes” #1: the single assumption the handoff rests on; still not measured. This research did not change that — it remains the first thing to build/verify.

---

## Primary sources

- OBS Studio 32.2.2 checkout: `/Users/misty/Developer/obs-studio` — cited files include `libobs/obs.h`, `libobs/obs-view.c`, `libobs/obs-encoder.c`, `libobs/obs-encoder.h`, `libobs/obs-module.c`, `libobs/obs-internal.h`, `libobs/util/source-profiler.h`, `plugins/obs-nvenc/nvenc.c`, `plugins/obs-nvenc/nvenc-properties.c`, `plugins/obs-nvenc/nvenc-helpers.c`, `plugins/obs-outputs/mp4-output.c`, `plugins/obs-outputs/mp4-mux.c`, `plugins/obs-outputs/mp4-mux.h`, `shared/bpm/bpm.c`, `plugins/image-source/color-source.c`.
- OBS 32.2 release notes (NVIDIA SDK 13 / driver 570; `obs_properties_add_button` deprecation): https://github.com/obsproject/obs-studio/releases/tag/32.2.0
- OBS 32.0 release notes (Hybrid MOV added, Hybrid MP4/MOV default): https://github.com/obsproject/obs-studio/releases/tag/32.0.0
- NVIDIA NVENC Application Note (session limits): https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-application-note/
- obs-plugintemplate (buildspec, CI, macOS signing/notarization): https://github.com/obsproject/obs-plugintemplate and https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS
- Exeldro `obs-source-record`: https://github.com/exeldro/obs-source-record
- obs-websocket vendor API: https://github.com/obsproject/obs-websocket (`lib/obs-websocket-api.h`)
- This repo: `docs/spec.md`, `docs/adr/0001-iso-recording-is-pre-composite-in-process-capture.md`, `README.md`, `CMakeLists.txt`, `src/*.cpp`, `src/*.hpp`.
