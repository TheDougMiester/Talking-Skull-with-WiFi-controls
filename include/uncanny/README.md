# 184px palette-selectable Demon Eye asset

`DemonEye184.h` contains a palette-neutral 184x184 intensity texture plus 128x128 upper/lower eyelid threshold masks. It was generated offline from the Goat/Krampus tables in:

https://github.com/thelastoutpostworkshop/ESP32LCDRound240x240Eyes

Reviewed source commit:
`0d987ac201cca626899565d2369dba2ce5fee24d`

The source project and adapted Uncanny Eyes renderer are MIT licensed. The copyright notice and license are retained in `LICENSE-ESP32LCDRound240x240Eyes.txt`.

The offline conversion performs high-quality Lanczos resampling once. Firmware applies green, blue, or red palettes plus autonomous brightness, position, blink and squint at runtime. Zero intensity remains black in every palette, preserving the vertical slit pupil.

`GoatEye128.h` and `GreenDemonEye208.h` are retained in the fork for provenance and rollback but are no longer included by the firmware and do not occupy compiled flash.
