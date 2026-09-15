# 0003 — The render pass is not being built

Status: accepted · 2026-09-15

## Context

Every armed source is a full extra render pass per frame. `can_reuse_mix_texture`
(`libobs/obs-video.c`) only reuses a previously rendered mix when `other->view ==
mix->view`, and each recorder owns a **distinct** `obs_view_t`
(`source-recorder.cpp`), so no reuse is possible and `output_frames` renders every mix
each frame on the graphics thread. This is inherent to the pre-composite design
(`docs/adr/0001`), not a bug.

The work plan's Task 8 proposed removing the duplicate render for a source that is also
on the main canvas, through a private `OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW` proxy
source owning a cached `gs_texrender`, rendering its target once per frame (invalidated
in `video_tick`) and blitting thereafter, following `gpu-delay.c`'s colour-space handling
rather than branch-output's plain `GS_BGRA`.

## Decision

The per-source duplicate render pass is **accepted as-is**; the proxy/render-pass design
is **rejected**. The measurement gate in Task 8 said to stop if N renders are not a
measurable share of frame time, and they are not:

| counter | median | frame budget |
| --- | --- | --- |
| `render_main_texture` | 0.018 ms | 16.667 ms |
| `render_video` | 0.11 ms | 16.667 ms |

N renders are not a measurable share, so there is nothing to buy.

## Consequences

- **N visual sources still cost N source renders per frame.** No supported API changes
  that, and the measured cost does not justify the proxy.
- **The proxy was rejected on more than the measurement.** It pays off only when the
  source is also on the main canvas — the canvas has to be rendering the proxy for the
  duplicate render to disappear — so it would require the plugin to silently restructure
  the user's scenes. That is out of bounds, and the measurement removes the incentive to
  find a way around it.
- **The size cap is still worth having.** It shrinks the convert, the staging/readback
  and the encode in proportion to pixel count; it does not shrink the source's own
  render, which happens at base size and is the part this ADR accepts.
