# Talking Skull Calibration Notes

> Historical development notes. The production release intentionally removes continuous calibration, clipping, memory, jaw, and FFT print statements. Re-enable temporary instrumentation only while tuning, then remove it again.

## Audio output boost
The MAX98357A GAIN pin remains floating because grounding it made even the test tone noisy. Software volume now reaches a **2.0× PCM factor** at web volume 30 instead of 1.1×. This adds about **5.2 dB** over the previous maximum and recovers the 6 dB deliberately lost by the stereo `(left + right) / 2` downmix.

The development build once reported a digital-clipping percentage after playback. That calibration output and its per-sample counters were removed from the production release. If distortion is audible, reduce web volume one or two steps. If maximum volume is clean but still too quiet, inspect amplifier supply voltage/current, speaker impedance/sensitivity, and enclosure loading.


## Round 1 (done): full travel + fast slew
Root cause was `JAW_OPEN_LIMIT_FRACTION = 0.35` plus a 6 µs/tick slew cap. Fixed to 100% range with 20/28 µs asymmetric steps. The user subsequently recalibrated the safe closed endpoint from 1754 to **1774 µs**; the active range is now 269 µs from 1505 µs fully open to 1774 µs fully closed. Both endpoints remain hard-constrained.

## Round 2 (done): the mouth pegged open during speech
Symptom: jaw moved, but hovered nearly fully open the whole time.

Cause, in `AudioFFTProcessor_ESP32_Optimized.cpp::processBuffer()`:
1. `normalized` is computed against the envelope's own running peak. With a hot, continuously-mastered vocal track the envelope sits near its peak almost constantly, so `normalized` hovered at 0.8–1.0.
2. `jawFactor = powf(normalized, 0.65f)` — an exponent *below* 1 pushes mid-levels up toward open.
3. Slow release (0.15) + 90 ms hold swallowed the dips between words.

Fixes (all tunable via `EnvelopeConfigFast`; the development build originally echoed these values at boot):
| Knob | Before | Now | What it does |
|------|--------|-----|--------------|
| `peakHeadroom` | (implicit 1.0) | **1.25** | Normalization ceiling = 1.25 × tracked peak. Sustained speech now maps to ~0.7–0.8 of normalized range instead of ~1.0, so the mouth is *not* pegged open. |
| `jawCurveExp` | 0.65 (compresses up) | **1.7** (expands) | `jawFactor = normalized^1.7`. Typical speech lands mid-range; word gaps collapse toward closed; emphasized syllable onsets (which overshoot the lagging peak tracker and clamp at 1.0) still hit full open. |
| `releaseAlpha` | 0.15 | **0.20** | Faster envelope fall at word gaps → jaw visibly closes between words. |
| `holdMs` | 90 | **50** | Shorter floor-hold before the between-word decay starts. |
| gap decay | 0.92/frame | **0.88/frame** | Envelope drops to ~43% over a 200 ms gap → real closure, not a tick. |

Net effect: no added micro-jitter (the curve actually *compresses* movement near closed, where small wiggles map to ~nothing), but the jaw now has a large word-to-word swing: roughly 15–20% open during gaps, 50–75% open on sustained speech, 100% on emphasized onsets like "er" in "thriller".

## Round 2 tuning knobs
All in `main.cpp` `setup()` → `audioFFT.setConfig(EnvelopeConfigFast(...))`:
`EnvelopeConfigFast(attack, release, gain, noiseFloor, holdMs, sibilantSuppression, peakHeadroom, jawCurveExp)`

If further tuning is required, temporarily instrument `handleJawTick()` to capture `raw`, `filtered`, and `env` while playing:

| Symptom | Change |
|---------|--------|
| Still too open overall | `peakHeadroom` ↑ (1.25 → 1.5). Never go below 1.1 or it pegs open again. |
| Too closed / weak peaks, "er" no longer hits 1505 µs | `peakHeadroom` ↓ (1.25 → 1.15) and/or `jawCurveExp` ↓ (1.7 → 1.4). |
| Word-to-word swing too subtle | `jawCurveExp` ↑ (1.7 → 2.2). Watch for jumpiness on consonant bursts; that's when it's too high. |
| Mouth doesn't close between words | `releaseAlpha` ↑ (0.20 → 0.28), `holdMs` ↓ (50 → 30). |
| Too fluttery within a word | `releaseAlpha` ↓ (0.20 → 0.15), `jawCurveExp` ↓ slightly. |
| Quiet track → mouth barely opens | `jawAnalysisGain` ↑ (3.5 → 5.0) — analysis gain only, never touches speaker output. |

Rules of thumb:
- `raw` (jawFactor) should spend time in three zones during a song: ~0.0–0.2 in gaps, ~0.4–0.8 on ordinary words, ~0.95–1.0 a few times per phrase on emphasized syllables. If it's parked in one zone, the knobs above are the levers.
- Full open (target=1505) a handful of times per phrase is the goal — not constantly, not never.

## What was deliberately left alone
- FFT bands (300–1000 Hz opens / 2500–6000 Hz closes): correct for "th" vs "er".
- Peak tracker rise/fall rates: the slow fall (~40 s) is intentional — it keeps the reference stable so headroom does its job predictably.
- `main.cpp` jaw smoothing (0.45/0.22 @ 20 ms) and slew (20/28 µs) from Round 1.

## Round 1 leftovers (still valid)
- Current hard limits are 1505 µs open and 1774 µs closed, with a 269 µs active range at a 1.00 limit fraction.
- If mid-word closures still lag: the hold is only active below the noise floor, so mid-word "th" depends on `releaseAlpha` + sibilant suppression — raise `sibilantSuppression` (0.65 → 0.8) for harder consonant snaps.
