# 0001 — ISO recording is pre-composite, in-process capture

Status: accepted · 2026-09-12

## Context

A stream leaves OBS as one composited picture: the game and the character baked
together, voices summed into a track or two. Handing that recording to an editor
means the pieces no longer exist — nothing can be reframed, reordered, or rebalanced,
because there is nothing separate to move. Recovering them means re-downloading the
VOD and rebuilding by hand.

The alternative is to capture each source on its own before OBS composites it. The
obvious precedent, Source Record, does exactly this, but implements it as an **OBS
filter** on a source. A filter needs a parent with a picture, so a pure audio source —
the mic, the Discord tap — cannot take one. The recorder has to own its sources to
reach all of them.

## Decision

Capture happens **pre-composite, in-process, per source**, rather than splitting a
composited recording after the fact. Each armed source is held as an `obs_source_t *`
and driven through its own encoder; its own filters are included and the scene's mix
decisions are not. The frames carry OBS's own graphics-thread clock, which is what
lets the independent files line up.

The recorder is **dock-owned, not a filter**, because a filter cannot attach to an
audio-only source. The dock holds a reference to any source — with or without a
picture — and records it on its own terms.

## Consequences

- **N visual sources cost N simultaneous encodes.** Arming several is arming several
  encoders at once, on top of the stream. The dock warns past a threshold
  (`kEncoderWarnThreshold`) and never refuses; that threshold will be set from a
  measured ceiling on each machine once one is measured, rather than guessed.
- **Audio-only files are written by us, with our own `WavWriter`.** No OBS output
  accepts an audio-only stream (`obs-output.c`), so the spec's `ffmpeg_muxer` +
  `ffmpeg_pcm_s24le` path does not exist for a WAV. Our writer is fed by the private
  `audio_output` and produces WAV 24-bit PCM at 48 kHz.
- **FLAC and AAC are deferred to v2.** Both need an FFmpeg muxer on the audio-only
  path. WAV is the default and the editor's choice; the alternatives wait for v2 with
  the timeline export.
- **A source armed mid-session starts at its own offset.** Not every file begins at
  zero. Each start time is written to `session.json` and `README.txt`, so alignment is
  arithmetic rather than guesswork.
