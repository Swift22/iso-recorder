# 0002 — The background is a source because a view has no clear

Status: accepted · 2026-09-15

## Context

A source with a transparent background — veadotube published over Spout2 — recorded
its transparent regions as **black**. The cause is in libobs, not in the plugin:

- `render_main_texture` (`libobs/obs-video.c`) clears the mix render target to
  `vec4(0,0,0,0)` — transparent black — and then calls `obs_view_render`.
- `obs_view_render` (`libobs/obs-view.c`) walks the view's channels in ascending order
  and sets no blend state and does no clear of its own.
- The encoder receives NV12, which has no alpha plane, so a transparent pixel has only
  its RGB value to fall back on: black.
- There is no `obs_view_t` equivalent of `obs_display_set_background_color`
  (`obs-display.c` is display-only).

So the background cannot be set on the view. It has to be a **source** drawn under the
recorded one, because the default straight-alpha blend (`src=SRCALPHA`,
`dst=INVSRCALPHA`) lets an `a=0` pixel show whatever is beneath it.

## Decision

Each per-source view carries a private `color_source` on channel 0, below the recorded
source on channel 1. Channel order is guaranteed by `obs_view_render`'s ascending loop.

The colour is the `0xAARRGGBB` int `0xFF00FF00` — chroma green, read by the source as
`vec4_from_rgba` into `float4(0,1,0,1)`. The type is resolved through
`obs_get_latest_input_type_id("color_source")` so it lands on the non-obsolete
`color_source_v3`, and it is sized to the view's base dimensions so it fills exactly.

Rejected alternatives: a private scene (plain items use the same straight-alpha path, and
a canvasless scene falls back to main-canvas dimensions), and a custom shader (the plugin
cannot intercept the mix render).

## Consequences

- **Always on, hardcoded.** There is no toggle and no colour picker; the green is a fixed
  part of every video recording.
- **4:2:0 subsampling makes the green soft at the edges.** The chroma is stored at half
  resolution, so a one-pixel-wide transparent region is blended, not pure. That is the
  format, not a defect.
- **A Spout source can still cover it.** The Spout2 source never sets a default for
  `compositemode`, so it reads `0` and falls into the opaque shader arm that forces
  `alpha = 1.0`; in that state transparent pixels are RGB 0 with alpha 1 and occlude the
  green. Only `Premultiplied Alpha` (4) or `Converted Premultiplied Alpha (legacy)` (2)
  blend correctly. The plugin **warns** — in the row tooltip and the dock status — and
  never edits another plugin's source settings.
