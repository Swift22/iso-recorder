## What this changes

<!-- One or two sentences. The diff says how; this says why. -->

## How it was tested

<!--
"Compiles" is not a test. Name the platform, the OBS version, and what you actually
did — e.g. "Windows 11, OBS 32.2.2, ticked Browser + Desktop Audio, recorded 10 s,
both files play and session.json says complete".

If you touched session-writer or wav-writer, paste the ctest result:
  ctest --test-dir build-mac --output-on-failure
-->

## Checklist

- [ ] One change, focused — no unrelated reformatting
- [ ] `ctest` passes (and a test covers anything pure that I changed)
- [ ] Anything touching OBS was checked by hand in OBS, on a real build
- [ ] `docs/spec.md` / `docs/adr/` still hold, or I have said why they should change
