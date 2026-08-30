# Gap 1, the scale `s` — the derived default

**This file is README text that has not been merged.** It is written as a drop-in for `README.md`
and is kept out of it only because another change was in flight in that file at the time. Merge it
and delete this, or fold the parts you want.

Nothing in `src/` describes the calibration anywhere else; the derivation itself lives in
`src/hdr_codec.hpp` under **"THE SCALE, s — WHERE THE DEFAULT COMES FROM"**, which is the
authoritative copy. This file is the reader-facing half.

---

## 1. What changed

| | old | new |
|---|---|---|
| `cfg::paper_white_scale` default (`src/addon_config.hpp`) | `1.0f` | `4.0f / 3.0f` |
| `paper_white_scale` in the shipped `stray_dlssnr.ini` | `1.0` | `1.3333334` |
| the overlay's pre-seed atomic (`src/overlay_ui.hpp`) | `1.0f` | `4.0f / 3.0f` |

`1.3333334` is not a typo for `1.3333333`: it is the decimal that `strtof` maps to exactly the same
`float` as `4.0f / 3.0f`, and `1.0f / float(4/3)` rounds to **exactly `0.75f`**, so the `s` handed
to both dispatches is a clean binary constant. Verified numerically, not asserted.

**Nothing else moved.** Both HLSL string literals are byte-identical to the previous revision — the
encode's and the decode's FNV-1a source hashes, and therefore every `stray_dlssnr_*.<hash>.dxbc`
cache filename, are unchanged. This change cannot cause a recompile, and cannot cause a compile
failure.

---

## 2. Replacement for §2, *"`paper_white_scale` is a tuning knob with no calibrated value"*

Suggested new title: **`paper_white_scale` is derived, not measured**

> The semantics are Remix's, which means it is a **divisor**: `s = 1.0 / max(paper_white_scale, 0.01)`
> and the encode computes `original * s`. `SrgbEncode` saturates at `1.0`, so
>
> > **`paper_white_scale` is the scene-linear value this codec calls display white.**
>
> The default is **`4/3`**, and it is **derived, not measured on hardware**. It used to be `1.0`,
> which had nothing at all behind it. Two independent criteria pick `4/3`, and they agree to 5 %:
>
> * **the knee.** `1 / 0.75` puts UE4's diffuse white (SceneColor `1.0`) exactly on the soft-clip
>   knee, so the entire diffuse range is transferred through the **linear** segment of `SoftClip`
>   and the whole `0.75 → 1.0` shoulder is spent on things that are genuinely brighter than diffuse
>   white — which is what a shoulder is for.
> * **the measured full-gain point.** `1 / 0.79` is where ≥ 95 % of the network's requested change
>   still survives the FP16 round trip (`tools/hdr_codec_selftest.cpp` section 8, and it is the
>   selftest's own printed figure, not a recollection).
>
> What it buys, arithmetically:
>
> | | at `1.0` | at `4/3` |
> |---|---|---|
> | SceneColor `1.00` (diffuse white) → proxy code value | `1.0000` — **clipped** | `0.8808` (225/255) |
> | SceneColor `0.18` (mid grey) → proxy code value | `0.4614` (118/255) | `0.4030` (103/255) |
> | where the transfer goes **one-directional** (`fp16(SrgbEncode(SoftClip(v))) == 1.0`, so `neural − proxy ≤ 0` and the pixel can only **darken**) | SceneColor `1.809` | SceneColor `2.412` |
> | where ≥ 95 % of the requested change still survives | SceneColor `0.790` | SceneColor `1.053` |
>
> The third row is the point. At `1.0` the transfer lost the ability to brighten a pixel at
> SceneColor `1.81` — barely above diffuse white, which in a UE4 buffer is every lit surface facing
> a light and every neon sign in the slums. The fourth row is the other half: the whole diffuse
> range now receives ≥ 95 % of what the network asked for, where at `1.0` it began losing strength
> at `0.79` and was down to half by `1.15`.
>
> The cost is that the proxy the network sees is about **12 % darker in code values**
> (`0.75^(1/2.4) = 0.887`), with mid grey 15 code values under a textbook sRGB mid grey. That is
> the **bound** on how far this can be pushed rather than an anchor: the proxy has to stay a
> normally-exposed sRGB image, because that is the distribution the network was trained on. `4/3`
> keeps it there. `2.0` — attractive because it is a power of two, which would make even
> `hdr_graft = 1`'s round trip exact — puts mid grey at 85/255 and diffuse white at 188/255, and
> that is a visibly under-exposed photograph. It was rejected for that.
>
> * **raise** it if the image looks blown out — highlights crushed into the soft-clip shoulder;
> * **lower** it if the image looks black.
>
> Useful range is roughly `0.01 .. 64`. The overlay slider is **live**, so a sweep costs no relaunch
> — see the procedure below, which matters more than it sounds.
>
> If the image comes back with a colour cast rather than a brightness error, `color_strength` is the
> knob: at `0.0` the original's chromaticity is kept exactly and only the network's luminance change
> is transferred.

---

## 3. Addition to §6, Gap 1, replacing *"What remains open here is the scale"*

> **The scale `s` now has a derivation.** It does not yet have a hardware measurement, and those are
> different things.
>
> The evidence that something was wrong is real and is worth stating, because it is the only
> quantitative thing anyone has measured about this codec on the actual machine. Eight cold
> launches, sampling a 160 × 90 luma grid of 4K captures of the same scene:
>
> | | mean 8-bit luma | runs |
> |---|---|---|
> | `transfer_strength = 0` (a bit-exact bypass) | **46.9** | 47.02, 46.72 |
> | `transfer_strength = 1` | **41.0** | 41.08, 40.89, 40.94, 41.14, 41.23 |
>
> Within-group spread is ± 0.15 against a between-group gap of 5.9 — a 40:1 separation. Grafting the
> network's answer in **reliably darkens the frame by ~12 % of mean code value**. (Read as
> display-linear luminance rather than code value that is nearer ~22 %, but a mean of encoded
> values pushed through the sRGB curve is not the same thing as a mean of luminances, so treat the
> larger figure as indicative only. The camera position was also not perfectly reproducible between
> cold launches. **The direction and the grouping are solid; the exact percentage is not.**)
>
> **A systematic one-directional loss is the fingerprint of the `1.81×` clip**, because that clip is
> the only term in this codec with a built-in sign: above it the proxy is exactly `1.0`, the network
> cannot signal an increase at all, and `(neural − proxy) ≤ 0` for the rest of the transfer's life.
> Two things that could have explained the darkening do **not**, and ruling them out is what points
> at the scale:
>
> * **a multiplicative bias in the network is scale-invariant here.** Below the knee the residual is
>   `(SrgbDecode(neural) − v·s) / s`, so a network that returns `(1 − ε)x` contributes `−ε·v`
>   whatever `s` is. Raising `s` can neither help nor hurt that term.
> * **the chroma valve is inert at `color_strength = 1`**, which is where those runs were:
>   `graded = lerp(luminanceOnly, transferred, 1.0)` is exactly `transferred`.
>
> So the clip asymmetry is the only mechanism on the list that `s` touches at all, and the direction
> of the fix is "raise it".
>
> **What the new default does not claim.** It removes *one* of the two candidate mechanisms. No
> value of `s` can remove the other: a denoiser that eats positively-skewed Monte-Carlo noise lowers
> the mean by construction, because the noise it is removing is biased upward. **Expect an
> improvement here, not a null.** And the derivation rests on one external assumption that nothing
> in this add-on measures — that STRAY leaves UE4's pre-exposure at its default, so that SceneColor
> at the TAA tap really is exposure-normalised with diffuse white near `1.0`. If STRAY writes raw
> radiance instead, the right value is not near `4/3` at all, and the derivation has only fixed the
> *shape* of the answer (diffuse white belongs on the knee) and not its magnitude.
>
> **Why it is still a constant and not measured from the frame.** It should be measured eventually:
> the missing quantity is a statistic of the encode's own input, and that input is right there on
> the GPU beside the dispatch. It is not done because every cheap way to get it changes something
> this toolchain cannot re-verify. An average-luminance reduction needs a second UAV on the encode,
> which means a new root signature, new shader text, a new source hash, every on-disk
> `stray_dlssnr_*.dxbc` cache orphaned, and a mandatory fresh compile under whatever
> `d3dcompiler_47.dll` Proton hands us — and **a decode that does not compile latches the whole
> codec off and hands the user back the pre-codec darkened frame.** That is the exact trade this
> tree already refused once, for `hdr_graft`, and the survival build exists because of it. A
> constant that is wrong is one live slider away from right; a codec that will not compile is not.

---

## 4. The sweep to run on hardware

This is the thing that actually closes the gap, and it is cheap.

**Use the overlay slider, not the ini.** *Scene Paper-White Scale* is a tier-0 live knob: it is a
root constant snapshotted at the top of each pass, so it takes effect on the very next frame. A
cold relaunch per value moves the cat, and a same-config control run measured that confound at mean
|diff| **0.874** over the same 160 × 90 grid — *larger* than some of the effects being looked for.
**Park the camera and never relaunch during the sweep.**

1. `hdr_codec = 1`, `hdr_graft = 0`, `transfer_strength = 1`, `color_strength = 1`.
2. Park the camera somewhere with **both** a dark diffuse area and a bright saturated one — a slums
   alley with neon in frame is the ideal shot, because it exercises the clip and the shadows at
   once.
3. Sweep the slider `1.0 → 1.33 → 1.6 → 2.0 → 3.0 → 4.0`, capturing at each stop.
4. Also capture `transfer_strength = 0` at any one of them — it is the bit-exact bypass and it is
   the same image at every scale, so it is the **anchor** the whole sweep is measured against.

What to look for, in order:

| | what it means |
|---|---|
| mean luma at `ts = 1` climbing toward the `ts = 0` anchor as the scale rises | the clip really was the darkening. The value where it stops climbing is the calibration. |
| mean luma flat across the whole sweep | the darkening is **not** the clip — it is the network's own denoise bias, and `paper_white_scale` is the wrong knob entirely. Report that; it is a more useful result than a number. |
| neon signs and wet-street reflections regaining detail as the scale rises | the highlight half of the same effect, visible by eye where the mean is not. |
| the frame going flat and grey at the top of the sweep | the proxy has gone out of distribution the *other* way — under-exposed. Back off; do not chase mean luma past this. |

`transfer_strength = 0` must stay pixel-identical to `copy_back = 0` **at every stop of the sweep**.
It is bit-exact at every `s` in `hdr_graft = 0` and the selftest proves it over 20,000 pixels per
scale, so if it ever is not, the fault is in the plumbing and not in this number.

---

## 5. Loose ends this change deliberately did not touch

`src/stray_dlssnr.cpp` was **not edited** — another change was in flight in it. Three log strings in
it still describe the value the way they did when it was a guess. None of them is now *false*, since
the value still is not a hardware measurement, but two of them would read better refreshed:

* the `LOGW` beginning `"DLSS-NR: paper_white_scale=%.4f is UNCALIBRATED for STRAY"` — the RAISE /
  LOWER advice below it is still exactly right; the sentence worth adding is that the default is
  derived (diffuse white on the knee, `1/0.79`) rather than arbitrary.
* the startup summary line `"  hdr codec       hdr_codec=%d paper_white_scale=%.4f (UNCALIBRATED)"` —
  `(DERIVED, NOT MEASURED)` is the more accurate tag.
* the line ending `"with no calibrated value - see paper_white_scale."` — still true as written.

`README.md` §1's tuning walkthrough (*"If it is too dark or too bright, tune `paper_white_scale`"*)
and the key table's `| paper_white_scale | 1.0 | ... UNCALIBRATED |` row both need the new default.
