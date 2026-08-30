# Final consistency check

Run this when the outstanding work is merged, BEFORE calling the project done. It exists because
this project has repeatedly shipped things that were individually correct and collectively wrong.
Every item below is here because a real instance of it happened.

## 1. Nothing finished is left unmerged
- [ ] `git branch -r` — every branch is either merged into `main` or deliberately abandoned with a
      reason recorded. Historical strays: `overlay-ui`, `dlss-sr`, `rr-census`, `rr-experiments`,
      `parity-fixes`, `live-settings`, `renodx-hdr`, `nr-sr-chain`, `nr-tuning-live`, `mvec-decode`.
- [ ] Nothing of value is left only in the **non-git** tree
      `/Users/lunks/Downloads/dlssnr-patch/stray-dlssnr/`, which is not under version control.
- [ ] CI green on `main` for **all three** jobs: MSVC, mingw parity, ImGui ABI probe.

## 2. A merge did not silently revert someone
Every branch cut before another merged shows the other's files as **deletions** in a naive diff.
`rr-census`, `dlss-sr` and `live-settings` each hit this.
- [ ] `git log --diff-filter=D --name-only main` — no source file was deleted by a merge.
- [ ] `src/` still contains: `stray_dlssnr.cpp`, `hdr_codec.hpp`, `mvec_decode.hpp`, `ue4_jitter.hpp`,
      `dlss_sr.hpp`, `overlay_ui.hpp`, `overlay_imgui.hpp`, `rt_census.hpp`, `ngx_interop.hpp`,
      `descriptor_shadow.hpp`, `d3d12_state.hpp`, `msvc_abi.hpp`, `shader_detect.hpp`,
      `dxbc_tokens.hpp`, `format_names.hpp`, `reshade_compat.hpp`, `addon_config.hpp`.

## 3. No control lies — the bug the user actually hit
Five NR tuning sliders were live in the UI, written to NGX, logged with changing values, and had
**no effect**, while three codec constants worked. A control that does nothing is worse than none.
- [ ] Every control in the overlay reaches something that reads it. Cross-check
      `addon_config.hpp` keys against `overlay_ui.hpp` controls AND against a real write/read site.
- [ ] Every key the ini can express is either in the UI or documented as deliberately absent.
      Generated gap list: `OVERLAY-GAP-SPEC.md` (35 keys at the time of writing).
- [ ] Anything that cannot be live says so **in its tooltip**, with the reason, not in a header comment.
- [ ] Known-dead snippet parameters stay documented as dead: `DLSSNR.ScalingRatio` (overwritten with
      1.0f unconditionally, no `cmp eax,0xbad00000` guard), `DLSSNR.InputWidth/InputHeight` (inert).

## 4. ini, code defaults and the deployed file agree
- [ ] For every key: `addon_config.hpp` default vs `stray_dlssnr.ini` shipped value. A disagreement
      is a bug — `copy_back` shipped as `0` while the code defaulted `true` and the working config
      was `1`, twice, and would have made DLSS evaluate correctly into a texture nobody displays.
- [ ] The ini on the box has every key the build knows, or the missing ones are intentional.

## 5. Docs do not contradict measurements
- [ ] `docs/FG-FEASIBILITY.md`, `docs/FG-OPTISCALER-BRIDGE.md`, `docs/PRIOR-ART-*.md`, `README.md`,
      `STAGING-sr.md`, `rr-experiments/` — no claim that a later measurement overturned.
      Known retractions that must be reflected: the renodx HDR graft does **not** recover
      soft-clipped highlights (it is luminance-identical to ours; the difference is chroma, and it
      neutralises saturated highlights); flip metering is **not** an FG blocker at 2x; and
      `DLSS.Use.HW.Depth` is **absent from the NR snippet**, so it was never a shipping-NR bug.
- [ ] Every `[ASSUMED]` in the tree is still assumed, or has been settled and updated.

## 6. Load-bearing invariants still hold
- [ ] Trampoline: **all 18** forwarders `call=1 tailjmp=0` (9 slot A + 9 slot B). A tail jump
      re-exposes the caller and every gated export returns `0xbad00002`.
- [ ] `init_complete` is release-stored **last**, only on full success; everything after `pd_create`
      holds `st->mutex`. This fixed a critical race and a merge could quietly undo it.
- [ ] Lock order: `nr_try_run` takes `st->mutex` then `g.mutex`; the `on_present` census uses
      atomics and never takes `st->mutex`.
- [ ] The two deliberately deleted latch lines (`codec_failed`, `mvec_failed`) are still deleted.
- [ ] HDR codec identity: `transfer_strength = 0` is still a bit-exact no-op.
- [ ] Defaults that must stay OFF: `dlss_sr`, `rt_census`, and any chain/experimental key.

## 7. The deployed build is the build we think it is
- [ ] md5 of `stray_dlssnr.addon64` and `remix_nvngx.dll` on the box == a fresh build of `main`.
- [ ] Present next to the exe: `nvngx_dlssnr.dll`, `nvngx_dlss.dll` (SR), `remix_nvngx.dll`
      (dual-slot), `d3dcompiler_47.dll`, `dxgi.dll`.
- [ ] `ReShade.ini` has `DisabledAddons=` **empty** — a disabled add-on is indistinguishable from a
      broken build and cost a whole session once.
- [ ] Stale `.dxbc` shader caches cleared if any shader source changed.

## 8. Claims about hardware are actually from hardware
- [ ] Anything stated as `[HW]` traces to a log line or a command output, not to inference.
      Two separate false-absence conclusions were reached from ASCII greps of UTF-16 strings, and
      one "unreachable" verdict came from a call-graph walk that could not see indirect calls.
