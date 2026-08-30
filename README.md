# STRAY DLSS-NR — a ReShade add-on

Runs NVIDIA's DLSS Neural Rendering denoiser (NGX feature 18, `nvngx_dlssnr.dll`) on STRAY's
resolved post-TAA colour buffer, under D3D12.

Built from the STRAY DLSS-NR probe. The probe's shader identification, descriptor shadow and
root-signature 1.1 handling are carried over unchanged — they were measured working on the real
game — and extended with UAV resolution, a D3D12 state save/restore, and the NGX lifecycle.

## What changed in this revision

**Newest: chain mode** (`dlss_chain`, default `0`) — DLSS-NR **and** DLSS-SR on one accepted TAA
dispatch, denoise first and upscale second, instead of the two features being mutually exclusive.
See **§6c**. With the key at `0` the build behaves exactly as it did before it existed.

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

**1b. The graft-back is now selectable** (`hdr_graft`, default **`0` — unchanged behaviour**). The
reference add-on carries the answer back a different way, and both now ship so they can be A/B'd
live: `0` is the additive residual above, `1` is its `UpgradeToneMap` — rebuild the pixel from the
network's answer, hue-locked in OkLab with an AP1 clamp. It is one root constant, so the overlay
flips it mid-frame with nothing to rebuild. **The difference is chroma, not brightness**: their
"headroom" term is algebraically our additive residual, both modes deliver the same luminance gain
at every magnitude, and what actually changes is that mode 1 pulls a clipped highlight toward the
white point. Mode 0 remains the default and remains bit-exact — 1,080,000 replayed cases say so.
See §6 gap 1.

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
| `rt_census` | **`0`** | the DXR dispatch census — see below. Off is a strict no-op |
| `rt_census_frames` | `600` | presents between census summary blocks |
| `shader_hash` | `0x1708ec956099e259` | the primary measured TAA pass; `0` = any shader passing all census gates (not recommended) |
| `srv_depth` / `srv_velocity` / `srv_colour` | `0` / `2` / `5` | t-registers on that shader |
| `uav_output` | `0` | u-register carrying the resolved colour |
| `copy_back` | `1` | `0` for bring-up |
| `hdr_codec` | `1` | the display-referred proxy/residual codec; `0` = feed the network raw linear SceneColor (the old behaviour) |
| `paper_white_scale` | `1.0` | the codec's scale `s = 1/max(v, 0.01)`. **UNCALIBRATED — see below** |
| `transfer_strength` | `1.0` | global lerp back to the original; `0.0` is an **exact bypass of the denoise** — identical to `copy_back = 0`, *not* to `hdr_codec = 0` |
| `color_strength` | `1.0` | `0.0` keeps the original's chromaticity and transfers only luminance |
| `hdr_graft` | **`0`** | which graft-back the decode uses. `0` = our additive residual (**default, unchanged behaviour, bit-exact identity**), `1` = the reference add-on's `UpgradeToneMap`. Live — a root constant, nothing is rebuilt. See §6 gap 1 |
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
| `intensity` | `1.0` | **conditional.** The snippet's own fallback. Slider range `[0,1]`; **the ini is NOT clamped**. An *attenuation*, not a gain: `1.0` = **full NR** (not "off") and you drag **down**. *Proven:* every value `>= 1.0` is byte-identical, and below `1.0` the selector moves mode `0 → 1` (the `cmovne` at `0x18001d53d` can only force mode 3 with a ControlMask bound, and this add-on binds a null one). *Not confirmed:* that mode 1 runs — `0x18001f500` gates it on a backend capability bit (`bt eax,0` at `0x1800295ff`) we cannot read |
| `local_tone_strength` | `1.0` | **inert at `style = 0`.** The snippet does clamp it to `[0,1]` itself, so `>= 1.0` is byte-identical to `1.0` — but each of the 14 lerps it feeds is gated by a per-style bitmask (`mov eax, dword [rdx]` at `0x18001d606`) and this build's default mask at `0x1800b0da8` is `0x00000000`. **0 of 14 parameters move at the default style.** See below |
| `local_structure_strength` | `1.0` | **conditional — see below.** Consumed only behind two `dynamic_cast` null tests; not clamped anywhere in the snippet |
| `skin_structure_strength` | `-1.0` | negative = inherit local structure strength; `0.0` is **not** neutral |
| `style` | `0` | uint. Unmeasured on hardware, but **the binary shows two enabled keyed sub-entries, keys `1` and `2`**, carrying local-tone masks `0x34` (3 of 14 params) and `0x20` (1 of 14) against the default record's `0x00000000`. If any index makes `local_tone_strength` bite, it is one of those. Whether `DLSSNR.Style` is the value compared against those keys is **untraced** |
| `use_auto_mask` | `1` | **conditional.** Selects what both structure strengths become (`-1.0f` when off); shares their gate |
| `ui_correction` | `0` | `DLSSNR.UICorrection`. A real parameter of this build (one exact-line match in `nvngx_dlssnr.dll`'s string table; read with a `0xbad00000` guard, fallback `0`). Written per evaluate. **Its visual effect on STRAY is unverified** — a diagnostic knob, not a tuning one |

The five tuning knobs default to the snippet's **own internal fallbacks**, recovered from its
disassembly. Change them one at a time.

**Two of the five are range-explained; three are gate-explained, and the gate is not ours.**

`intensity` and `local_tone_strength` genuinely do nothing above `1.0`, and that is now the top of
their sliders. But note what `intensity >= 1.0` actually does: `fn 0x18001d4d0` is a **mode
selector** returning `0`, `1` or `3`, and mode `0` means *the optional attenuation pass is not
enabled* — which at full strength is correct, because there is nothing to attenuate. Its caller's
`false` is stored to a flag at `0x1800191bd` and the evaluate falls straight through it; **it is
not an abort and the denoise is unaffected.** An earlier revision of this README and of the
overlay said `>= 1.0` "skips the pass entirely" and logged the shipped default as `INERT`. That
was wrong, and it told users their denoiser was off when it was at full strength.

`local_tone_strength` is the one proven range claim: clamped to `[0,1]` at `0x18001d603` before
being used as the lerp coefficient for 14 network parameters at `[rcx+0x124..0x158]`.

**`local_structure_strength`, `skin_structure_strength` and `use_auto_mask` are gated on the
loaded model, not on their values.** The effective pair the snippet computes at `+0xf8`/`+0xfc`
has exactly one reader — an exhaustive scan of every `movss` in `.text` finds three sites per
displacement and the other two are frame locals in a different function — and that reader sits
behind two `dynamic_cast` null tests:

| gate | site | test | target type |
|---|---|---|---|
| network | `0x180021cc8` | `0x18002253f` `test rcx,rcx / je` | `.?AVCCNetwork@HNetCpp@@` |
| layer | `0x18003f5e8` | `0x18003f5f3` `test rax,rax / je` | `.?AVCCTinlayoutFusedPreBlockSwin1HLayer@HNetCpp@@` |

If either cast returns null, `call 0x180061710` — the pure setter that stores the pair at
`cb+0x98`/`cb+0x9c` — never runs, and **all three controls are inert together**. That is exactly
the pattern reported from hardware. A previous diagnosis called these "proven to reach the network
raw and unclamped"; it had walked the *call edges* and never looked at the guards on them, so it
established reachability and reported it as liveness. Whether the shipped model satisfies both
casts is **not settled from the binary** — it needs a run.

**Nothing is clamped on load.** An earlier build clamped all four knobs in `cfg::load`. For
`intensity` and `local_tone_strength` that changed no behaviour and only rewrote the user's file;
for the two structure strengths it removed values the snippet is willing to accept, since
`0x180061710` stores them raw. The sliders carry the conventional `[0,1]` domain — `-1.0f` is the
snippet's own disabled sentinel and out-of-range conditioning of a trained network is undefined
rather than "more" — and the ini is the unclamped escape hatch.

### Every setting is live — what each one costs, and the two that are not

The overlay (`src/overlay_ui.hpp`) can change **every key in the table above except `app_id`**
without restarting the game. That is a deliberate property, not a convenience: this add-on exists
because a whole play session once ran with nothing running and nobody knew, and a control that
looks editable but silently needs a relaunch is the same failure in a smaller box.

The mechanism is one ladder with six rungs, each implying every rung below it. There is exactly
one deferred-work seam — `nr_state::pending_work`, raised on a recording thread, serviced on the
next present by `nr_service_reconfigure` on the main thread, where idling the GPU queue and
destroying a resource are legal. That is the same seam a resolution change has always used; it was
generalised rather than duplicated.

| Rung | What it does | Cost you can see |
|---|---|---|
| **R0** snapshot | `begin_pass` copies the overlay's atomics into `g_cfg` once per pass, on the render thread, under the lock the pass already holds | none |
| **R1** reset | + one `DLSSNR.Reset` frame, because the accumulated temporal history was built under the other geometry | one un-accumulated frame |
| **R2** flush | + the armed pristine copy is dropped (it names a raw `ID3D12Resource` address held across a frame, and UE 4.27's pool recycles those) | one frame of temporal feedback |
| **R3** ident | + the per-PSO identification memo is invalidated on **every** command list at once, and the identification one-shot log lines are re-armed | one frame |
| **R4** rebuild | + the NGX feature, every view and every texture are released on the next present and rebuilt on the following dispatch | a visible hitch |
| **R5** rearm | + pipelines built, parameter block replaced, or the snippet loaded, on the present thread | a stalled frame |

Per key:

| Key | Rung | Note |
|---|---|---|
| `intensity`, `local_tone_strength`, `local_structure_strength`, `skin_structure_strength`, `style`, `use_auto_mask`, `ui_correction` | R0 | every NGX tuning parameter is written from `g_cfg` on **every** accepted dispatch and none is baked at `CreateFeature`, so these are free |
| `paper_white_scale`, `transfer_strength`, `color_strength`, `restore_graphics_root` | R0 | read more than once per pass; the snapshot is what makes the reads agree. A `restore_graphics_root` tear between `capture_state` and `restore_state` would be **corrupting**, not a tuning difference |
| `copy_back`, `history_restore` | R2 | both edges must drop the armed copy |
| `depth_inverted`, `mvec_scale_x`, `mvec_scale_y`, `mvec_reconstruct`, `mvec_dilate` | R1 | |
| `mvec_clip_row`, `mvec_clip_transpose` | R1 + latch clear | a bad value latches `view_layout_failed` **permanently** for the resolution; the reconfigure clears it, so the knob stays a knob after the first wrong answer |
| `shader_hash` | R3 | read *before* `st->mutex` and *before* `begin_pass`, so it cannot ride the snapshot — it goes through a lock-free `read_ident()` at its own site, and a per-command-list epoch invalidates every cached answer at once |
| `srv_depth`, `srv_velocity`, `srv_colour`, `uav_output` | R3 | these *do* ride the snapshot; they need the arm drop and the latch re-arm, not the memo. `srv_colour` is the only setting in the add-on that tears **across** frames — it is also the history-restore refusal test |
| `hdr_codec` | R4 (+R5 on) | **both directions.** See below |
| `mvec_decode` | R4 + R5 on | off→on builds the decode pipeline (runtime `D3DCompile`, hence the present thread) |
| `enabled` | R4 + R5 on | off→on runs the shipping startup path: `ngx::load_snippet` then `g_nr_pending_init`, which the render thread's existing deferred initialiser consumes. **LoadLibraryW of a 166 MB module — expect one stalled frame** |
| `populate_parameters` | R4 + R5, **explicit Apply button** | a gated export whose exact signature is unverified against this snippet build; a checkbox that fired on click would be the wrong shape |
| `diagnostics` | R0, **atomic at its own site** | read on every draw and every dispatch in the process, on arbitrary recording threads, outside any snapshot. It could never go through `g_cfg`; one relaxed load at each of the three sites makes it live at zero risk |
| `rt_census`, `rt_census_frames` | R0, into the census's own atomics | its counters are **cumulative from the first arm** and are not reset by an off/on cycle — read the deltas between summaries, not the totals |
| `require_trampoline` | R5 one way only | **1→0 is live**; 0→1 needs a relaunch — see below |
| `app_id` | **relaunch** | see below |
| `dlss_sr` | R4 one way + branch R0 | **1→0 is fully live**: the branch into `sr_try_run` is taken after the snapshot, so unticking hands the dispatch back to DLSS-NR on the next frame with the SR feature and both SR textures released. **0→1 needs a relaunch** when `nvngx_dlss.dll` was not loaded at launch — see below |
| `sr_shader_hash` | R3 | the DLSS-SR re-pin, through the same `read_ident()` + identification epoch `shader_hash` uses. Consulted only while `dlss_sr=1`; `0` means "use `shader_hash`" |
| `sr_suppress_taa` | R0 + latch re-arm | free per dispatch. The one-shot that reports the refused `suppress=1, direct=0, copy_back=0` combination is re-armed on every reconfigure, or a live toggle would make the second refusal silent |
| `sr_mvec_decode`, `sr_mvec_reconstruct` | R1 | independent of `mvec_decode` in VALUE, but the decode pipeline is **one** root signature, PSO and DXBC shared by both features and built when either asks. off→on therefore costs the same R5 build `mvec_decode` does when nothing had built it yet |
| `sr_perf_quality`, `sr_render_preset` | R0 value + **explicit recreate button** | both are latched into the DLSS create-params at `CreateFeature` and have no evaluate-time equivalent, so the value is live but cannot be *re-read* without releasing the feature. "Recreate the SR feature" raises R4 on the same seam a resolution change uses |
| `dlss_nr` | **relaunch, both directions** | its only two read sites are the 166 MB `LoadLibraryW` in `init_device` and the `Init_Ext` gate on the first dispatch. It is owned, saved and reverted like every other key; it is deliberately **not** in the per-pass snapshot, because writing it into `g_cfg` would put a value in front of a reader that does not exist. To turn the DLSS-NR pass off for *this* session, use `enabled` |

#### `dlss_sr` — live one way, and the UI says which way

The branch is live in both directions. The **arm** is not, and the reason is specific rather than a
general caution: arming DLSS-SR is a 59 MB `LoadLibraryW` of `nvngx_dlss.dll` claiming the
trampoline's **slot B**, followed by `NVSDK_NGX_D3D12_Init_Ext` through that slot from a render
thread with a fully built device. That call is made exactly once per process, inside
`nr_lazy_ngx_init`, on the first accepted dispatch. Making the ON direction live would mean a
*second* `Init_Ext` in the session, which is the same unverified action `app_id` is refused for —
and the only measurement this project has of `Init_Ext`'s fragility is that it **hangs** when called
at a moment the snippet does not tolerate. A hang is not a failure that degrades.

So with `dlss_sr = 0` in the ini at launch, ticking the box:

* changes the branch immediately — the next accepted dispatch really does go to `sr_try_run`;
* is refused there on the second line with `DLSS-SR: pass did not run - not armed`, leaving ReShade
  to issue the game's own TAA, i.e. a correct frame and a strict no-op;
* is reported by the reconfigure banner as **RELAUNCH REQUIRED**, not APPLIED, and by a permanent
  amber line beside the checkbox that names *which* of the two unarmed cases applies — the snippet
  was never loaded, or it loaded and `Init_Ext` through slot B failed. Those have different fixes;
* is saved by the Save button and takes effect next launch.

#### The DLSS-SR keys that are still ini-only

Nineteen of the keys `main` added have an ini entry and a documented meaning but **no control yet**.
Every one of them is read inside `sr_try_run`, i.e. downstream of the per-pass snapshot, so each is
one `live_block` atomic, one `OVERLAY_OWNED_FIELDS` entry, one snapshot line and one widget away
from being live — the mechanism is already there and none of them needs new machinery:

* **tier 0, free** — `sr_copy_back`, `sr_direct_output`, `sr_mv_scale_x`, `sr_mv_scale_y`,
  `sr_jitter_scale_x`, `sr_jitter_scale_y`, `sr_jitter_projection_only`
* **tier 1, needs the SR feature releasing** (all latched into the create-params) — `sr_hdr`,
  `sr_hw_depth`, `sr_depth_inverted`, `sr_mv_lowres`, `sr_mv_jittered`, `sr_auto_exposure`,
  `sr_alpha_upscaling`, `sr_out_width`, `sr_out_height`, `sr_group_tile`, `sr_use_view_rect`
* **arm-time diagnostic, not a render-path setting** — `sr_optimal_settings`

Until they have controls they behave exactly as they did before this branch: read from
`stray_dlssnr.ini` at load, constant for the session. **They are listed here rather than left to be
discovered**, because a key the ini can express and the UI silently cannot is the exact failure this
overlay exists to remove.

#### `hdr_codec`, both directions — the hard one

Two separate things blocked it, one per direction, and neither was a design constraint:

* **off→on was blocked by one line.** `nr_lazy_ngx_init`'s `else` branch read
  `st->codec_failed = true;   // not a failure, but the same "do not use it" state`. That is the
  **run-latched shader-build failure** flag, and `nr_release_feature_and_output` deliberately never
  clears it — because a resolution change cannot undo a failed `D3DCompile`. So `hdr_codec=0` at
  load latched a permanent failure state to mean "the user configured it off". That assignment is
  **deleted**, not cleared at reconfigure time: clearing the latch to service a config change would
  also erase a *real* build failure and make the add-on retry a broken compile every frame.
  `mvec_decode` carried the identical defect at `st->mvec_failed = true;`, with the same comment;
  it is deleted too.
* **on→off was blocked by the format.** `out_tex` is forced to `r16g16b16a16_float` for its
  lifetime whenever the codec is on. With the codec off it becomes the copy-back source, its format
  no longer matches an `r11g11b10_float` TAA output, and the copy-back guard **silently skips** —
  which on screen reads as "no denoise" while every other indicator stays healthy. The fix needed
  no new code: the teardown destroys `out_tex` and zeroes `out_w`/`out_h`, so the next accepted
  dispatch re-enters `nr_ensure_output` on the create branch and re-decides the format against the
  new value.

#### The two that still need a relaunch, and the proof

* **`require_trampoline`, 0→1 only.** Honouring it would mean unloading an already-initialised
  snippet, and there is no in-process unload path anywhere in this tree — `nr_destroy_device`
  declines to `FreeLibrary` even at device teardown, on the grounds that "a 166 MB module that may
  still hold worker threads" buys nothing. The 1→0 direction **is** live, and it is the direction
  that matters: on that path `ngx::load_snippet` already called `unload()`, so nothing is loaded and
  re-running it is clean.
* **`app_id`.** The mechanism exists — `Shutdown1` is resolved, is required at load, and is already
  called in-process at device teardown, so `Shutdown1` + `Init_Ext` would be one more action on the
  service. What is **not** proven is that `Init_Ext` survives a second call, and the only
  measurement this project has of its fragility is that it *hangs* when called at a moment the
  snippet does not tolerate: the log stops between `loaded nvngx_dlssnr.dll` and the `Init_Ext`
  result, the process sits at ~2% CPU, and the title never reaches its menu. A hang is not a failure
  that degrades, and the standing rule for the ladder is that a reconfigure which fails leaves the
  previous working state. Since `app_id` has **no render-path effect at all** — the snippet resolves
  its weights from its own embedded `WEIGHTS_HT` resource, so it only names the log file written
  beside the add-on — shipping an unverified path that can hang the game to rename a log file is the
  wrong trade. It is stated as exactly that in the UI, not as "load-only".

The same reasoning is why `enabled = 0` **releases the feature and stops the pass** rather than
tearing NGX down: clearing the armed flag would make the next `enabled = 1` call `Init_Ext` a second
time in the session. Off gives back the VRAM, which is where the memory actually is; NGX itself
stays initialised, and turning it back on rebuilds everything with no second `Init_Ext`.

#### What a reconfigure looks like in the log

One line, and it fires only when a rung was actually climbed — the service returns early when there
is no work, so this is never per-frame noise:

```
DLSS-NR reconfigure APPLIED: "hdr_codec" -> tier 1 (feature recreate) +ident-epoch +reconcile. codec=built mvec=built feature=released
```

A failure names itself the same way and leaves the previous state running:

```
DLSS-NR reconfigure FAILED: "hdr_codec" - the HDR codec's shaders or pipelines could not be built; the denoise still runs, undecoded. The PREVIOUS working state is still running and nothing is half-applied.
```

The overlay draws that failure in red above every other status rung, because the add-on may well be
evaluating perfectly happily on the old settings — and "EVALUATING" on its own would then be a true
headline answering the wrong question.

#### Saving

The overlay's Save button now round-trips every key it can change, which is all of them except
`app_id`. It still rewrites `stray_dlssnr.ini` **in place** — every comment, blank line, column
alignment, trailing comment and your own spelling of `colour`/`color` survives — via a temp file and
`MoveFileExW(REPLACE_EXISTING)`, because a half-written ini is worse than none.


### `rt_census` — the DXR dispatch census

Read-only instrumentation (`src/rt_census.hpp`) that measures which ray tracing effects the title
actually runs: the RT shader entry-point names the engine compiles — read out of the DXIL `RDAT`
part by ReShade, so they are the engine's own strings (`OcclusionRGS`,
`RayTracingReflectionsRGS`, `AmbientOcclusionRGS`, …) — plus one bucket per distinct `DispatchRays`
signature, with counts, extents, shader-binding-table sizes and strides, and the bound state
object. A summary block goes to `ReShade.log` every `rt_census_frames` presents and again at
device teardown, so one log read answers *which effects dispatched, how often, at what resolution*.

**It defaults to `0`, and off means off.** Every entry point returns after one relaxed atomic
load: the `init_pipeline` hook, the `SetPipelineState1` hook, the `dispatch_rays` handler (which
then returns `false`, so ReShade issues the game's dispatch exactly as with no add-on present),
the `present` hook and the `destroy_device` hook. Nothing is counted, named, logged or allocated —
and the census allocates nothing at any time, on or off: every table it keeps is a fixed-size
array. The `dispatch_rays` *event* is registered unconditionally in `DllMain` because the ini is
not read until the first `init_device` and ReShade invokes that event with no listener check, so a
late registration would race a recording thread; the cost with the census off is one extra
indirect call in a loop that already runs. `build_acceleration_structure` is deliberately **not**
registered — ReShade allocates and converts every geometry desc on every AS build as soon as that
event has any listener at all.

It is gated **only** by `rt_census`, not by `enabled`, so the title's ray tracing can be measured
with the DLSS-NR pass switched off.

Related: the `probe census` line's `dxil=` counter counts **pixel and compute shaders only** —
`on_init_pipeline` skips every sub-object that is not a PS or a CS, so a DXIL ray tracing library
could never reach it and `dxil=0` never meant "no ray tracing". The line now says so, and ends in
`rt=MEASURED` or `rt=NOT MEASURED`.

Deployment and the exact greps that read the result: **`STAGING-census.md`**.

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

#### The graft-back is now selectable: `hdr_graft`

The reference add-on (`renodx-dlss5-v2.5`) grafts the network's answer back **differently**, and
both are now shipped so they can be A/B'd live on hardware. `hdr_graft = 0` is ours and is the
default; nothing about the shipping image changes unless you move it.

**The encode is the same either way.** Its HLSL is embedded as plaintext in
`renodx-reference.addon64` (`.rdata` RVA `0x42f90..0x440bd`), and its curve matches
`src/hdr_codec.hpp` constant for constant: the exact piecewise sRGB (`0.0031308` / `12.92` /
`1.055` / `1/2.4` / `0.04045`), the soft-clip knee `0.75`, the shoulder `5.770780` and the
`0.75 + 0.25·(1 − exp(−5.770780·(v − 0.75)))` form. Two shader-level differences remain and neither
changes the curve: they *divide* by paper white where we multiply by its reciprocal (identical when
the reciprocal is exact — it is at the `1.0` default — and within 1 ULP otherwise), and they write
`source.a` into the proxy where we deliberately write `1.0`. So **the network is shown the same
proxy and returns the same answer in both modes.** Only the graft-back differs. Do not describe the
two encoders as "byte-identical": the curve is, the shader is not.

| | `hdr_graft = 0` — additive (ours, default) | `hdr_graft = 1` — renodx `UpgradeToneMap` |
|---|---|---|
| the transfer | `result = original + (neural − proxy) / s` | `ratio` from the asymmetric branch below, then `HueOkLab(neural · ratio, neural)` |
| where it works | scene-linear | display-referred, normalised in and out by `s` |
| what it scales | the **original**, uniformly across RGB | the **network's answer** |
| hue | cannot drift — RGB is scaled by one number | can drift, so it is locked to the *neural's* hue in OkLab, then AP1-clamped for negatives |
| `transfer_strength = 0` | **bit-exact no-op at every `paper_white_scale`** | `(original · s) / s` — exact only when `s` is a power of two; otherwise ~`1e-7` relative on 22–36 % of pixels |
| NaN firewall, FP16 clamp | yes | yes — **ours, kept**; theirs has neither |
| alpha from the original | yes | yes — and this is **not** a difference: their decode loads `float4 source = OutputOriginal.Load(...)` and writes `source.a` (`renodx-codec-shaders.hlsl:199`, `:222`), and `OutputOriginal` (`t3`) *is* their original. Same behaviour, arrived at independently |

Their branch, reproduced verbatim including its asymmetry:

```hlsl
if (original_y < proxy_y)  ratio = original_y / proxy_y;
else { float new_y = neural_y + max(0.0, original_y - proxy_y);
       ratio = neural_y > 0.0 ? new_y / neural_y : 0.0; }
```

The `original_y < proxy_y` side is **not** a rare edge — it is the FP16-rounding side of the knee
and it fires on ~25 % of pixels (49,778 of 200,000 measured). It is continuous at `oy == py`.

##### What the trade actually is — and it is *not* what it looks like

The obvious reading of `max(0, original_y − proxy_y)` is "this recovers luminance the soft clip
discarded, and the additive residual cannot". **That reading is wrong, and the arithmetic says so.**
Luminance is linear, so

```
theirs:  new_y = neural_y + max(0, original_y − proxy_y)   =  Y(orig) + Y(neural) − Y(proxy)
ours:    Y(original + (neural − proxy))                    =  Y(orig) + Y(neural) − Y(proxy)
```

Measured over 200,000 pixels through real FP16 surfaces with the same luma weights on both sides:
**worst relative difference `3.82e-07`**, a few ULP. (Every figure in this section is quoted from the
CI run, where MSVC and mingw agree to the digit; a different host's `pow` moves the last significant
figure — macOS/clang gives `4.55e-07` for the same sweep — and none of the conclusions move with it.) The magnitude sweep agrees to four decimals at
every source level:

| source magnitude | source `Y` | proxy `Y` | gain, mode 0 | gain, mode 1 | chroma distance between them |
|---|---|---|---|---|---|
| 0.10 | 0.0758 | 0.0757 | 1.3000 | 1.3000 | 0.0001 |
| 0.90 | 0.6821 | 0.6811 | 1.2484 | 1.2484 | 0.0017 |
| 1.60 | 1.2126 | 0.9585 | 1.0267 | 1.0267 | **0.0943** |
| 3.00 | 2.2736 | 0.9990 | 1.0004 | 1.0004 | **0.1330** |
| 8.00 | 6.0631 | 1.0000 | 1.0000 | 1.0000 | **0.1340** |

`0.1340` is the *full* rg-chromaticity distance to the white point. So:

* **Both modes deliver the same luminance.** Neither one corrects a bright highlight, and the
  ceiling is **much lower than the soft clip suggests**, because the proxy is not kept in FP32 — it
  is written to an `r16g16b16a16_float` texture and read *back out of it* (`proxy_desc` in
  `src/stray_dlssnr.cpp`; the "why the decode re-reads the proxy" section of `src/hdr_codec.hpp`).
  A half has ~11 mantissa bits, so `fp16(SrgbEncode(SoftClip(v)))` is **exactly `1.0` at
  `v ≥ 1.81× paper white`**. `3.47×` is the value at which `SoftClip(v)` itself rounds to `1.0f` in
  FP32 (`0.25·exp(−5.77·(v − 0.75))` below `2⁻²⁵`) — it is a real number about a value this codec
  never stores, and quoting it overstated the headroom by nearly a factor of two.

  The transfer is mostly gone well before even `1.81×`, because the soft clip is a curve, not a
  wall. Of a requested **+30 %** display-referred gain, measured through the real FP16 surfaces:

  | delivered | out to |
  |---|---|
  | ≥ 95 % of the request | `0.79×` paper white |
  | ≥ 50 % | `1.15×` |
  | ≥ 5 % | `1.86×` |

  **Those figures are ratios *to paper white* and do not move with `paper_white_scale`** — the
  earlier claim that `paper_white_scale = 4.0` "holds full gain out to ~2.3× paper white" mixed the
  units, and also compared "where full gain still holds" against "where all signal is lost", which
  are not the same measurement. What `paper_white_scale` moves is the **scene-linear magnitude**
  those ratios land at, in proportion: at `4.0`, full gain reaches a source magnitude of `3.17`
  instead of `0.79`. So if highlights are being lost, **the knee is in the wrong place and
  `paper_white_scale` is exactly the right knob to raise** — just do not expect the ×-paper-white
  ceiling to move with it. (Selftest section 8 measures all of this, so the numbers cannot drift
  from the code.)
* **The entire difference is chroma, and it runs the other way.** Mode 1 rebuilds the pixel from the
  network's answer and locks the hue to *that*; where the proxy clipped, the network's answer is
  neutral white, so a saturated highlight is pulled toward the white point. `[6.0, 5.2, 3.0]` comes
  back as `[6.0, 5.2, 3.0]` in mode 0 and as roughly `[5.21, 5.21, 5.21]` in mode 1. Over 60,000
  random pixels at `color_strength = 1` the two differ by up to **83 % of the pixel's magnitude** — the
worst case is `[10.04, 10.12, 0.94]`, which mode 0 leaves essentially alone and mode 1 returns as
`[9.45, 9.45, 9.45]`.

**So `hdr_graft = 1` is a colour experiment, not a highlight-recovery fix.** It is a defensible,
different aesthetic — *trust the network's colour* — and STRAY's neon signage is exactly where you
will see it. Just do not run it expecting recovered highlights.

##### `color_strength` is the third point of comparison — and it does *not* cancel the graft

`color_strength` already **is** a genuine luminance-ratio mode, not a partial one:
`luminanceOnly = lerp(transferred, original · luminanceRatio, chromaWeight)` with
`chromaWeight = saturate(originalLuminance / (0.001/s))`. At `color_strength = 0` and above the
chroma floor the output is `original · (Y_transferred / Y_original)`: an RGB-uniform, hue-exact
rescale. Their decode has the *same* construction (`luminance_only = original · ratio`, then
`lerp(luminance_only, upgraded, ColorStrength)`) and their upgraded luminance equals our
transferred luminance, so on ordinary pixels the two modes agree there to **under one 8-bit code
value**.

**But `color_strength = 0` is not a control that cancels the graft — it swaps which half of the
graft difference you are looking at**, and the earlier text here got that wrong. Mode 0's chroma
floor is a real term with a real effect: below `Y = 0.001·(1/s)` it crossfades away from the
hue-exact rescale and hands the pixel to the network's own colour. **Mode 1 has no floor at all** —
`luminanceOnlyRdx = originalDisplay · ratioY`, faithfully to renodx — so it keeps the original's
chromaticity all the way down and rescales it by an unbounded ratio. That region is not a curiosity:
it is where a *denoiser* changes the image most.

Measured over 400,000 dark, strongly chromatic pixels (scene-linear magnitude ≤ `0.01`, network gain
`0.3×`–`6×` with a `0.6` pull toward the pixel's own mean, `transfer_strength = 1`,
`color_strength = 0`):

| | worst difference | pixels differing ≥ 2 code values |
|---|---|---|
| mode 0 vs mode 1 | **27.6** 8-bit code values | **42.5 %** |
| mode 0 *with its chroma valve forced open* vs mode 1 | **0.0** | 0 % |

The second row is the point: force mode 0's valve open and the shadow difference vanishes entirely,
so the valve is the whole cause and nothing else in either graft is moving. The shortest statement
of it — a dim red shadow the network denoises to a neutral `0.2`, at `color_strength = 0`:

```
src [1e-5, 0, 0]   mode 0 -> [0.20166, 0.19965, 0.19965]   the valve handed it to the network's grey
                   mode 1 -> [0.94077, 0.00000, 0.00000]   no valve: the original's red, rescaled
```

The earlier figure quoted here — "worst 4.74 % of a channel, so the two are nearly the same image at
`color_strength = 0`" — was an artefact of the selftest's sampler: `random_pixel` draws all three
channels as `mag·(0.05 + 0.95u)`, which bounds chromaticity at about 20:1 and never approaches
black, and `network_answer` never departs from the proxy by more than `0.6×`–`1.8×`. Neither could
reach the region where the modes part company. Section 5 of the selftest now measures both regions
and asserts the shadow divergence as a *lower* bound, so the sampler cannot be quietly narrowed back.

There is still no fourth mode worth adding: two real graft behaviours, one crossfade that applies to
both. But **A/B the grafts at `color_strength = 1` for the highlight difference *and* at
`color_strength = 0` on a dark coloured area for the shadow difference.** Neither setting shows both.

##### Verified on the build host

`tools/hdr_codec_selftest.cpp` replays the decode's arithmetic natively. It carries **two separate
transcriptions of mode 0** — the decode as it shipped before `hdr_graft` existed, and the decode as
it is now — so "mode 0 is unchanged" is 1,080,000 bit-pattern comparisons rather than an argument
about where a brace went.

```sh
c++ -std=c++17 -O2 -Wall -o /tmp/hdr_codec_selftest tools/hdr_codec_selftest.cpp \
  && /tmp/hdr_codec_selftest
```

`tools/hdr_source_variants_test.cpp` is the companion gate for the *survival* build described
below: it calls the real `full_source_decode()` and proves the graft-free variant differs by
exactly one byte, that both `#define` markers are where the code thinks they are, and that every
graft symbol (`float3x3`, `nrRdxToOkLab`, …) has all of its code uses **inside** the `#if` — so
the preprocessor really removes them and the retry is not theatre. **23 assertions, all passing.**
It needs `<d3dcompiler.h>`, so it runs on the Windows toolchains only; CI runs it under both MSVC
and mingw, which agree to the digit.

**37 assertions, all passing:**

| check | result |
|---|---|
| identity, mode 0 | `transfer_strength = 0` returns the original **bit for bit** in **1,080,000/1,080,000** cases (12 scales × 9 `color_strength` × 10,000 pixels), worst absolute deviation `0` |
| alpha | taken from the original in all 1,080,000 |
| mode 0 vs the **shipping** decode | **1,080,000/1,080,000** identical bits, with the network actively changing the image (6 scales × 6 `transfer_strength` × 5 `color_strength` × 6,000 pixels) |
| NaN firewall | a broken source passes through untouched, alpha included, in **both** modes |
| FP16 range | mode 1 never emits a non-finite or out-of-range value, including their `neural_y == 0` cliff |
| their cliff | reproduced, not smoothed: an exactly-zero network answer forces `lerp(original, 0, ts)` |
| headroom term ≡ additive residual | worst relative difference `3.82e-07` over 150,250 else-branch samples |
| their asymmetric branch | fires 49,750 / 200,000 times on FP16 data — real, not an edge case |
| mode 1 at `transfer_strength = 0` | exact at `paper_white_scale` 1.0 / 2.0 / 0.5; **7144**, **4365**, **7076** of 20,000 non-exact at 1.5 / 2.2 / 0.75, worst `1.08e-07` relative. Mode 0: **0/20,000 at every one** |
| divergence, `color_strength = 0`, ordinary pixels | **0.7** of an 8-bit code value over 60,000 pixels — they *do* agree here |
| divergence, `color_strength = 0`, **shadows** | **27.6** code values over 400,000 dark chromatic pixels, **42.5 %** of them ≥ 2 |
| …and its cause | mode 0 with its chroma valve forced open vs mode 1, same 400,000 pixels: **0.0** code values |
| divergence, `color_strength = 1` | **83 %** of the pixel's magnitude (140 code values) |
| the transfer's ceiling | `fp16(SrgbEncode(SoftClip(v))) == 1.0` at `v ≥ 1.8088`; `SoftClip(v) == 1.0f` in FP32 at `v ≥ 3.4740`; the ×-paper-white ceiling is invariant across `paper_white_scale` 1.0 / 2.0 / 4.0 |

CI runs the same replay under **both** MSVC and mingw — and the two agree to the printed digit on
every figure above — and separately compiles the codec's HLSL — extracted from the string literals
in `src/hdr_codec.hpp` exactly as `full_source_decode()` assembles it, in **both** of its variants —
with **`fxc /T cs_5_0 /O3 /Ges /Gis`**, the same *flags* the add-on passes to `D3DCompile` at load.
A typo in that HLSL has no compile-time symptom in the C++ build and no crash at runtime: the codec
just latches **off** and the user silently gets the darkened frame back. That gate is the only thing
in the tree that would notice.

**`fxc` is not the compiler that runs on the play box, and the gate must not be read as if it were.**
Under Proton, `D3DCompile` resolves to whatever `d3dcompiler_47.dll` is in the prefix, which may be
Wine's builtin — vkd3d-shader's HLSL front end, whose SM5 compute coverage varies by version. Mode 1
introduced this tree's first `float3x3` literals, first `mul(matrix, vector)`, first `sign()` and
first `length()`, and adding `hdr_graft` changed the decode's source hash from
`0x397c6b5d90cbe29b` to `0x34fc9b8beea4af4e`, so every existing on-disk cache is orphaned and a
*fresh* compile of the new code is mandatory on first launch. A green CI run does not de-risk that.

**So the decode is compiled twice, and mode 0 survives the experiment failing.** The literal carries
`#define NR_RDX_GRAFT 1`; everything of renodx's lives behind that `#if`, inside one function, so
`main()` is byte-identical in both variants and mode 0's expression tree is untouched. If the
compile with it at `1` fails, `hdr_codec::build` flips that one character to `0` and recompiles.
Mode 1's body becomes a stub, `blobs::decode_has_graft` comes back false, and the CPU **pins
`hdr_graft` to 0 for the run** — the log says so, and the overlay shows an amber line and disables
the combo. The default graft the user plays on every day is unaffected. Losing the shipping path to
a compiler that cannot build an experiment would have been strictly worse than not shipping the
experiment.

A **user-supplied `stray_dlssnr_decode.dxbc`** (the documented escape hatch when `D3DCompile` is
unavailable at all) is preferred over both the cache and a fresh compile, and such a blob may
predate `g_hdrGraft` and never read it — in which case `hdr_graft` is inert. That cannot be
detected, so it is *reported*: `blobs::decode_overridden` reaches both the log and an amber line
above the combo, saying that the control may do nothing while it is in place. To pin the *old*
decode deliberately, rename `stray_dlssnr_decode.397c6b5d90cbe29b.dxbc` (if you still have it) to
`stray_dlssnr_decode.dxbc`.

##### The exact A/B to run on hardware

All ini-only, no rebuild; or flip **HDR Graft** in the overlay, which is live. `ReShade.log`
re-prints its `GRAFT-BACK MODE` line **every time the mode changes**, so the log from a session in
which you flipped the combo says which graft was running when, rather than asserting whichever one
happened to be first. Check that line before trusting any A/B you report: it also warns if a
`.dxbc` override is in place or if the decode was built without mode 1.

| # | config | expectation |
|---|---|---|
| 1 | `hdr_graft=0 transfer_strength=0` | pixel-identical to `copy_back=0` and to the add-on unloaded. **The gate.** Run it first; if it fails, nothing below means anything. |
| 2 | `hdr_graft=0 transfer_strength=1 color_strength=1` | today's image. The control. |
| 3 | `hdr_graft=1 transfer_strength=1 color_strength=1` | **the comparison.** Same brightness as 2 everywhere; look only at *saturated bright* things — neon signage, wet-street reflections, the bar interiors. Mode 1 washes them toward white; mode 0 keeps their colour. If you cannot see a difference here, you are not looking at a clipped highlight. |
| 4 | `hdr_graft=1 transfer_strength=0` | should be visually identical to 1. It is *not* bit-identical (see above); if it looks different, something other than the round trip is wrong. |
| 5 | 2 and 3 with `color_strength=0`, looking at a **bright** area | the two should look **the same as each other** there. This is the control that proves the highlight difference in 2-vs-3 is chroma and nothing else. |
| 5b | 2 and 3 with `color_strength=0`, looking at a **dark, coloured** area — shadowed alley walls, unlit interiors, anything dim with a hue | they should **not** match, and this is the graft's *other* characteristic behaviour. Mode 0's chroma floor hands a near-black pixel to the network's colour; mode 1 has no floor and keeps the original's chromaticity, rescaled. Up to 27.6 code values, on 42.5 % of such pixels. If you see shadow chroma speckle in mode 1, the graft **is** the cause. |
| 6 | 2 and 3 at `paper_white_scale=4.0` | moves the soft-clip knee up. Both modes should regain gain on bright things; the *chroma* difference should shrink, because fewer pixels are clipped. This is the test that distinguishes "the graft is wrong" from "the knee is wrong". |

##### The neural target's resolution — settled, and it matters for DLSS-SR

The reference add-on's decode does not `Load` the network's answer; it *bilinearly resamples* a
`ProxySize` region up to `Size`, with this comment:

> DLSSNR 310.8 writes the neural answer at its active network resolution even when the Output
> resource is larger (the signed runtime reports success but leaves the remainder untouched).

That is worth checking rather than assuming, because if true it would mean a *larger* output
resource reads untouched texels.

**It does not bite us, and it cannot.** Three separate facts:

1. `((p + 0.5)·N)/N − 0.5` evaluates to exactly `p` in FP32 for every index of `N` ∈ {720, 900,
   1080, 1234, 1440, 1600, 1920, 1999, 2160, 2560, 3440, 3840}: `floor(position) == p`, the
   fractions are exactly `0`, and the bilinear collapses to a plain `Load`. **`SampleNeural` is an
   exact identity whenever `ProxySize == Size`**, so porting it would buy four texture fetches per
   pixel to compute the same number.
2. `ProxySize == Size` here, structurally. Our proxy, our result and the network's target are all
   created at the same colour extent, and `nr_ensure_output` additionally forces the neural target
   to `r16g16b16a16_float`. We never upscale.
3. **The signed runtime cannot upscale at all.** In `nvngx_dlssnr.dll` 310.8 the network resolution
   is hard-wired to the requested resolution: `CreateFeature` reads `DLSSNR.ScalingRatio` and then
   *unconditionally* stores `1.0f` over the result (`0x180018006`), with no `0xbad00000` guard —
   unlike every neighbouring parameter read — and copies the network W/H verbatim from the
   requested W/H into its own `"CreateFeature begin requested resolution %ux%u (network %ux%u)"`
   log. `EvaluateFeature` repeats the same unconditional store at `0x18001a96a`.
   `DLSSNR.Upscaling`, `DLSSNR.InputWidth` and `DLSSNR.OutputWidth` have **zero** occurrences in the
   DLL. The evaluate also compares the Color rect against the Output rect for equality
   (`0x1800189de`) and *skips* with `"Skip feature evaluate: Invalid Color/Output rect
   configuration"` when they differ.

So the reference add-on's "upscaling" is **its own codec resampling, not the runtime's**: it binds
Color and Output *both* at network resolution — satisfying that equality check — inside larger
display-resolution resources, the runtime legitimately writes only that top-left region, and
`SampleNeural` then covers the display target. `SampleNeural` is deliberately **not** ported here,
and the reason is recorded in `src/hdr_codec.hpp` so a future SR path knows where to find it.

**For a DLSS-SR path (1080p → 4K) this is the whole answer, and it is not the hopeful one.**
DLSS-NR will not upscale for you, and it will *reject* an evaluate whose Color and Output rects
differ in size. Two shapes work:

* **run NR at 4K, after the game's SR.** Color = Output = 4K, `ProxySize == Size`, `SampleNeural`
  stays a no-op, no shader change — but you pay the denoise at 4K.
* **run NR at 1080p and resample in the decode.** This is exactly what `SampleNeural` plus their
  `SourceBase` / `SourceSize` / `ProxySize` / `Size` constant buffer exists for, and it also needs
  their fourth texture: their `Original` (`t0`) is the *encode's* source, read through a subrect,
  while `OutputOriginal` (`t3`) is the *decode's* display-resolution graft target. Ours collapses
  the two — our decode's `InOriginal` **is** their `t3` — because we never subrect and never
  rescale. An SR path has to split them.

There is no third shape in which the runtime does the upscale.

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

## 6c. Chain mode — `dlss_chain`, default `0`

`dlss_sr = 1` and `dlss_nr = 1` do **not** give you both features. Both want the same accepted TAA
compute dispatch, and whoever takes it owns the frame — which, before this revision, was DLSS-SR,
unconditionally. That was an implementation choice, not a limitation: both snippets already load
side by side through the trampoline's two slots, DLSS-NR already evaluates into its **own** texture
rather than writing the frame, and DLSS-SR's colour input is a parameter. `dlss_chain = 1` wires
the one to the other.

### The order, and why it is that way round

```
game TAA dispatch (suppressed)
  -> [codec encode]   1920x1080 linear HDR -> display-referred proxy
  -> DLSS-NR evaluate proxy -> out_tex, at the RENDER extent
  -> [codec decode]   the denoised answer grafted back onto the linear original
  -> DLSS-SR evaluate COLOUR = that denoised 1920x1080 image -> u0 at 3840x2160
  -> one state restore
```

**Denoise first, then upscale.** Upscaling noise is precisely what you do not want, and it is what
DLSS-NR alone does in an upsampling configuration: it denoises at one resolution and a spatial
filter with no temporal information then magnifies whatever is left.

Both evaluates run between **one** `probe::capture_state` and **one** `probe::restore_state`, on
the raw command list, with the descriptor-heap cache re-synced between them — that sync is the call
whose absence is a device removal, not an artefact.

### What DLSS-SR actually receives

`st.result_tex`: **linear HDR** at the render extent, in the colour SRV's own format — the
untouched original plus the network's additive residual. Never the display-referred proxy, which
exists only between the encode's barrier and the restore and is read by nothing but the DLSS-NR
evaluate and the decode.

`hdr_codec.hpp`'s identity property is unchanged by chaining: the proof is algebraic in exactly
three things — the original, `InProxy`'s bits and `InNeural`'s bits — and never mentions the TAA
output. Chain mode changes only *which resource supplies the original*: the game's own `t5`
descriptor instead of a copy. So the property still holds, and it yields a stronger on-hardware
check than the DLSS-NR one:

> **`transfer_strength = 0` in chain mode must be pixel-identical to `dlss_chain=0` / `dlss_sr=1`
> at the same ini.** At 0 the decode is `result = lerp(original, graded, 0) = original`, exactly,
> so DLSS-SR is handed the same `t5` bits it is handed today. That single A/B validates the
> geometry move, the encode, the DLSS-NR evaluate, the decode, the extra barrier and the shared
> motion guide — independently of image quality.

With `hdr_codec = 0` chain mode feeds DLSS-SR the network's **raw display-referred** answer as if
it were linear HDR: gap 1 propagated *through* the upscaler rather than merely present in the
frame. The add-on says so once and runs anyway.

### The geometry move, which is the idea the whole thing rests on

In `MainUpsampling` there is no resolved image at the render resolution anywhere in the frame — the
TAA pass's output is already 4K. So chained, **DLSS-NR denoises the TAA pass's *input*** (`t5`,
render resolution), not its output. One call — `nr_ensure_output(colour.w, colour.h, colour.fmt)` —
moves every downstream DLSS-NR site together: the codec's dispatch domain, the proxy, the result,
the `Color` and `Output` rects, and the motion guide's extent.

That last one matters. DLSS-NR's guide is prepared at its **output** extent and DLSS-SR's at the
**render** extent, so they do **not** coincide by default; they coincide here *because* the
geometry moved. Had `nr_ensure_output` been left on the TAA output, DLSS-NR would have read a
3840x2160 guide while DLSS-SR read a 1920x1080 one — a silent 2x-per-axis error. One
`mvec_decode` dispatch therefore serves both networks, with `MVecScaleX/Y` and `MV.Scale.X/Y` all
forced to exactly 1.0.

### What does *not* happen in chain mode

* **The DLSS-NR copy-back does not run, and must not.** Its result is at the render extent and
  `u0` is at the output extent, and a full-subresource `CopyTextureRegion` between mismatched
  extents is *invalid usage*, not an error return. `copy_back = 1` is ignored and the log says so.
* **`history_restore` is inert**, because it is gated on `copy_back` and exists to undo a write
  chain mode never makes. No pristine copy is taken; the `original` is read straight from the
  game's own `t5` SRV, borrowed for the event and never stored — the same borrow the motion-vector
  decode already makes of the game's velocity and depth descriptors.
* The only write to `u0` is DLSS-SR's (`sr_direct_output = 1`), or its copy-back
  (`sr_direct_output = 0, sr_copy_back = 1`).

### The fallback ladder — every rung, and what is on screen

| rung | condition | on screen |
|---|---|---|
| 1 | `dlss_chain = 0` | today's build, exactly |
| 2 | one snippet absent / not armed | one named refusal, then the single feature that *did* arm |
| 3 | not an upsampling dispatch | one named refusal with the Engine.ini block; DLSS-SR alone if `dlss_sr=1`, else DLSS-NR alone |
| 4 | DLSS-NR textures unavailable at the render extent | **DLSS-SR alone** — a correct upscaled 4K frame, not denoised |
| 5 | DLSS-NR `CreateFeature`/`EvaluateFeature` fails | **DLSS-SR alone**; latched off after 8 consecutive failures |
| 6 | DLSS-SR fails, DLSS-NR fine | the game's own TAAU at 4K. DLSS-NR's evaluate for that frame is **discarded** — one wasted evaluate, a correct frame |
| 7 | jitter unreadable, geometry latch, SR latched off | the game's own TAAU at 4K |
| 8 | an exception in the owned window | the game's own TAAU at 4K; the restore still runs |

Rungs 6 and 7 are the two places chain mode is **strictly worse than DLSS-NR alone**: there is no
legal destination for a 1920x1080 denoise when `u0` is 3840x2160, so "DLSS-NR alone" is not an
available fallback and the honest answer is the game's own TAAU. Ownership is reported only after
DLSS-SR succeeded *and* the state restore completed, so none of these can produce a black or stale
frame.

### Configuration

`Engine.ini` — identical to the DLSS-SR block, chain mode adds no new CVar:

```ini
[SystemSettings]
r.TemporalAA.Upsampling=1
r.SecondaryScreenPercentage=100
r.ScreenPercentage=50
```

`stray_dlssnr.ini`:

```ini
dlss_chain      = 1
dlss_nr         = 1        ; both snippets must load
dlss_sr         = 0        ; chain is its own branch; leave this off
hdr_codec       = 1        ; effectively mandatory - see above
sr_shader_hash  = 0x....   ; the MainUpsampling permutation's hash, RE-PINNED
sr_mvec_decode  = 1
sr_suppress_taa = 0        ; raise to 1 only after the chain is running
sr_direct_output= 0
sr_copy_back    = 1
```

`r.TemporalAA.Upsampling` changes `TAA_PASS_CONFIG`, which is a `#define`, so the DXBC and the
`fnv1a64` change with it. The pinned `shader_hash` for the `Main` permutation **stops matching**,
and the only symptom would be `DLSS-NR: pass did not run - this dispatch is not the target shader`.
`dlss_chain` therefore selects `sr_shader_hash` exactly as `dlss_sr` does, and `shader_hash` keeps
DLSS-NR's own pin so the two can still be A/B'd on one install.

### Proving it ran

* `DLSS-CHAIN: CHAINED EVALUATE #1 OK - BOTH networks ran on ONE accepted dispatch.` — printed
  from inside DLSS-SR's success branch **and** guarded on the DLSS-NR half having returned
  `Success` on the same dispatch. It names both evaluates, both resources and both `Reset` values.
* `--- DLSS-CHAIN @ frame N: chained=... (of ... DLSS-SR evaluates) sr_armed=1` in the periodic
  census. `chained` is incremented on that one branch and nowhere else. **If it is zero, the chain
  did not run, whatever else the log says.**
* every refusal names itself once: `DLSS-CHAIN: <what and why>`.

### Known unknowns, stated

Chained, DLSS-NR is handed the **pre-TAA, jittered, single-sample** render-resolution scene colour,
not the TAA-resolved image it has been run on to date. The snippet has no jitter parameter at all,
so it reads sub-pixel jitter as motion or noise; its own temporal history plus the motion guide
plus `Reset` is what stands against that. This is unavoidable in this ordering — in
`MainUpsampling` there is no un-jittered image at the render resolution — and it cannot be settled
by inspection. An A/B against `r.TemporalAASamples=1` is the cheapest probe. If DLSS-NR turns out
to need a resolved image, the chain is mis-ordered and the answer is not a tuning change.

Separately: if the colour texture is *wider than the view rect* (`QuantizeSceneBufferSize` rounds
to a multiple of 4, e.g. 1932 vs 1930), the encode reads those uninitialised columns and the
network's receptive field can bleed them inward. At `r.ScreenPercentage=50` into 3840x2160 the two
are exactly equal and this does not arise; at 58.8% it does, and nothing detects it.

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
* **The decode is survivable *within itself*, too.** Both graft modes live in one shader and one
  compile, so a failure anywhere in mode 1's OkLab/AP1 matrices would otherwise latch
  `codec_failed` and take the *default* graft — the one that ships and that the user plays on —
  down with the experiment. The literal carries `#define NR_RDX_GRAFT 1` and everything of
  renodx's sits behind that `#if` inside one function; on failure `build()` flips that character
  to `0`, recompiles, reports `decode_has_graft = false`, and the CPU pins `hdr_graft` to 0 for
  the run. `main()` is byte-identical between the two variants, so mode 0 is not merely
  "unaffected in principle" — it is the same text.

On a successful compile the blob is cached beside the ini as
`stray_dlssnr_encode.<source-hash>.dxbc` / `stray_dlssnr_decode.<source-hash>.dxbc`. The hash is
FNV-1a over the exact source text handed to the compiler, so a stale blob from an older revision
can never be picked up silently. A plain `stray_dlssnr_encode.dxbc` / `stray_dlssnr_decode.dxbc`
(no hash) is honoured as a **user override** — drop one in on a machine whose `d3dcompiler` cannot
build the shader — and the log says loudly when an override is in use. Note what an override
implies for a *root constant*: such a blob is whatever the user built and may predate a constant
entirely, in which case the control bound to it does nothing. `hdr_graft` is the current example,
so `blobs::decode_overridden` is now plumbed through to the log and to an amber line above the
overlay's HDR Graft combo. A control that silently does nothing is this project's recurring defect;
"we cannot know, so we say we cannot know" is the only honest handling.

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
