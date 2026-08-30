# STAGING-sr.md — bringing DLSS Super Resolution up in STRAY

**This file is the runbook.** It is written to be walked without help, from a cold machine, in
order, one rung at a time. Every rung is an ini edit (no rebuild), names exactly what to look for
in `ReShade.log`, and says what each failure means.

**Nothing here is deployed.** The build in this branch has never run on hardware.

**The default is `dlss_sr = 0`, and with it 0 the build is bit-identical in behaviour to the
DLSS-NR build that ships today.** You can install it, play, and walk this file later.

---

## 0. What must be on disk

In `S:\common\Stray\Hk_project\Binaries\Win64\`, beside the ReShade DLL:

| file | note |
|---|---|
| `stray_dlssnr.addon64` | this build |
| `remix_nvngx.dll` | **THIS build.** It now carries a second snippet slot. An older one has slot A only |
| `nvngx_dlss.dll` | the DLSS-SR snippet, 310.8.0.0, 59 MB — from `SL 2.13` |
| `nvngx_dlssnr.dll` | the DLSS-NR snippet, only needed while `dlss_nr = 1` |
| `stray_dlssnr.ini` | this build's, which has the whole `dlss_sr` block |

> **The trampoline is the one file people forget.** `remix_nvngx.dll` holds one set of forwarding
> pointers *per slot*; DLSS-SR uses **slot B**, which did not exist before this branch. If you copy
> the new add-on over an old trampoline, DLSS-NR keeps working and DLSS-SR fails with a message
> that names the problem exactly:
>
> ```
> DLSS-SR not available: remix_nvngx.dll is present but does not export
> RemixNgxTrampoline_SetSnippetB, so it is an OUT-OF-DATE trampoline.
> ```
>
> If you instead see every SR call returning `0xbad00007 FAIL_NotInitialized`, that is the same
> problem one layer down.

---

## 1. The Engine.ini block

`%STEAM%/steamapps/compatdata/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor/Engine.ini`

```ini
[SystemSettings]
r.TemporalAA.Upsampling=1
r.SecondaryScreenPercentage=100   ; the post chain and the TAA OUTPUT move to 3840x2160
r.ScreenPercentage=50             ; 1920x1080 in -> 3840x2160 out. DLSS "Performance".
```

**Why this and not DLAA.** The TAA pass currently runs at **1920x1080** while the swapchain presents
at **3840x2160** — both measured on this hardware. So the game is *already* spatially upscaling
1080p→4K downstream of TAA, with a filter that has no temporal information. That upscale is exactly
what DLSS replaces, and 1920→3840 costs the same to render as the game does today. Native-4K DLAA
(`r.ScreenPercentage=100`) is a valid fallback but is ~4x the pixel cost and is not the shape you
will actually play.

**Never go below 50%.** `FTAAScreenPercentageDim = 3` is gated on `Pass == MainSuperSampling`, so
below 50% `MainUpsampling` falls through to a permutation whose LDS cache is sized for ≥50%. The
whole safety story on this path rests on the game's own TAAU producing a correct frame when DLSS
declines, so the engine must not be put in a configuration where its own TAAU is broken. Quality
(0.667) and Performance (0.5) are fine. **Ultra Performance (0.333) is off the table.**

---

## 2. The ladder

Each rung: the ini edit, the log line that means it worked, and what a failure means.

> Every rung leaves `dlss_sr = 1` and adds exactly one thing. **Never change two.**

---

### Rung 0 — the CVars, with the add-on unchanged

`stray_dlssnr.ini`: nothing. Leave `dlss_sr = 0`. Apply the Engine.ini block. Launch.

**Look for** the DLSS-NR pass's own banner:

```
DLSS-NR: TAA pass located and accepted.
  shader   0x................
  depth    t0   ... 1920x1080
  colour   t5   ... 1920x1080
  output   u0   res=0x.... r16g16b16a16_float 3840x2160
```

**What it proves.** That `r.SecondaryScreenPercentage=100` is honoured from `[SystemSettings]` in
the shipping build — the colour SRV stays at 1920x1080 while **u0 moves to 3840x2160**. This is the
single highest-value cheap measurement in the whole plan.

**If u0 is still 1920x1080** the secondary screen percentage is not being honoured. The ratio is
coming from somewhere else (an in-game resolution-scale slider writing a different CVar, or a
Proton/gamescope-level scale). Find it before going further — nothing below works without it.

**If the banner does not appear at all**, the shader hash moved. That is expected: flipping
`r.TemporalAA.Upsampling` changes `TAA_PASS_CONFIG`, which changes the DXBC, which changes the
`fnv1a64`. The symptom is exactly:

```
DLSS-NR: pass did not run - this dispatch is not the target shader
```

Go to rung 1.

---

### Rung 1 — re-pin the shader hash

Temporarily set `shader_hash = 0` (the census gates plus the SRV class quorum still identify it),
relaunch, and read the census line the probe prints. Then put the real number in **`sr_shader_hash`**
and restore `shader_hash` to `0x1708ec956099e259` — that keeps DLSS-NR's own pin intact so the two
features can be A/B'd on one install.

```ini
shader_hash    = 0x1708ec956099e259   ; DLSS-NR's, unchanged
sr_shader_hash = 0x................   ; the MainUpsampling permutation
```

`FTAAFastDim` also depends on the in-game AA-quality slider, so a single pinned hash is inherently
fragile across settings. Keep the `shader_hash = 0` fallback working.

---

### Rung 2 — arm DLSS-SR, evaluate into nothing

```ini
dlss_sr         = 1
sr_copy_back    = 0      ; write to a texture NOTHING READS
sr_suppress_taa = 0      ; the game's TAAU still runs
sr_direct_output= 0      ; the add-on owns the Output texture
```

**Look for, in order:**

```
DLSS-SR: loaded nvngx_dlss.dll and routed every call through remix_nvngx.dll's slot B.
DLSS-SR: View uniform buffer LOCATED for JITTER. status=ok tier=full ... rows{proj=28 noaa=32 clip=122 jitter=126 size=130 params=152}
DLSS-SR: FIRST JITTER READ. ... echo=first_frame
DLSS-SR: OUTPUT EXTENT 3840x2160, derived from the dispatch's own group counts. ... upscale 2.000x by 2.000x
DLSS-SR: CreateFeature parameters - render 1920x1080 -> output 3840x2160 (ratio 0.5000), PerfQualityValue=MaxPerf (0), Create.Flags=0x4a ..., DLSS.Use.HW.Depth=1. ... the key that will actually be read is "DLSS.Hint.Render.Preset.Performance" (Performance)
DLSS-SR: CreateFeature(feature 1) SUCCEEDED. render 1920x1080 -> output 3840x2160
DLSS-SR: EVALUATE #1 OK. Color=t5 ... MotionVectors 1920x1080 (decoded, absolute render-grid pixels, r16g16_float), Output=the add-on's own texture 3840x2160, Jitter.Offset=(..., ...), ...
--- DLSS-SR @ frame 600: evaluates=600 suppressed_dispatches=0 mvec_decodes=600 geometry=1920x1080 -> 3840x2160 (armed=1 ...)
```

**`EVALUATE #1 OK` is the proof the feature RAN**, not merely that it linked. It is printed from
the branch immediately after `EvaluateFeature` returned `Success`, and the census line's
`evaluates=` counter is incremented on that same branch. If you see `evaluates=0`, the pass never
reached the evaluate and a one-shot line says which stage refused:

```
DLSS-SR: pass did not run - <reason>
```

**The frame on screen is entirely the game's** at this rung. Nothing DLSS writes is read. A frame
that still renders correctly is positive evidence that the D3D12 state save/restore around the NGX
evaluate is faithful — independent of image quality.

#### Failures at this rung, and what each means

| what the log says | what it means | what to do |
|---|---|---|
| `DLSS-SR not available: nvngx_dlss.dll was not found next to the add-on` | the snippet is not deployed | copy it from `SL 2.13` |
| `... does not export RemixNgxTrampoline_SetSnippetB, so it is an OUT-OF-DATE trampoline` | old `remix_nvngx.dll` | redeploy the trampoline from this build |
| `Init_Ext FAILED: 0xbad00002 FAIL_PlatformError` | the **caller gate**: the call did not appear to come from a module whose path contains `nvngx.dll` | the trampoline is missing, or its forwarders tail-jump. `build.sh` and CI both verify `call=1 tailjmp=0` for every slot-B forwarder — if that passed, suspect the deployed DLL is stale |
| `the sub-pixel jitter could not be recovered` | `ue4_jitter` refused. **The SR pass will NOT run** — `Jitter.Offset.X/Y` are unconditionally required and sending `(0,0)` would be a silent shimmer rather than an error | the message immediately above names the failing predicate. If only the tier failed, `sr_jitter_projection_only = 1` accepts the weakest tier |
| `CreateFeature(feature 1) FAILED: 0xbad00005 FAIL_InvalidParameter` | one of `Width`/`Height`/`OutWidth`/`OutHeight` was unreadable, or a dimension check failed (min 32x32, `Width <= OutWidth`, `PerfQualityValue` outside 0..5) | read the `CreateFeature parameters` line printed just above it — every value is there |
| `CreateFeature ... 0xbad0000d FAIL_OutOfGPUMemory` | 4K allocation | the NR path never ran at 4K; this is the first place a memory problem shows |
| `EvaluateFeature FAILED: 0xbad00009 FAIL_RWFlagMissing` | the resource bound as `Output` has no `ALLOW_UNORDERED_ACCESS` | at this rung the Output is the add-on's own texture, so its usage set is wrong — that is a code bug, report it |
| `EvaluateFeature FAILED: 0xbad00008 / 0xbad0000e` (unsupported format) | a bound resource's DXGI format was refused. **Prime suspect: `Depth`.** STRAY's `t0` is `r32_g8_typeless`, a typeless planar resource, and D3D12 NGX has no channel through which to be told the view format | the add-on will revert the motion guide on its own after 4 frames and say so; if the failure survives that, it is the depth resource and a conversion pass into a dedicated `R32_FLOAT` texture is required |
| `EvaluateFeature FAILED: 0xbad0000a FAIL_MissingInput` | one of the four resources resolved to NULL | a handle was 0 at bind time — read the `EVALUATE` line's resource list |
| after 8 consecutive failures: `DLSS-SR is latched OFF for the rest of this run` | intentional | read the **first** `EvaluateFeature FAILED` line, not the last |

---

### Rung 3 — look at the picture

```ini
sr_copy_back = 1
```

DLSS's output is now copied over `u0` after the game's TAAU has already written it. **This is the
first time you see DLSS's image.** It costs a full-extent 4K copy per frame.

**What to look for, in this order:**

1. **Geometry.** Is the image the right size and in the right place? A wrong `OUTPUT EXTENT` shows
   up as an image occupying a fraction of the screen, or a stretched one.
2. **Shimmer on thin edges under camera motion.** That is the **jitter sign**. It is the single
   most likely bug in this feature and it is *silent* — DLSS runs and reports success.
   The A/B, no rebuild:
   * `sr_jitter_scale_y = -1` — a per-axis Y sign error.
   * `sr_jitter_scale_x = -1` — a per-axis X sign error.
   * **both** at `-1` together — the whole convention.
   If one of these is clearly better, **report which**: the fix belongs in `ue4_jitter.hpp`'s
   contract, not in these keys.
3. **Ghosting/smearing that does not track the camera.** That is the **motion guide**. A/B:
   * `sr_mvec_decode = 0` → the game's raw encoded velocity (expect this to be *worse*).
   * `sr_mv_scale_x = -1`, then `sr_mv_scale_y = -1`, then **both** — the same two distinct tests
     the DLSS-NR path documents. Only "both together" can settle the direction convention.
4. **Brightness.** `sr_hdr` is `0` — `[ASSUMED]`, see §3. Try `sr_hdr = 1`. Note that with
   `sr_hdr = 0` the snippet *silently pins* `DLSS.Pre.Exposure` and `DLSS.Exposure.Scale` to 1.0,
   so there is no exposure contract to get wrong at 0.

---

### Rung 4 — suppress the game's TAA

```ini
sr_suppress_taa = 1
```

The game's TAAU no longer runs; DLSS is the only thing that writes the output.

**Look for:**

```
DLSS-SR: sr_suppress_taa=1. The game's TAA Dispatch is NOT being issued - DLSS replaces it.
--- DLSS-SR @ frame 600: evaluates=600 suppressed_dispatches=600 ...
```

`suppressed_dispatches` must track `evaluates`. If it lags, some frames are falling back — which is
*safe* (the game's TAAU runs and writes every pixel of the output view rect) but means something is
failing intermittently. The one-shot error lines say what.

**`sr_suppress_taa = 1` with `sr_direct_output = 0` AND `sr_copy_back = 0` is refused**, not
obeyed — that combination would stop the game's TAA while DLSS wrote into a texture nothing reads,
leaving the frame holding whatever was last in `u0`. The add-on logs it once and keeps issuing the
game's dispatch.

**What this rung really tests** is the ownership contract. Under suppression the add-on reports
"already issued" to ReShade **only after** `EvaluateFeature` returned `Success` **and** after
`probe::restore_state` has run — so any bail, any NGX failure and any exception leaves ReShade to
issue the game's own TAAU, which unconditionally writes every pixel with no read-modify-write. A
failed frame costs one wasted dispatch, not a garbage frame.

> **This rung changes behaviour for every other add-on in the process.** ReShade's event dispatch
> does not short-circuit — it ORs every callback with no break — so this suppression applies to any
> co-loaded RenoDX/Luma too, and they are given no way to learn it. If you run other add-ons and
> something breaks here, that is why.

---

### Rung 5 — bind `u0` directly

```ini
sr_direct_output = 1
```

Removes the add-on's 4K output texture, the full-extent copy per frame, and both of the copy's
barriers. `u0` carries `TexCreate_UAV` from RDG and already rests in `UNORDERED_ACCESS`, which is
the state NGX wants for `Output`.

**Look for:**

```
DLSS-SR: sr_direct_output=1. The game's own TAA output UAV (u0, ...) is bound DIRECTLY as DLSS's Output.
DLSS-SR: EVALUATE #1 OK. ... Output=the game's u0 DIRECTLY 3840x2160 ...
```

**If this rung breaks and rung 4 did not**, go back to `sr_direct_output = 0`. That is a complete,
working configuration; it just pays for a copy.

---

### Rung 6 — optional: drop the DLSS-NR snippet

```ini
dlss_nr = 0
```

Skips the 166 MB `nvngx_dlssnr.dll` load. SR takes the accepted dispatch either way, so this only
saves load time and address space. Do it last, so that "put `dlss_sr = 0` back" remains a one-line
return to the shipping build at every earlier rung.

---

### Rung 7 — optional: DLAA instead

```ini
sr_perf_quality = 5      ; DLAA. The default 0 = MaxPerf is a TRAP here.
sr_mv_lowres    = 0      ; velocity is no longer lower-res than the output
```
```ini
; Engine.ini
r.ScreenPercentage=100
```

Re-pin `sr_shader_hash` again — `TAA_SCREEN_PERCENTAGE_RANGE` changes too. Expect ~4x the pixel
cost. The `CreateFeature parameters` line will now name `DLSS.Hint.Render.Preset.DLAA` as the slot
the snippet reads, because it picks that slot **by ratio**, not by `PerfQualityValue`.

---

## 3. Assumptions carried into this build

Every one is tagged in the code and in `stray_dlssnr.ini`. These are the ones that can be wrong
*silently* — no error, only pixels.

| what | tier | why it is here | how to settle it |
|---|---|---|---|
| `r.SecondaryScreenPercentage` is honoured from `[SystemSettings]` | **[ASSUMED]** | never tested on the shipping build | rung 0 |
| the output view rect equals `8 * group_count` | **[ASSUMED]** | `GetGroupCount` is `DivideAndRoundUp`, so the true value is in `(8gx-8, 8gx]`; it is exact whenever the output is a multiple of 8, which every display resolution is | the `OUTPUT EXTENT` log line prints both the band and the u0 texture; `sr_out_width`/`sr_out_height` pin it |
| `IsHDR` (`sr_hdr = 0`) | **[ASSUMED]** | nothing in the snippet bears on whether STRAY's colour buffer is HDR — it is a property of the game's TAA pass. The **conservative** choice was taken: with `IsHDR` clear the snippet pins `DLSS.Pre.Exposure`/`DLSS.Exposure.Scale` to 1.0, so there is no exposure contract to get wrong | rung 3, item 4 |
| `MVJittered` (`sr_mv_jittered = 0`) | **[ASSUMED]** | UE4's velocity buffer is believed not jitter-compensated; never measured | A/B at rung 3 |
| `ViewRectMin == (0,0)` in the motion decode | **[ASSUMED]** | `SetupViewRect` forces `OutputViewRect.Min = (0,0)` for upsampling configs and `SceneRendering.cpp` shifts every view rect to the top-left. The same assumption the DLSS-NR path already makes | a non-zero `ViewRectMin` shows up as a uniform smear that does not vary with camera motion |
| the snippet's own log sink is **not** wired | **[ASSUMED / deliberately skipped]** | `NGXDLAA::Init` reads `Log.Callback` out of the parameter block during `Init_Ext`, and routing it into `ReShade.log` would carry every internal error with its `dlaa.cpp` line number — the cheapest diagnostic on this path. It is **not implemented** because the callback's ABI signature was never verified against this binary, and handing NGX a wrong-arity function pointer is a crash, not a log line. Worth doing once that signature is measured | measure the callback's arity in the disassembly first |
| `DLSS.Use.HW.Depth = 1` is right for STRAY | **[SRC + HW]** — *not* assumed | the parameter is read at CREATE into the creation struct at `+0x24` and its absent branch stores `0 = Linear`; it reaches the network config as `HW_Depth`. STRAY's `t0` is a `r32_g8_typeless` hardware depth-stencil | it is set explicitly, always. What the network does *differently* at 0 vs 1 has not been traced — A/B once SR renders |

### Where the two research passes disagreed, and which one won

**`NGX_DLSS_GET_OPTIMAL_SETTINGS`.** The task brief stated the query *cannot* work through this
add-on's parameter block, because `GetVoidPointer("DLSSOptimalSettingsCallback")` returns null and
the block is the add-on's own `unordered_map`. The disassembly pass found
`NVSDK_NGX_D3D12_PopulateParameters_Impl` at `0x18002ccb0` doing exactly two `Set(void*)` calls —
writing that very callback **into whatever block it is handed**.

**The disassembly won** (a cited instruction address beats a stated inference), so the query *is*
implemented — but behind `sr_optimal_settings = 0`, on a **scratch** parameter block, and never on
any path that matters. Both sources agree on that last part: the query returns a *recommendation*
and has no power to make UE render at that resolution. If it turns out the brief was right after
all, the add-on says so in one line and nothing depends on it.

---

## 4. Reverting

At any rung: `dlss_sr = 0`. That is the whole revert. The build is then bit-identical in behaviour
to the DLSS-NR build that ships today — `nvngx_dlss.dll` is not loaded, no SR resource is created,
slot B of the trampoline stays null, and the cost is a handful of predictable branches per frame.
