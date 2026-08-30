# STRAY DLSS-NR — a ReShade add-on

Runs NVIDIA's DLSS Neural Rendering denoiser (NGX feature 18, `nvngx_dlssnr.dll`) on STRAY's
resolved post-TAA colour buffer, under D3D12.

Built from the STRAY DLSS-NR probe. The probe's shader identification, descriptor shadow and
root-signature 1.1 handling are carried over unchanged — they were measured working on the real
game — and extended with UAV resolution, a D3D12 state save/restore, and the NGX lifecycle.

## What changed in this revision

The pass itself — identification, SRV/UAV resolution, RS 1.1 handling, `capture_state` /
`restore_state`, the re-issued game dispatch — is **untouched**. Two targeted additions sit on top
of it, and each has its own ini switch so it can be A/B'd on hardware.

**1. The HDR colour codec** (`hdr_codec`, default `1`) — this was §6 gap 1, *"the largest
correctness gap"*, and it is the *"a bit dark"* symptom. DLSS-NR is a **display-referred** image
network and it was being handed unbounded linear UE4 SceneColor. Two compute dispatches of the
add-on's own now wrap the evaluate, ported from the working dxvk-remix `rtx.neuralRendering`
deployment constant for constant:

```
proxy  = SrgbEncode(SoftClip(original * s))    -> this, not SceneColor, is DLSSNR.Color
neural = DLSS-NR(proxy, depth, mvec)
result = original + (neural - proxy) / s       -> additive residual; alpha from the ORIGINAL
```

The residual form is not cosmetic. It makes the transfer an **exact identity** when the network
changes nothing (`neural == proxy` ⇒ the delta is exactly `+0.0` ⇒ the pixel comes back bit for
bit), it preserves HDR above the soft-clip knee because the original is added to rather than
reconstructed, and it has exactly one division — by a scale clamped to `[1e-6, 1e6]`.
`transfer_strength = 0` is therefore an exact bypass **of the denoise** — the encode, the evaluate
and the decode all still run — so that run must be pixel-identical to `copy_back = 0`. It is *not*
identical to `hdr_codec = 0`, which is a different image entirely (see §6 gap 1). That comparison
is the cheapest on-hardware check that the whole path is wired up correctly.

**2. The temporal-feedback fix** (`history_restore`, default `1`) — this was §6 gap 5. UE 4.27's
`AddTemporalAAPass` uses **one** texture as both the pass output and the next frame's history
(`TemporalAA.cpp:696`), so the denoised image was re-entering the game's own accumulator at a
history weight of 0.96 per frame — roughly 25 passes of the denoiser in steady state. The add-on
now keeps a private copy of the *pre-denoise* TAA output and writes it back over that resource at
the start of the next accepted dispatch, **after verifying** the resource really is bound as a
colour SRV there. The contract: *the game's TAA never sees a denoised pixel; post-processing only
ever sees denoised pixels.*

**3. The motion-vector decode** (`mvec_decode`, default `1`) — this was §6 gap 2. The snippet wants
**absolute pixels on the colour grid, y-down**; UE4 packs screen-space velocity into a
normalised-integer texture with a scale **and a bias**, which `MVecScaleX/Y` can never remove — so
DLSS-NR was running on a meaningless motion guide. A third compute dispatch now applies UE 4.27's
own `DecodeVelocityFromTexture` and, **wherever the velocity texel is the cleared sentinel**,
reconstructs camera motion by reprojecting depth through `View.ClipToPrevClip`.

That second half is the one that matters: UE writes velocity only for movable primitives that
actually moved, so a naive decode would hand DLSS **zero motion for the static world, the sky and
all translucency** — worse than the status quo. `View.ClipToPrevClip` is read CPU-side from the
game's own View uniform buffer at a row that must agree **two independent ways** (the buffer's
content signature and this game's TAA bytecode), and the pass **refuses to run** if they disagree
rather than reprojecting the world through the wrong four rows. `MVecScaleX/Y` are then forced to
`1.0` so the grid correction cannot double-apply.

All three are fail-open. If a feature's shaders cannot be compiled, or its textures cannot be
allocated, or — for the motion vectors — the View constant buffer cannot be located and validated,
that feature latches off with a logged reason and the add-on runs **exactly as it did before it
existed**. The motion-vector ladder in particular has no rung that is worse than `mvec_decode=0`.

---

## 1. Install

Build (macOS/Linux, needs `mingw-w64`):

```sh
./build.sh
```

That produces `stray_dlssnr.addon64` and `remix_nvngx.dll`, and verifies that every trampoline
forwarder makes a real `call` rather than a tail jump — see §5.

Copy **all four** files next to the game executable, alongside the ReShade DLL:

```
S:\common\Stray\Hk_project\Binaries\Win64\
    stray_dlssnr.addon64     this build
    remix_nvngx.dll          this build — REQUIRED, see §5
    nvngx_dlssnr.dll         the patched DLSS-NR snippet
    stray_dlssnr.ini         optional; every key defaults to the value shown in the file
```

Requires **ReShade 6.8.0 with add-on support**. Launch STRAY with `-dx12`.

The add-on refuses to load against a different ReShade API version rather than downgrading. Event
ids in `reshade_events.hpp` have no explicit enumerators, so they are positional and valid only
for the exact header revision compiled against; registering against a mismatched runtime would
silently wire callbacks to the wrong events.

### First run

Set `copy_back = 0` in the ini before the first launch.

The whole path still executes — barriers, `CreateFeature`, `EvaluateFeature`, the state restore,
and both of the add-on's own compute dispatches — but the denoised result goes to a texture nothing
reads. If the game still renders correctly, that is positive evidence the state restore is
faithful, *independently of image quality*. Only set `copy_back = 1` once that holds.

Then, in order:

1. `copy_back = 1`, `hdr_codec = 1`, `transfer_strength = 0`. This is the **exact bypass of the
   denoise**: the codec runs end to end — encode, `EvaluateFeature`, decode, state restore,
   copy-back — but the decode's transfer is `lerp(original, graded, 0) = original`, so what is
   written back over the frame is the untouched pre-denoise TAA output. The frame must therefore
   be **pixel-identical to the previous step (`copy_back = 0`), i.e. to the game with the add-on
   unloaded**. If it is not, the codec's plumbing is wrong and nothing after this is worth
   looking at.

   > Do **not** compare this against `hdr_codec = 0`. Those two settings copy back completely
   > different images by construction and can only ever differ: `transfer_strength = 0` returns
   > the game's own frame, while `hdr_codec = 0` returns the network's raw display-referred
   > answer — the darkened image of gap 1. An earlier revision of this README, of the ini and of
   > the runtime log all stated that check the wrong way round; a correctly wired build fails it.
2. `transfer_strength = 1`. Now judge the image. If it is too dark or too bright, tune
   `paper_white_scale` (§2 — **raising it darkens**). If it has a colour cast rather than a
   brightness error, lower `color_strength`.
3. `history_restore = 0` versus `1`, with everything else fixed. The difference should show up as
   ghosting and over-smoothing that accumulates over seconds with the fix off, and does not with it
   on. Check the census line for `applied` climbing and `dropped` flat.

---

## 2. Configuration

`stray_dlssnr.ini`, next to the add-on. The shipped file documents every key inline. A missing
file is not an error — every default is the shipping default. Every recognised key, and every
unrecognised one, is echoed into `ReShade.log` at startup, so a typo is visible rather than
silently taking a default.

| Key | Default | Notes |
|---|---|---|
| `enabled` | `1` | `0` = strict no-op on the render path |
| `diagnostics` | `1` | the probe's read-only log output |
| `shader_hash` | `0x1708ec956099e259` | the primary measured TAA pass; `0` = any shader passing all census gates (not recommended) |
| `srv_depth` / `srv_velocity` / `srv_colour` | `0` / `2` / `5` | t-registers on that shader |
| `uav_output` | `0` | u-register carrying the resolved colour |
| `copy_back` | `1` | `0` for bring-up |
| `hdr_codec` | `1` | the display-referred proxy/residual codec; `0` = feed the network raw linear SceneColor (the old behaviour) |
| `paper_white_scale` | `1.0` | the codec's scale `s = 1/max(v, 0.01)`. **UNCALIBRATED — see below** |
| `transfer_strength` | `1.0` | global lerp back to the original; `0.0` is an **exact bypass of the denoise** — identical to `copy_back = 0`, *not* to `hdr_codec = 0` |
| `color_strength` | `1.0` | `0.0` keeps the original's chromaticity and transfers only luminance |
| `history_restore` | `1` | break the TAA feedback loop; `0` = the old behaviour. Inert with `copy_back=0` |
| `restore_graphics_root` | `1` | replay graphics root state too |
| `require_trampoline` | `1` | refuse to run without `remix_nvngx.dll` |
| `populate_parameters` | `0` | call the snippet's `PopulateParameters_Impl` |
| `depth_inverted` | `1` | UE4 reversed-Z — **inferred, not measured**, see §6 |
| `mvec_decode` | `1` | decode UE4's velocity encoding and reconstruct camera motion into absolute colour-grid pixels; `0` = the old raw encoded buffer (§6, gap 2) |
| `mvec_reconstruct` | `1` | reconstruct camera motion from depth where the velocity texel is invalid. `0` = decode only, invalid texels **exactly zero** — a bring-up A/B, **worse than `mvec_decode=0`** for play |
| `mvec_dilate` | `0` | UE's `AA_CROSS` nearest-depth velocity dilation; off because DLSS does its own neighbourhood work |
| `mvec_clip_row` | `0` | pin the `View.ClipToPrevClip` float4 row. `0` = discover **and** cross-check against this game's own TAA bytecode |
| `mvec_clip_transpose` | `0` | read `ClipToPrevClip` transposed — the escape hatch for the matrix convention |
| `mvec_scale_x` / `mvec_scale_y` | `0` / `0` | `0` = derive from extents (forced to `1.0` when `mvec_decode=1`). With the decode on, `-1` is the per-axis **sign A/B** |
| `intensity`, `local_tone_strength`, `local_structure_strength` | `1.0` | the snippet's own fallbacks |
| `skin_structure_strength` | `-1.0` | negative = inherit local structure strength; `0.0` is **not** neutral |
| `style` | `0` | uint |
| `use_auto_mask` | `1` | gates both structure strengths |

The five tuning knobs default to the snippet's **own internal fallbacks**, recovered from its
disassembly. `1.0` is a fallback, **not a calibrated neutral midpoint**, and the scale these
values sit on is not known. Change them one at a time.

### `paper_white_scale` is a tuning knob with no calibrated value

There is **no measurement** behind the `1.0` default and it **needs tuning on hardware**. In the
Remix deployment this scale folds in that renderer's own auto-exposure and the user's EV bias;
STRAY exposes no equivalent to an add-on, so here it is a plain constant.

The semantics are Remix's, which means it is a **divisor**: `s = 1.0 / max(paper_white_scale, 0.01)`
and the encode computes `original * s`. So:

* **raise** it if the image looks blown out — highlights crushed into the soft-clip shoulder;
* **lower** it if the image looks black.

At the default of `1.0` the divisor and multiplier conventions coincide exactly, so `1.0` means
"no rescale" either way. Useful range is roughly `0.01 .. 64`. The first evaluate logs the value,
the derived `s`, and this caveat.

If the image comes back with a colour cast rather than a brightness error, `color_strength` is the
knob: at `0.0` the original's chromaticity is kept exactly and only the network's luminance change
is transferred.

There is deliberately no preset selector. The snippet ships exactly one network — its weight
registry is one entry wide and the accessor hardcodes `cmp rcx, 1` — so preset 1 is hardcoded.

---

## 3. What success looks like in `ReShade.log`

Grep for `DLSS-NR`.

**Startup — the snippet loaded and NGX initialised:**

```
DLSS-NR: reading configuration from ...\stray_dlssnr.ini
DLSS-NR: loaded nvngx_dlssnr.dll and routed every call through remix_nvngx.dll
==================================================================
DLSS-NR ARMED. feature id 18, preset 1 (the only network in this snippet build).
  target shader   0x1708ec956099e259
  registers       depth=t0 velocity=t2 colour=t5 output=u0
  ...
==================================================================
```

plus, when `hdr_codec = 1`, the codec's own bring-up:

```
DLSS-NR: HDR codec encode compiled to NNNN bytes of cs_5_0 DXBC and cached as stray_dlssnr_encode.<hash>.dxbc.
DLSS-NR: HDR codec decode compiled to NNNN bytes of cs_5_0 DXBC and cached as stray_dlssnr_decode.<hash>.dxbc.
DLSS-NR: HDR codec pipelines created (encode + decode, cs_5_0 DXBC, [numthreads(16,16,1)]).
  hdr codec       hdr_codec=1 paper_white_scale=1.0000 (UNCALIBRATED) transfer_strength=1.000 color_strength=1.000
  feedback fix    history_restore=1 (the pre-denoise TAA output is written back over the game's history before it is read)
```

A `WARN` line follows naming the remaining input-side gap (§6, gap 2) and, when the depth buffer is
typeless, gap 3. `TEMPORAL FEEDBACK` still appears once after a few frames when `copy_back=1` — it
is the independent detector, and with `history_restore=1` its own text says the loop is mitigated
(§6, gap 5).

**The TAA pass was found and accepted** — note `output u0` is the *resolved* colour, which is what
is denoised; the `colour t5` SRV is an *input* to TAA:

```
DLSS-NR: TAA pass located and accepted.
  shader   0x1708ec956099e259 (compute, sm 5.0)
  depth    t0   res=0x... r32_g8_typeless 1920x1080
  velocity t2   res=0x... r16g16b16a16_unorm 1920x1080
  colour   t5   res=0x... r16g16b16a16_float 1920x1080  (an INPUT to TAA; not what is denoised)
  output   u0   res=0x... r16g16b16a16_float 1920x1080  (the RESOLVED colour; this is DLSSNR.Color)
  resolved SRV classes at this dispatch: colour=2 depth=1 velocity=1
```

**Both ABI identity checks passed** — these gate the state restore and must both appear:

```
DLSS-NR: descriptor_table handle identity VERIFIED - ...
DLSS-NR: descriptor heap identity VERIFIED - ...
```

**The feature was created and is evaluating:**

```
DLSS-NR: created the output texture, 1920x1080 r16g16b16a16_float, UAV + SRV + copy source (...). The TAA output UAV is ... The codec is on, so this texture is the network's target only ...
DLSS-NR: created the pre-denoise copy, 1920x1080 r16g16b16a16_float (copy src/dst + SRV)
DLSS-NR: HDR codec resources ready at 1920x1080 - proxy r16g16b16a16_float, neural target r16g16b16a16_float, result ...
DLSS-NR: CreateFeature(feature 18) succeeded at 1920x1080, preset 1.
DLSS-NR: evaluate #1 OK. colour/output 1920x1080, depth 1920x1080 (r32_g8_typeless), ..., hdr_codec=1, history_restore=1.
DLSS-NR: HDR CODEC ACTIVE. DLSSNR.Color is the display-referred PROXY (res=0x..., r16g16b16a16_float), ...
DLSS-NR: IDENTITY IS EXACT, algebraically. ... The pixel comes back BIT FOR BIT ...
DLSS-NR: evaluate #100 OK. ...
```

**The `#1` and `#100` lines are the point.** A frame looks identical whether or not the pass ran,
so the absence of an error is not evidence of success. If you do not see `evaluate #1 OK`, the
pass is not running, and one of the messages in §4 will say why. `hdr_codec=1` on the evaluate line
is what says the network is being shown the proxy rather than raw SceneColor — the ini key being
`1` is not enough on its own, since the codec latches itself off if it cannot be built.

**The two new features report themselves once, and then only through the census:**

```
DLSS-NR: HISTORY RESTORE ACTIVE - the pre-denoise TAA output has been written back over res=0x...,
         which is resolved as a colour SRV at t6 on this very dispatch, i.e. it IS this frame's
         HistoryBuffer[0]. ... the feedback loop of README gap 5 is BROKEN.
--- DLSS-NR history restore @ frame 1800: applied=1793 dropped=0 (history_restore=1 copy_back=1 hdr_codec_running=1 pristine=allocated)
```

---

## 4. Failure modes

Every one of these is logged once, loudly, with the remedy. The add-on **never guesses** and never
half-runs: every refusal happens before the game's dispatch is issued, so a refused frame is
byte-identical to having no add-on installed.

| Log line | Meaning | Fix |
|---|---|---|
| `nvngx_dlssnr.dll was not found next to the add-on` | expected for a stock install; logged at info, not error | ship the snippet |
| `remix_nvngx.dll is missing or does not export RemixNgxTrampoline_SetSnippet` | the caller gate would reject every gated call | ship `remix_nvngx.dll` |
| `FAIL_PlatformError` from `Init_Ext` or `CreateFeature` (`0xbad00002`) | the snippet's caller check rejected us | the trampoline is missing, or its forwarders tail-jump — rebuild, `build.sh` verifies this |
| `FAIL_UnableToWriteToAppDataPath` | the add-on's directory is not writable | the snippet wants to write its log there |
| `its bound SRVs do not describe a TAA pass` | the shader matched but the class quorum failed | adjust `srv_*`, or re-run the probe |
| `the configured output register uN is not a usable TAA output` | UAV identification failed; every candidate is listed | set `uav_output` to one of the listed CANDIDATE rows |
| `AMBIGUOUS output UAV` | more than one UAV fits; the configured one was used | check the listed alternatives if the image lands in the wrong place |
| `the D3D12 state-restore plan is INCOMPLETE` | the restore would be partial, so the pass is skipped entirely | see §5; the reason is named |
| `descriptor_table handle identity MISMATCH` | ReShade does not pass the GPU descriptor handle through unchanged | pass permanently off; re-check against that ReShade build |
| `descriptor heap identity MISMATCH` | the heap is a ReShade wrapper, not the app's | pass permanently off |
| `FAIL_RWFlagMissing` (`0xbad00009`) | the output lacks UAV access | should not happen — the output is created with `unordered_access` |
| `FAIL_UnsupportedInputFormat` / `FAIL_UnsupportedFormat` | almost certainly the typeless planar depth | see §6, gap 3 |
| `d3dcompiler_47.dll / D3DCompile is not available` | the codec's shaders cannot be built here | drop a precompiled `stray_dlssnr_encode.dxbc` / `_decode.dxbc` beside the ini, or set `hdr_codec = 0`; everything else still runs |
| `D3DCompile(cs_5_0) FAILED for the HDR codec ... shader` | the compiler's error blob is printed verbatim | as above; the codec latches off and the pass runs without it |
| `the HDR codec could not be built - create_pipeline...(...) failed` | root signature or PSO creation was refused | codec off for the run; the denoise still runs and is still written back |
| `hdr_codec=1 but the codec is NOT running` | a texture or pipeline the codec needs is missing | the parenthetical names which one |
| `HISTORY RESTORE SKIPPED` | the armed resource is not resolved as a colour SRV at this dispatch, or no longer has the extent/format the copy was taken at | the UE 4.27 history model may not hold; the census counts it as `dropped` — see §6, gap 5 |
| `HISTORY RESTORE REFUSED` | the armed resource came back bound at `srv_colour`, i.e. as this frame's scene-colour **input** rather than the history slot | restoring there would freeze/ghost the frame, so it is deliberately dropped; check `srv_colour`, or set `history_restore = 0` — see §6, gap 5 |
| `create_resource failed for the ... pre-denoise copy` | out of video memory | both new features off for that resolution; the pass is otherwise unchanged |

If `EvaluateFeature` fails, **the game's TAA still ran and the frame is unchanged** — only the
denoise was skipped. The message is printed once, not every frame.

---

## 5. The two things most likely to break

### The NGX caller gate, and the trampoline

Every gated export in the snippet resolves its **caller's** module from the return address and
requires `"nvngx.dll"` as a substring of that module's path, or returns `0xbad00002`. `Init_Ext`
and `CreateFeature` are both gated, so without a correctly-named trampoline nothing works.

`remix_nvngx.dll` exists purely to be that module. Two properties are load-bearing:

1. Its **filename** contains `nvngx.dll`.
2. Each forwarder makes a **real `call`** and then returns. A tail jump would reuse the add-on's
   return address and put the check straight back on `stray_dlssnr.addon64`. The forwarders defeat
   this with a `volatile long` store after each call, plus `-fno-optimize-sibling-calls`, and
   **`build.sh` disassembles the result and fails the build** if any forwarder shows an indirect
   `jmp` or no indirect `call`. Do not ship a build where that check did not print `OK` for all
   nine exports.

The gate cannot be detected at resolve time — `GetProcAddress` succeeds and only the calls fail —
which is why `require_trampoline` defaults to refusing rather than trying and failing obscurely.

### D3D12 state clobbering

NVIDIA states it directly (DLSS Programming Guide 310.6.0 §5.4, p52): *"NGX modifies the Vulkan
and D3D12 command list states. The calling process must save and restore its own Vulkan or D3D12
state before and after making the NGX evaluate feature calls."*

This matters more than usual here because **UE 4.27 will not repair the damage**. Its state cache
is purely dirty-flag driven: after the evaluate, `Compute.bNeedSetRootSignature` is still false,
its cached PSO pointer still matches what it believes is bound, and its descriptor-heap cache
compares against its own shadow — which NGX did not touch — so it issues no `SetDescriptorHeaps`
and never dirties its tables. The next compute dispatch would run UE's root arguments against
NGX's root signature, out of a heap that is no longer bound. That is a device removal or a
silently wrong shader, and it persists until UE opens a fresh command list.

`ID3D12GraphicsCommandList` has 51 methods and **no getters**, so nothing can be read back. The
only options are "shadow every state-setting call and replay it" or "do not restore". This add-on
shadows, modelled on NVIDIA's own published `restorePipeline` from Streamline's manual-hooking
guide, with two deliberate additions: the **graphics** root signature and arguments are replayed
too (Streamline omits them, which is an empirical claim about the DLSS snippets, not a contract —
and a heap change invalidates graphics tables regardless), and the ray-tracing state object is
detected and refused rather than silently dropped.

Replay order is not negotiable — heaps, then each pipe's root signature, then that pipe's
arguments, then the PSO — because changing heaps undefines every table binding and setting a root
signature undefines every root argument.

All of it is issued on the **raw** `ID3D12GraphicsCommandList` from `get_native()`, never through
ReShade's abstraction. ReShade's `push_descriptors` does not map back to
`SetComputeRootConstantBufferView` — it allocates in ReShade's own heap and binds a *table*
instead — and its `bind_descriptor_tables` suppresses its own `SetDescriptorHeaps` whenever its
cache already matches, which it does, because NGX's heap change bypassed ReShade entirely.

**If the restore plan cannot be built completely, the pass does not run at all.** A partial
restore is strictly worse than no injection.

---

## 6. Known gaps

These are real, they are logged loudly at startup, and a reviewer should treat them as open.
Gaps 1 and 5 are now **addressed** and are kept here with what was done and what is still
uncertain, rather than deleted.

### Gap 1 — ~~there is no HDR codec~~ **FIXED, but the scale is uncalibrated**

DLSS-NR is a **display-referred** image network. What used to be bound as `DLSSNR.Color` was UE4
SceneColor: linear, unbounded, upstream of bloom, eye adaptation and the film tone curve. Handing
that to the network is out-of-distribution — the same error that produced the "everything's blue
and red" reports in the Remix deployment, and the *"a bit dark"* reported here.

**This is now implemented** (`hdr_codec = 1`), ported from the working dxvk-remix integration:
`neural_rendering_codec.slangh`, `neural_rendering_encode.comp.slang`,
`neural_rendering_decode.comp.slang` and the dispatch order in `rtx_neural_rendering.cpp`. Every
constant is the Remix value — the exact piecewise sRGB curve (deliberately **not** `x^2.2`, and
deliberately not a `_SRGB` view format), the soft-clip knee `0.75` and shoulder `5.770780`
(deliberately C0-but-not-C1, because that is what the only known working deployment ships), the
`[1e-6, 1e6]` scale clamp, the `65504.0` output clamp, the `0.001` chroma floor and the BT.709
luma weights.

Three properties of the transfer, all load-bearing:

* **Identity is bit-exact.** `neural == proxy` gives a delta of exactly `+0.0`, so a pixel the
  network did not change comes back unchanged, for every value of `s`, `transfer_strength` and
  `color_strength`. The decode re-reads the proxy **out of the FP16 texture the network was
  actually given** rather than recomputing it, which is what makes this independent of
  `SrgbDecode` being the exact inverse of `SrgbEncode`.

  This is a claim about **bits**, so it has a premise: `InProxy` and `InNeural` must be the same
  format. When the codec is on, the add-on therefore allocates the network's target
  `r16g16b16a16_float` to match the proxy, **regardless of the TAA output's own format** — which
  `classify_format` admits may be `r11g11b10_float`. That is what Remix does
  (`rtx_neural_rendering.cpp:108`/`:115` both use `VK_FORMAT_R16G16B16A16_SFLOAT`), and without it
  an identity network still leaves a channel-asymmetric quantisation floor (~`2^-7` in R/G,
  `2^-6` in B) that gets divided by `s` and added to every pixel — a colour cast and a per-pixel
  noise floor. The format coupling to the TAA output only ever existed for `CopyTextureRegion`,
  and with the codec on the copy source is `result_tex`, which is still in the TAA output's
  format. The startup log prints both formats next to the identity claim.
* **HDR survives.** The original is never scaled, clipped or reconstructed — it is *added to*, so
  values above the soft-clip knee are preserved.
* **Alpha comes from the original**, never from the network. DLSS-NR is an RGB network and its
  alpha output is meaningless; writing it is what destroyed portal-particle transparency in Remix.

**What remains open here is the scale**, `s`. It has no calibrated value for STRAY — see
§2 *"`paper_white_scale` is a tuning knob with no calibrated value"*. Remix derives it from its own
auto-exposure; nothing equivalent is exposed to a ReShade add-on, so it is an ini constant that
needs a pass on hardware.

The shaders are compiled at load with `D3DCompile` to `cs_5_0` DXBC (see §8). If that is not
possible on a given Proton build, the codec latches **off** with the compiler's error blob printed
verbatim, and the add-on falls back to exactly the pre-codec behaviour.

### Gap 2 — ~~motion vector encoding is not converted~~ **FIXED** (`mvec_decode = 1`)

The snippet expects **absolute pixels on the colour grid, y-down**. STRAY's `t2` is
`r16g16b16a16_unorm`; UE4 packs screen-space velocity into it with a scale **and a bias**, so the
raw texture is not in those units at all. `DLSSNR.MVecScaleX/Y` rescales a grid but **cannot
remove a bias**, so it cannot fix this. Until this landed, the pass ran and reported success while
the motion guide was meaningless.

There are **two halves**, and the second is the one that matters most.

#### (a) The decode

UE 4.27 `Common.ush:1537-1570`:

```hlsl
EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
V.xy        = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv   // decode
InvDiv      = 1.0f / (0.499f * 0.5f)                                // 4.00801611f
```

`InvDiv` in float32 is `0x408041AB` — **bit-identical** to `kVelocityDecodeScaleBits`, the constant
this project's Gate B already matches **inside STRAY's own DXBC**, and which is a hard reject. The
shipped game demonstrably carries it. **[HW]**

**The bias is not `0.5`.** It is `32767/65535 = 0.49999237…` (`0x3EFFFF00`), folding in the decode
to a MAD constant `2.00397754f` (`0x4000412B`). Epic's comment at `Common.ush:1539` says why
`0.499` and not `0.5`: it keeps the **clear colour `(0,0)` outside the encodable range** so it can
be a sentinel. Verified numerically on the build host — over the whole `V ∈ [-2,2]` range the
encoded `.x` lands in `[0.00099236, 0.99899238]`, u16 `[65, 65469]`, so exactly-zero is unreachable
for a texel UE actually wrote. A `0.5` scale would land on exactly `0.0` at `V = -2` and collide.

**The units are not pixels.** They are an **NDC delta, span 2.0, current − previous, Y axis UP**
(`Common.ush:1535`; `VelocityCommon.ush:11-18`).

> **The 4-channel `unorm` format is not caused by `r.BasePassOutputsVelocity`.**
> `VelocityRendering.cpp:354-358` picks `PF_A16B16G16R16` **iff the shader platform supports ray
> tracing**, else `PF_G16R16`. On a 4090/DX12 platform that is the ray-tracing branch. An earlier
> revision of this README attributed the format to the CVar; the observed format was right and the
> stated cause was wrong. Only `.xy` are read here either way.

#### (b) The velocity buffer is sparse — the half people get wrong

UE writes the velocity texture only where it decided to; elsewhere the texel is the cleared
`(0,0,0,0)` (`VelocityRendering.cpp:363`, `FClearValueBinding::Transparent`) and UE's own TAA falls
back to reprojecting depth through `View.ClipToPrevClip`. The validity test is exact:

```hlsl
bool DynamicN = EncodedVelocity.x > 0.0;      // TAAStandalone.usf:2004
```

**Red channel, strict `>`, on the raw encoded sample.** Six separate UE 4.27 consumers spell it
identically. Not `.y`, not `any()`, not the decoded value.

**`r.BasePassOutputsVelocity=1` does not make the buffer dense.** `BasePassPixelShader.usf:979`
zeroes `GBuffer.Velocity`, `:985` gates the real value on `GetPrimitiveData(...).OutputVelocity > 0`,
and `:997-1000` zeroes it again when `DrawsVelocity == 0`; `VelocityRendering.cpp:456-460` returns
false for any primitive whose transform equals its previous one. What the CVar changes is *where*
moving geometry is rasterised — base-pass MRT instead of a separate pass, which is what lets
world-position-offset materials produce velocity at all — not *whether* static geometry does.
Static opaque, sky, unmoved movables, translucency and particles all stay at exactly zero.

So a naive decode hands DLSS **zero motion for the majority of the screen**, which is worse than the
status quo. Where the texel is invalid the pass runs UE's own reconstruction verbatim
(`TAACommon.ush:348-356`):

```hlsl
ThisClip   = float4(ScreenPos, DeviceZ, 1);
PrevClip   = mul(ThisClip, View.ClipToPrevClip);
BackN      = ScreenPos - PrevClip.xy / PrevClip.w;
```

This produces the *same quantity in the same units* as the decode — provable from UE's own code
rather than argued: `TAAStandalone.usf` assigns one into the other's variable with no conversion.

#### The output contract, and the sign trap

```
mvec_px = BackN * float2(-0.5 * ViewW, +0.5 * ViewH)     // y-down colour-grid pixels
```

so that `current_pixel + mvec == previous_pixel`.

**The X channel is negated and the Y channel is not.** Two flips that cancel on Y and compound on
X: DLSS wants previous-minus-current while UE stores current-minus-previous (negates **both**), and
UE `ScreenPos` is y-**up** while the pixel grid is y-**down** (negates **only Y**). A naive "flip
the Y" gets *both* signs wrong and still half-works — exactly the "kind of works but smears"
failure. This is algebraically identical to what NVIDIA's own UE plugin ships
(`VelocityCombine.usf:195-197`).

**The DLSS direction itself is `[WEB]`, not measured here.** It comes from the DLSS Programming
Guide and from NVIDIA's plugin; it has **not** been confirmed against DLSS-NR (feature 18)
specifically. That is why it is exposed rather than welded in — `mvec_scale_x = -1` /
`mvec_scale_y = -1` flip either axis through NGX's own parameter with no rebuild.

#### Where `ClipToPrevClip` comes from

The four float4 rows are read **on the CPU** out of the game's own View uniform buffer — the root
CBV the add-on already captures as `{ID3D12Resource*, offset}` at `push_descriptors` — using
`ue4_jitter.hpp`'s validated discovery, vendored into `src/`. The row is required to agree **two
independent ways**: the constant buffer's own content signature (projection anchor + 94) and this
game's TAA bytecode analysed by the probe. STRAY measures **122** both ways. A disagreement
**refuses** the reconstruction rather than reprojecting the world through the wrong four rows,
which would be confident, coherent and completely wrong.

Reading it CPU-side (64 bytes/frame) rather than binding the game's `b1` to our shader is
deliberate: binding it would make the row an assumption instead of a validated fact, and the
documented worst case — reading `$Globals` at `b0` — produces plausible numbers from the wrong
frame with no diagnostic.

#### The fallback ladder

**Every** failure lands on today's behaviour — the game's raw encoded velocity bound as
`DLSSNR.MVec` with the derived grid scale — except the one the user explicitly asked for. The pass
can never make the add-on worse than `mvec_decode=0`.

| trigger | result |
|---|---|
| `mvec_decode=0` | raw — bit-for-bit today, gap-2 warning and all |
| shader or PSO could not be built | raw, run-latched |
| `r16g16_float` target could not be allocated | raw, per-resolution latch |
| the game's velocity/depth SRV handles not recovered | raw |
| no View CB / discovery failed / **the two clip rows disagree** | raw when `mvec_reconstruct=1`; decode-only when `mvec_reconstruct=0` |
| per-frame CB read failed | **keep the last good matrix**; latch off after 30 consecutive |
| the four rows fail the plausibility test | **keep the last good matrix**; latch off after 30 consecutive |
| a permanent View-CB latch fires *after* a good frame | raw — the cached matrix is **dropped**, never frozen |
| `EvaluateFeature` fails 8 frames running with the decoded guide bound | raw, run-latched — the binding reverts to exactly the pre-decode one and NGX gets one Reset frame |
| `mvec_reconstruct=0` | decode only, invalid texels exactly zero |

Staleness is **bounded everywhere**. The last-good-matrix behaviour belongs only to the two
*transient* per-frame paths above, and both give up after 30 consecutive failures. A **permanent**
latch — no root CBV at `b1`, a CBV that does not resolve to a readable buffer, the clip row out of
bounds — drops `clip_ok` with it, so it lands on `raw` rather than reprojecting the static world
through a matrix frozen at whatever the last good frame held. That failure would be *coherent* and
*camera-independent*, i.e. strictly worse than `mvec_decode=0`, and the census line would still
report a healthy run.

The `EvaluateFeature` rung exists because `mvec_decode` defaults to `1`: the pass changes both the
resource **and** the DXGI format handed to the snippet, and D3D12 acceptance of a 2-channel guide is
not measured. Without a rung, a rejection would leave `evaluated` false every frame — no copy-back,
**no denoise at all** — for the whole session. It now reverts to the binding that works today.

A missing `ClipToPrevClip` falls back to **raw**, not to decode-only, on purpose: decode-only hands
DLSS zero motion for the entire static world, which is the failure this feature exists to prevent.
Today's raw guide is at least uniformly wrong rather than confidently wrong in one region.

When the pass runs, `MVecScaleX/Y` are **forced to exactly 1.0** — the pass already emits absolute
colour-grid pixels, so letting the derived grid ratio through would double-apply it. In STRAY that
ratio happens to be 1.0 today (colour and velocity are both 1920×1080), so a stale ratio would be
invisible here and would come back as a silent 2× error the moment either grid moved. The NGX reset
latch keys on the bound **resource** as well as the extent, because the ladder can swap the guide
between our texture and the game's at an unchanged extent — a change of *units* an extent-only test
cannot see.

#### Validated on the build host

`tools/mvec_selftest.cpp` replays every piece of arithmetic the shader runs, against
independently-computed ground truth. It **restates** the shader's maths rather than sharing code
with it, so a disagreement is a real disagreement. Build and run it on the build host:

```sh
c++ -std=c++17 -O2 -Wall -o /tmp/mvec_selftest tools/mvec_selftest.cpp && /tmp/mvec_selftest
```

**24 assertions, all passing:**

| check | result |
|---|---|
| `InvDiv` bit pattern | `4.00801611` → `0x408041AB` ✓ matches the constant found in STRAY's DXBC |
| folded MAD bias | `2.00397754` → `0x4000412B` (negated `0xC000412B`) |
| bias ≠ 0.5 | `32767/65535` → `0x3EFFFF00`, provably distinct from `0x3F000000` |
| zero sentinel | encoded `.x` ∈ `[0.00099236, 0.99899238]`, u16 `[65, 65469]` — zero unreachable |
| `0.499` is load-bearing | a `0.5` encode gives exactly `0.0` at `V = -2`, colliding with the sentinel |
| encode → unorm16 → decode | worst error `3.076e-05` NDC ≤ one LSB (`6.1158e-05`) |
| quantisation | `0.0587 px` in X, `0.0330 px` in Y at 1920×1080 — irrelevant for a denoiser |
| camera reprojection | worst `|BackN − ground truth|` = `2.98e-08` NDC over 6 world points |
| **transpose discrimination** | untransposed error `0.0`; transposed error `0.370` NDC — the shipped convention is right **and** the wrong one is detectable |
| sign, strafe right | `BackN.x = -0.073` → `MV.x = +70.15 px` — history lies to the **right** ✓ |
| sign, pitch up | `MV.y = -233.83 px` — history **above** ✓ |
| sign, pitch down | `MV.y = +233.83 px` — history **below** ✓ |
| the asymmetry | `BackN = (+0.1,+0.1)` → `MV = (-96.0, +54.0) px` — opposite signs |
| root-constant packing | 128 bytes / 32 dwords; `g_clipToPrevClip` lands exactly on register `c4` |
| `ScreenPos` orientation | top-left `(-,+)`, bottom-right `(+,-)`, centre exactly `(0,0)` — y is up |
| identity matrix | exactly zero motion over a 5×5×4 sample grid (the still-camera check) |

The one thing a host replay **cannot** settle is the DLSS sign convention — see above.

#### Hardware A/B, in this order (all ini-only, no rebuild)

| # | config | expectation |
|---|---|---|
| 1 | `mvec_decode=0` | today, byte-for-byte. The control. |
| 2 | `mvec_decode=1 mvec_reconstruct=0` | moving objects track; static world still smears. Isolates half (a). |
| 3 | `mvec_decode=1 mvec_reconstruct=1` | the target. Static world tracks camera motion. |
| 4 | 3, standing still | no shimmer. Static geometry boiling at pixel level while moving objects look fine is the jitter-double-counting signature. |
| 5 | 3 + strafe **right** | world moves left; must not smear. `MV.x` is positive here. |
| 6 | 3 + `mvec_scale_x=-1` | should be **worse**. If better, the X sign alone is inverted. |
| 7 | 3 + `mvec_scale_y=-1` | should be **worse**. If better, the Y sign alone is inverted. X and Y are **not** symmetric, so both single-axis flips have to be tried. |
| 8 | **3 + `mvec_scale_x=-1` `mvec_scale_y=-1`** | **the direction-convention test — the only one that settles the `[WEB]`-only link.** Should be worse. If it is **better**, DLSS-NR wants *current − previous* and the shipped contract `mvec = BackN * (-0.5W, +0.5H)` must be negated **in the shader**, not left corrected by these keys. |
| 9 | 3 + `mvec_clip_transpose=1` | should be worse, and worse **at the edges**. Identity is transpose-invariant, so a still camera cannot tell. |
| 10 | 3 + `transfer_strength=0` | still pixel-identical to `copy_back=0`. The regression gate for the HDR codec. |

Tests 6 and 7 **cannot** settle the direction convention, and it is a mistake to read them as if
they could. Getting the convention wrong negates **both** axes at once; test 6 leaves Y inverted and
test 7 leaves X inverted, so under a fully inverted guide *both* come out "worse" and match their
printed expectation while the shipped binding is still wrong on both axes. Test 8 is the only row
that reaches the doubly-negated configuration.

### Gap 3 — the depth buffer is typeless and planar

STRAY's `t0` is `r32_g8_typeless`, sampled through an `r32_float_x8_uint` SRV. On D3D12, NGX reads
the format straight off the `D3D12_RESOURCE_DESC` and there is **no channel through which to tell
it the view format** (unlike Vulkan, where the working deployment passes the view format
explicitly). NGX may reject the evaluate with `FAIL_UnsupportedInputFormat` /
`FAIL_UnsupportedFormat`. The add-on warns about this before the first evaluate so the failure is
not a mystery. The fix is a depth conversion pass into a dedicated `R32_FLOAT` texture.

### Gap 4 — `depth_inverted` is inferred, not measured

Defaults to `1` because UE 4.27 renders with reversed-Z. This is the **opposite** of the working
Remix deployment's value, whose renderer writes non-inverted NDC depth. It has not been confirmed
against STRAY. If the denoise ghosts or smears in exactly the wrong direction, flip it first.

### Gap 5 — ~~the denoise feeds back into the game's own TAA history~~ **FIXED**

`copy_back=1` writes the denoised image over the TAA pass's **own output UAV**. In UE 4.27,
`AddTemporalAAPass` creates one texture and uses it as both the pass output and the history —
`TemporalAA.cpp:696` is literally `NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];`
— and `:969` queues that same texture for extraction into `OutputHistory->RT[0]`, which comes back
at `:857` as the next frame's `HistoryBuffer[0]`. So the resource we overwrite **is** the next
frame's TAA history. With `r.TemporalAACurrentFrameWeight = .04` (`:46-50`) the history weight is
**0.96 per frame**, so a pixel passes through the denoiser on the order of 25 times before it
decays out — the steady-state operator is `D^25`, not `D`. That is the over-smoothing and ghosting
that "builds up over seconds".

**The fix (`history_restore = 1`).** The add-on takes a private copy of the *pre-denoise* TAA
output each frame and writes it back over that resource at the **start of the next accepted TAA
dispatch**, before the game's dispatch runs. The game's accumulator then only ever blends its own
un-denoised results, while everything downstream of TAA still only ever sees the denoised image.

Why *that* insertion point, rather than a `present`-time restore or a redirected descriptor:

* it is the one place where the target can be **positively verified** rather than assumed. Three
  checks, all against the resource actually matched at this dispatch — not against the add-on's
  own recorded shape, which is written from the same source as the armed shape and so could only
  ever compare equal:
  1. the armed resource must resolve as a colour-class SRV *at this very dispatch*;
  2. it must be bound at a register **other than `srv_colour`**. A hit there is the TAA pass's
     scene-colour *input* for this frame, not the history slot; writing the previous frame's
     image over it would hand the game's own TAA a stale frame as its current input and freeze
     or ghost the picture, so that case is **refused**, with its own one-shot log line;
  3. it must still carry the extent and format the copy was taken at — `pending_res` is a raw
     `ID3D12Resource` address held across a frame, and UE's render-target pool can recycle that
     address for a differently sized colour texture, which would make the full-subresource
     `CopyTextureRegion` invalid usage rather than an error return.

  If any check fails, nothing is written, the copy is dropped, the census counts it under
  `dropped`, and the log says which check failed. Dropping costs exactly one frame of the
  feedback this fix removes, which is always the safe direction;
* the resource's D3D12 state there is a **known constant**. `FD3D12Resource::DetermineResourceStates`
  gives a UAV-and-SRV, non-RTV texture `ReadableState = NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`,
  and `RHIEndTransitionsWithoutFencing` uses `Resource->GetReadableState()` for *every* readable
  access on the graphics context. That is bit-for-bit `reshade::api::resource_usage::shader_resource`
  (`0xC0`), so the `shader_resource → copy_dest → shader_resource` round trip is net-zero against
  UE's own state tracker;
* nothing can recycle the resource underneath us: `FPooledRenderTarget::IsFree` refuses to hand out
  a target the history's `TRefCountPtr` still holds, and the texture is created
  `ERDGTextureFlags::MultiFrame`. The bytes we leave there are the bytes frame N+1 samples.

Redirecting the *consumers* instead was considered and rejected: there is one resource wearing two
hats, so separating "what post-processing reads" from "what next frame accumulates" would have to
discriminate by **time**, not by consumer, and a descriptor copied once and bound many times cannot
be un-patched.

The existing `TEMPORAL FEEDBACK` detector is **kept exactly as it was** — it is the independent
evidence that the ping-pong is real, and it still fires. Its message now also reports that the loop
is being mitigated when `history_restore` is on. The periodic census carries the running counts:

```
--- DLSS-NR history restore @ frame 1800: applied=1793 dropped=0 (history_restore=1 copy_back=1 hdr_codec_running=1 pristine=allocated)
```

`applied` climbing at one per accepted dispatch with `dropped` flat is the success signature.
`dropped` climbing means the resource we denoised is **not** turning up as a colour SRV at the next
dispatch — i.e. the UE 4.27 history model does not hold for this build and the loop is *not*
broken. Set `history_restore = 0` to A/B against the old behaviour.

**Residual risks, stated plainly.** (a) The `StateBefore` above is derived from engine source, not
measured in STRAY; if it is wrong the barrier is a lie and a debug layer would say so. (b) A second
accepted TAA dispatch in one frame (split screen, a second view) would thrash the single pending
slot; only one shader permutation is ever accepted while `shader_hash` is pinned, and a one-shot
warning fires if it ever happens. (c) The denoised image lives in the resource across the frame
boundary; nothing reads it there, but it is a window.

### Gap 6 — the D3D12 backend on vkd3d-proton is unproven

The Remix deployment that is verified on this hardware drives the snippet's **Vulkan** backend
under DXVK. Loading the snippet and asking its **D3D12** backend to run against a vkd3d-proton
`ID3D12Device` is a materially different proposition, and no measurement exists either way. The
first `Init_Ext` result in the log answers it: `0xbad00002` is the caller gate (fix: the
trampoline); anything else tells you whether the D3D12 backend can see the device at all. If it
cannot, the fallback is `ID3D12DXVKInteropDevice` to recover the `VkDevice`/`VkImage` behind each
resource and drive the verified Vulkan path instead.

---

## 6b. DLSS Super Resolution — `dlss_sr`, default `0`

A second NGX feature now lives in this add-on: **DLSS Super Resolution, NGX feature 1,
`nvngx_dlss.dll`**. It is **off by default**, and the runbook for turning it on is
[`STAGING-sr.md`](STAGING-sr.md), which is written to be walked without help.

### What executes when `dlss_sr = 0`

This matters more than anything else in this section, because DLSS-NR ships and is played on.
With `dlss_sr = 0` the build is **bit-identical in behaviour** to the build before SR existed.
Exactly this much runs:

* the key is parsed and one `bool` is stored;
* `nr_init_device` tests it once and does **not** `LoadLibraryW` `nvngx_dlss.dll`;
* `nr_lazy_ngx_init` tests it once and does not call `Init_Ext` on the SR snippet, does not
  allocate the SR parameter block, and never touches the trampoline's slot B — and when the
  DLSS-NR half of that function fails, it takes the **same early exit it took before SR existed**,
  so the HDR-codec and mvec pipelines are not built on that path either;
* `nr_try_run` tests it once per accepted TAA dispatch, *before* anything SR-related, and takes
  the DLSS-NR branch unchanged;
* `nr_pick_output_uav` tests it once per accepted dispatch and applies the **original**
  "extent must equal the colour SRV's" rule;
* `nr_ensure_output` is called exactly as before (the SR guard short-circuits);
* the teardown path calls `dlss_sr::release_feature` / `destroy_resources` on null handles, which
  is a handful of `!= 0` tests.

No SR resource is created, no SR code reaches the GPU, and `g_sr_armed` is never set. The cost is
a handful of perfectly-predicted branches per frame.

### The trampoline now has two slots, and this is the one deployment trap

The NGX caller gate is a property of the **module a call is issued from**, which is why every gated
export goes through `remix_nvngx.dll`. But that module holds *one set of forwarding pointers per
slot*, so calling `RemixNgxTrampoline_SetSnippet` twice would silently re-point **DLSS-NR's own
calls** at the SR snippet — the shipping feature would break with no diagnostic.

So the trampoline now carries two independent slots:

| slot | claim export | forwarders | snippet |
|---|---|---|---|
| A | `RemixNgxTrampoline_SetSnippet` | `NVSDK_NGX_D3D12_*` | `nvngx_dlssnr.dll` (DLSS-NR) |
| B | `RemixNgxTrampoline_SetSnippetB` | `NVSDK_NGX_D3D12_B_*` | `nvngx_dlss.dll` (DLSS-SR) |

Slot A's pointers, exports and generated code are untouched. `build.sh` and the MSVC CI job both
disassemble **all eighteen** forwarders and assert `call=1, tailjmp=0` on each — a tail jump would
reuse the caller's return address and hand the snippet the ReShade add-on's module identity.

**Redeploy `remix_nvngx.dll` from this build.** An older one has slot A only, and DLSS-SR then
fails with a message that says exactly that.

### What DLSS-SR does differently from DLSS-NR

| | DLSS-NR (feature 18) | DLSS-SR (feature 1) |
|---|---|---|
| `Color` | the TAA pass's **output** (`u0`, resolved), optionally through the HDR codec's proxy | the TAA pass's **input** (`t5`, `InputSceneColor`), at render resolution |
| `Output` | an add-on texture at the colour extent, copied back | an add-on texture at the **output** extent, copied back — or `u0` **directly** (`sr_direct_output`) |
| jitter | not a parameter | **`Jitter.Offset.X/Y`, unconditionally required.** Same hard gate as the four resources |
| motion guide extent | the **output** grid | the **render** grid — SR's colour input is the TAA *input* |
| subrect spelling | `DLSSNRColorSubrectBaseX` (**no dot**) | `DLSS.Input.Color.Subrect.Base.X` (dotted) |
| `DLSS.Use.HW.Depth` | does not exist in the NR snippet | **exists, is read at CREATE only, and defaults to `0 = Linear`** |
| the game's dispatch | always re-issued | re-issued, or **suppressed** (`sr_suppress_taa`) |

Every parameter name the add-on emits was verified against `nvngx_dlss.dll` as an exactly
NUL-delimited string occurring exactly once — 63 of them, all present. The SDK headers were not
used as a source for any name. See the header comment in `src/dlss_sr.hpp` for the method and the
instruction-level citations.

### The one interaction nobody else in the process can see

`reshade::invoke_addon_event` does **not** short-circuit — it ORs every callback with no `break`.
So when `sr_suppress_taa = 1` and this add-on reports "already issued", the TAA dispatch is
suppressed **for every co-loaded add-on too**, and they are given no way to learn it. DLSS-NR never
had this property, because it re-issued. If you run RenoDX or Luma alongside, this is the rung
where something can break for reasons that are not in their logs.

### Proving it ran, rather than merely linked

An earlier pass in this tree compiled cleanly and was dead code because its `build`/`create` were
never called. The SR path is instrumented so that cannot happen quietly:

* `DLSS-SR: EVALUATE #1 OK ...` is printed from the branch **immediately after**
  `EvaluateFeature` returned `Success`. It is unreachable by a feature that only linked.
* the periodic census prints `--- DLSS-SR @ frame N: evaluates=... suppressed_dispatches=...
  mvec_decodes=... geometry=WxH -> WxH`, and `evaluates` is incremented on that same branch.
* every early return names itself once: `DLSS-SR: pass did not run - <reason>`.
* the create path prints its full parameter set, including which
  `DLSS.Hint.Render.Preset.*` slot the snippet will actually read — the snippet chooses that slot
  **by the `Width/OutWidth` ratio**, not by `PerfQualityValue`, so the log can never silently
  disagree with it.

Success is never reported from the absence of an error.

---

## 7. Hard rules this build honours

* **The TAA dispatch is never suppressed.** DLSS-NR consumes *resolved* colour, so it must run
  after TAA. The handler issues the game's dispatch itself, unchanged, then denoises. Every early
  return happens *before* that point and returns `false`, leaving ReShade to issue the dispatch
  exactly as it would with no add-on present.
* **Root signature 1.1 handling is unchanged.** ReShade emits `descriptor_table_with_flags`
  (enum 4) for this title, whose ranges live in a function-local temporary and whose stride is 40
  bytes rather than 28. The probe's deep copy is carried over **byte-identical** — verified by
  diff — because the whole restore is keyed off the `is_table` / `ranges` data it produces.
* **Disabled or snippet-missing is a strict no-op.** No snippet load, no resource creation, no
  render-path effect.
* **Identification is by resolved SRV *classes*, not by confidence score.** The measured false
  positive `0x901e041a7cadc9db` scores confidence 150 with colour=1 depth=2 velocity=0 — a
  depth-consuming pass, not TAA — and would pass any score-based test. The gate is
  `velocity >= 1 && depth >= 1 && colour >= 1` among the SRVs actually *resolved at that
  dispatch*, on top of the exact DXBC hash.
* **Ambiguity is logged, never guessed.** If the configured output UAV is not a usable candidate,
  the pass refuses to run and lists every candidate it found. Writing the denoised image over the
  wrong render target would look like a game bug, not an add-on bug.
* **The add-on's own compute dispatches live INSIDE the existing save/restore window, and the
  restore is not weakened.** `capture_state` still runs before anything is issued; the game's
  dispatch still runs first, against untouched UE state; `restore_state` is still the last
  state-touching call and is still unconditional. What is added is the **cache sync** —
  `bind_descriptor_tables(..., count = 0)`, which is ReShade's own documented escape hatch — before
  each of the add-on's dispatches and once before the restore. ReShade's `command_list_impl`
  caches the bound heaps and root signature and *skips* redundant `SetDescriptorHeaps` /
  `SetComputeRootSignature`, but NGX writes the **raw** list, which ReShade never sees. Without
  that flush the decode would issue a root descriptor table whose GPU handle lives in a heap that
  is not bound: undefined behaviour or a device removal, not an artifact.
* **The add-on creates views only on its own resources.** The decode reads the pre-denoise frame
  out of a private copy rather than through an SRV on the game's texture. A cached descriptor
  naming a resource UE later releases is a dangling read with no diagnostic, and creating one per
  frame instead leaks a slot out of ReShade's CPU descriptor pool every frame. The copy that
  avoids both is needed by the temporal-feedback fix anyway.
* **Every new feature fails open, individually.** Codec shaders that will not compile, a root
  signature or PSO that will not create, any of the four textures that will not allocate — each
  latches the feature that needed it off, prints the reason once, and leaves the rest of the pass
  running exactly as it did before that feature existed.

---

## 8. Build notes

`-std=gnu++17` is required, not `-std=c++17`: `reshade_compat.hpp` relies on `__typeof`, a GNU
extension, to make mingw's `__uuidof` macro parse.

### The codec's shaders are compiled at runtime, on purpose

`src/hdr_codec.hpp` carries the encode and decode HLSL as string literals and compiles them with
`D3DCompile` to **`cs_5_0` DXBC** at load. That is a decision, not a shortcut:

* **This toolchain cannot produce a blob.** The build host is macOS with mingw-w64: no `fxc`, no
  `dxc`. An embedded byte array could only ever be produced on a *different* machine, and then
  nobody on this toolchain could rebuild — or even verify — the shader they were shipping. That is
  the class of unverifiable artifact `build.sh` already refuses (it disassembles the trampoline
  rather than trusting it).
* **DXIL would be worse.** It additionally needs the Windows-only `dxil.dll` signer. DXBC is
  demonstrably accepted in this exact Proton/vkd3d stack — the add-on identifies STRAY's own D3D12
  compute shaders by parsing them as DXBC (`src/dxbc_tokens.hpp`).
* **`d3dcompiler_47.dll` is `LoadLibraryW`'d, never linked.** A load-time import would make the
  whole `.addon64` fail to load when the DLL is absent, taking the working NGX path down with it.
  Under Proton this may be Wine's builtin, whose SM5 compute coverage varies by version — which is
  exactly why a compile failure has to be survivable, and is.

On a successful compile the blob is cached beside the ini as
`stray_dlssnr_encode.<source-hash>.dxbc` / `stray_dlssnr_decode.<source-hash>.dxbc`. The hash is
FNV-1a over the exact source text handed to the compiler, so a stale blob from an older revision
can never be picked up silently. A plain `stray_dlssnr_encode.dxbc` / `stray_dlssnr_decode.dxbc`
(no hash) is honoured as a **user override** — drop one in on a machine whose `d3dcompiler` cannot
build the shader — and the log says loudly when an override is in use.

The shader source is reviewable in-tree, and it round-trips through glslang's HLSL front end
cleanly, but note that **it has not been compiled by `fxc` on this host**. The first launch's log
is the authority on whether `cs_5_0` accepted it.

Two separate C++ ABI hazards are handled, and both are easy to reintroduce:

* **ReShade side.** mingw-w64 g++ uses the Itanium ABI; ReShade is MSVC-built. The two disagree
  about how a member function returns a class **by value**, and three device virtuals do exactly
  that (`get_resource_desc`, `get_resource_from_view`, `get_resource_view_desc`). `msvc_abi.hpp`
  routes them through explicit out-parameter thunks and verifies the vtable offsets at load. Do
  not add further calls to by-value-returning ReShade virtuals without going through it.
* **NGX side.** MSVC numbers an **overload set** in *reverse* declaration order; the Itanium ABI
  does not. `NVSDK_NGX_Parameter`'s 17-slot vtable is therefore laid out **by hand** in
  `ngx_interop.hpp` rather than written as C++ virtuals — writing it as virtuals under mingw would
  put `Set(const char*, ID3D12Resource*)` in slot 6 instead of slot 1, i.e. call
  `Get(const char*, void**)` with the wrong argument count. Editing that array is editing an ABI.

D3D12 COM methods returning an aggregate are safe: mingw defines `WIDL_EXPLICIT_AGGREGATE_RETURNS`
for GCC C++, which turns `GetGPUDescriptorHandleForHeapStart` and `GetDesc` into the explicit
out-parameter form that matches MSVC's convention exactly. Do not `#undef` it.

The add-on implements its own `NVSDK_NGX_Parameter` rather than calling
`NVSDK_NGX_D3D12_AllocateParameters`. The snippet exports no `AllocateParameters` on any backend,
and the SDK fallback would require the **driver's** NGX runtime to be initialised — precisely the
dependency the direct-load design exists to avoid. Resources are set through the
`ID3D12Resource*` slot (slot 1), not the `void*` slot; on D3D12 the parameter map records a type
tag and the snippet's read path looks for the D3D12-resource tag. `DLSSNR.ControlMask` is written
**every frame** with a null resource and a zeroed subrect, because the parameter block is reused
across evaluates and a stale pointer would both dangle and keep `UseAutoMask` forced to 0.
