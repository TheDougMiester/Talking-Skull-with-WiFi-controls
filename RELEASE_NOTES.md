# MIT Production Release

This release is based on the successful Stage 10 firmware.

## Eye mounting and palette update

- Added independent `LEFT_GC9A01_INVERTED` and `RIGHT_GC9A01_INVERTED` compile-time options in `GC9A01_Eyes.h`.
- Set both current installation options to `1` for 180°-inverted physical mounting.
- Added a scary hazel iris palette and a fourth HAZEL radio option to the `/eyes` page.
- Preserved the permanently black pupil, independent flares, blink, squint, and manual motion behavior.

## Production cleanup

- Added a file-purpose summary and complete MIT license notice to every `.cpp` and `.h` file.
- Added a project-level `LICENSE` file.
- Removed routine startup, calibration, jaw, FFT, memory, playback, eye-mode, upload-success, and clipping print statements.
- Removed the per-sample clipping counters that existed only to feed calibration output.
- Retained critical `ERROR`, `WARNING`, and `FATAL` Serial messages.
- Updated the user manual and calibration notes for the quiet release.

## Verified build

```text
Flash: 1,317,471 / 3,145,728 bytes (41.9%)
Static RAM: 61,356 / 327,680 bytes (18.7%)
Result: SUCCESS
```
