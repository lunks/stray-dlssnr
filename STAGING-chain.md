# STAGING-chain.md — bringing CHAIN MODE up in STRAY

DLSS-NR **then** DLSS-SR, on one accepted TAA dispatch, behind `dlss_chain` (default `0`).

This is STAGING-sr.md's ladder with one extra rung in front of it. Do not skip to the end: the
whole point of a ladder is that each rung has exactly one thing that can be wrong. If DLSS-SR alone
has never run on this install, **go and do that first** — chain mode is DLSS-SR with DLSS-NR
spliced in ahead of it, and debugging both at once is debugging neither.

---

## 0. What must be on disk

Everything STAGING-sr.md rung 0 lists, **plus** `nvngx_dlssnr.dll`. Chain mode needs BOTH snippets:

```
S:\common\Stray\Hk_project\Binaries\Win64\
  stray_dlssnr.addon64
  remix_nvngx.dll          <- slot A = DLSS-NR, slot B = DLSS-SR
  nvngx_dlssnr.dll         <- the patched NR snippet
  nvngx_dlss.dll           <- the SR snippet
  stray_dlssnr.ini
```

If either snippet is missing you get one line naming which, and the run falls back to whichever
single feature armed. It never half-runs.

---

## 1. Engine.ini

Identical to the DLSS-SR block — chain mode adds no new CVar.

```
%STEAM%/steamapps/compatdata/1332010/pfx/drive_c/users/steamuser/AppData/Local/
  Hk_project/Saved/Config/WindowsNoEditor/Engine.ini
```

```ini
[SystemSettings]
r.TemporalAA.Upsampling=1
r.SecondaryScreenPercentage=100
r.ScreenPercentage=50
```

Never below 50%. `r.TemporalAA.Upsampling=1` selects UE4's **MainUpsampling** permutation, which is
a different `#define` set, therefore different DXBC, therefore a different `fnv1a64` — so
`sr_shader_hash` must be re-pinned (rung 1 below). Without this block the TAA output UAV is the
same size as the colour input, there is nothing to upscale into, and chain mode refuses by name.

---

## 2. The ladder

### Rung 0 — DLSS-SR alone, working

`dlss_chain = 0`, `dlss_sr = 1`, and STAGING-sr.md walked to the rung you intend to run at. You
should have, in `ReShade.log`:

```
DLSS-SR: EVALUATE #1 OK. Color=t5 ... 1920x1080 ..., Output=... 3840x2160 ...
--- DLSS-SR @ frame N: evaluates=... geometry=1920x1080 -> 3840x2160
```

Note the render and output extents. Chain mode does not change either of them.

### Rung 1 — the hash

`dlss_chain` selects `sr_shader_hash` exactly as `dlss_sr` does, so if rung 0 worked, the pin is
already right. If you are coming from a DLSS-NR-only install, re-pin it: the log's
`DLSS-NR: pass did not run - this dispatch is not the target shader` is the ONLY symptom of a stale
pin, and it looks identical to the add-on simply being off.

### Rung 2 — turn the chain on, codec on, nothing suppressed

```ini
dlss_chain      = 1
dlss_nr         = 1
dlss_sr         = 0
hdr_codec       = 1
sr_mvec_decode  = 1
sr_suppress_taa = 0
sr_direct_output= 0
sr_copy_back    = 0      ; still writing into a texture nothing reads
```

Expect, once:

```
DLSS-CHAIN ARMED. One accepted TAA dispatch, TWO networks, in this order:
  [1] codec encode -> DLSS-NR (feature 18) at the RENDER extent 1920x1080 -> st.out_tex
  [2] codec decode -> DLSS-SR (feature 1) COLOUR = result_tex, LINEAR HDR ... -> u0 3840x2160
DLSS-CHAIN: CHAINED EVALUATE #1 OK - BOTH networks ran on ONE accepted dispatch.
  [1] DLSS-NR  feature 18, Color=the display-referred PROXY 0x... 1920x1080 (s applied), MVec=0x... 1920x1080 scale 1.0000/1.0000, Output=out_tex 0x..., Reset=1
  [2] codec decode -> result_tex, LINEAR HDR (original + residual)
  [3] DLSS-SR  feature 1, Color=0x... (view rect 1920x1080), MotionVectors=THE SAME 0x..., Output=the add-on's own texture 3840x2160, Jitter=(...), Reset=1
```

and, every 1800 frames:

```
--- DLSS-CHAIN @ frame N: chained=1798 (of 1798 DLSS-SR evaluates) sr_armed=1
```

**`chained` is the only number that proves the chain ran.** It is incremented on the one branch
where both `EvaluateFeature` calls returned `Success` on the same dispatch. Zero means it did not,
and one of the one-shot `DLSS-CHAIN:` lines above it names the stage that refused.

The picture at this rung is stock TAAU — nothing is written back yet. That a frame still renders
correctly is positive evidence that the state restore survives **two** NGX evaluates in one window,
which is the thing in this design nobody has done before in this stack.

### Rung 3 — the identity A/B, before you look at image quality

```ini
transfer_strength = 0
sr_copy_back      = 1
```

This must be **pixel-identical** to `dlss_chain = 0, dlss_sr = 1` at the same ini. At 0 the decode
is `result = lerp(original, graded, 0) = original` exactly, so DLSS-SR is handed the same `t5` bits
it is handed today — while the encode, the DLSS-NR evaluate, the decode, the extra barrier and the
shared motion guide all still run. If this frame differs, the new plumbing is wrong and no amount
of tuning will fix it. Screenshot both, compare.

Then put `transfer_strength` back where it was.

### Rung 4 — look at the picture

`sr_copy_back = 1`, `transfer_strength` restored. Now you are seeing a denoised, upscaled frame.

What to look for, in this order:

1. **Scale.** A 2x-per-axis smear that tracks camera motion means the shared motion guide is being
   read at the wrong grid by one of the two networks. Check that the log's `[1] ... MVec ...
   1920x1080` and `[3] ... MotionVectors=THE SAME` name one resource at one extent.
2. **Brightness.** Too dark or blown out is `paper_white_scale`, which has no calibrated value.
   Raise it if highlights are crushed, lower it if the image is black.
3. **Shimmer that does not track motion.** This is the known unknown: chained, DLSS-NR is fed the
   **jittered** render-resolution colour, and the snippet has no jitter parameter. A/B against
   `r.TemporalAASamples=1` in Engine.ini. If that fixes it, the ordering is the problem, not a
   setting.

### Rung 5 — suppression and direct output

`sr_suppress_taa = 1`, then `sr_direct_output = 1`, one at a time, exactly as STAGING-sr.md rungs
6 and 7. Nothing about them is chain-specific.

---

## 3. What each refusal means

| line | meaning |
|---|---|
| `DLSS-CHAIN: dlss_chain=1 but the chain cannot run - DLSS-NR is NOT armed ...` | a snippet is missing or its `Init_Ext` failed. The named half did not arm; the other one is running alone. |
| `DLSS-CHAIN: ... this dispatch is NOT an upsampling one ...` | Engine.ini is not in MainUpsampling, or `sr_out_width/height` is pinned wrong. The line carries the exact block to paste. |
| `DLSS-CHAIN: the HDR codec is NOT running (...)` | DLSS-SR is being fed a display-referred image as linear HDR. Set `hdr_codec=1`. |
| `DLSS-CHAIN: the DLSS-NR textures could not be prepared at the RENDER extent ...` | a geometry move (self-heals on the next present) or an allocation failure. This frame is DLSS-SR alone. |
| `DLSS-CHAIN: the DLSS-NR EvaluateFeature FAILED: 0x...` | the NR half refused. DLSS-SR still ran; the frame is upscaled, not denoised. |
| `DLSS-CHAIN: the DLSS-NR half has failed 8 frames running` | run-latched off. Read the FIRST error above it. |
| `DLSS-SR: pass did not run - <reason>` | the SR half refused before the chain could start. Everything DLSS-SR's own staging notes say applies. |

`copy_back` and `history_restore` are reported as ignored/inert once, on the first chained frame,
so a user reading the ini is not misled about what they are doing.

---

## 4. What is NOT measured

No frame-time number appears anywhere in this file, because none has been taken. The shape of the
added cost per accepted dispatch, against DLSS-SR alone, is: one more `EvaluateFeature` (feature
18, at the render extent), two 1920x1080 compute dispatches (encode, decode), two barriers, and
three render-extent textures resident. Against DLSS-NR alone it is: one more `EvaluateFeature`
(feature 1), minus the pristine copy, minus the copy-back, minus four barriers. Measure it with the
census's frame counter against the same scene at `dlss_chain=0, dlss_sr=1`.
