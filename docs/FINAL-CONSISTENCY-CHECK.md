# Final consistency check

Run this when the outstanding work is merged, **before** calling the project done.

It exists because this project has repeatedly shipped things that were individually correct and
collectively wrong. **Every item is a failure that actually happened here.** Nothing is included
because it sounded prudent.

Each check gives the command that answers it. A check you cannot run is a check that did not happen.

---

## 0. How to use this

Work top to bottom. When something fails, **fix it before continuing** — the later sections assume
the earlier ones hold. Record every result, including the passes; "I checked and it was fine" is a
finding.

Two standing rules, both learned the hard way:

* **Absence is hard to prove.** A grep that finds nothing, a call-graph walk that finds no path, a
  log with no error — none of these establish that a thing does not exist. State the method and
  what it cannot see. Two false-absence conclusions here came from ASCII-grepping UTF-16 strings,
  and one from a call-graph walk blind to indirect/virtual calls.
* **Compiling is not running.** Twice, a feature shipped that built cleanly and never executed.
  For anything you assert works, name the evidence that it *ran*.

---

## 1. Nothing finished is left unmerged

```sh
git -C <repo> branch -r --format='%(refname:short)'
for b in $(git branch -r | grep -v HEAD); do
  echo "$b  ahead=$(git rev-list --count origin/main..$b)  behind=$(git rev-list --count $b..origin/main)"
done
gh run list --repo lunks/stray-dlssnr --branch main --limit 1
```
- [ ] Every remote branch is merged into `main` or abandoned **with the reason written down**.
- [ ] Nothing of value exists only in the **non-git** tree
      `/Users/lunks/Downloads/dlssnr-patch/stray-dlssnr/`.
- [ ] CI green on `main` for **all three** jobs: MSVC, mingw parity, ImGui ABI probe.

## 2. No merge silently reverted another

A branch cut before another merged shows the other's files as **deletions** in a naive diff.
`rr-census`, `dlss-sr` and `live-settings` each hit this; merging any of them without folding
`main` in first would have deleted the velocity decode.

```sh
git log --diff-filter=D --name-only --oneline main -- src/ | head -40
ls src/
```
- [ ] No source file was deleted by a merge.
- [ ] `src/` contains all of: `stray_dlssnr.cpp`, `hdr_codec.hpp`, `mvec_decode.hpp`,
      `ue4_jitter.hpp`, `dlss_sr.hpp`, `overlay_ui.hpp`, `overlay_imgui.hpp`, `rt_census.hpp`,
      `ngx_interop.hpp`, `descriptor_shadow.hpp`, `d3d12_state.hpp`, `msvc_abi.hpp`,
      `shader_detect.hpp`, `dxbc_tokens.hpp`, `format_names.hpp`, `reshade_compat.hpp`,
      `addon_config.hpp`.

## 3. No control lies

**The bug the user actually hit.** Five NR tuning sliders were live in the UI, written to the NGX
parameter block, logged with changing values — and had no effect, while three codec constants
worked. A control that does nothing is worse than no control.

```sh
# keys the ini can express vs keys the overlay mentions
python3 - <<'PY'
import re
cfg=open('src/addon_config.hpp').read(); ui=open('src/overlay_ui.hpp').read()
keys=sorted(set(re.findall(r'key\s*==\s*"([a-z0-9_]+)"', cfg)))
missing=[k for k in keys if not re.search(r'\b'+k+r'\b', ui)]
print(f"{len(keys)} keys, {len(missing)} with no overlay reference:"); print(*missing, sep='\n')
PY
```
- [ ] Every control reaches something that **reads** it — not merely a variable that is written.
      Trace to a real consumer: a shader constant buffer, or an NGX parameter the snippet reads on
      a path reachable from the export you call.
- [ ] Every ini key is in the UI or documented as deliberately absent (`OVERLAY-GAP-SPEC.md`).
- [ ] Anything that cannot be live says so **in its tooltip**, with the reason — not in a header
      comment the user will never read.
- [ ] Known-dead snippet parameters remain documented as dead: `DLSSNR.ScalingRatio` (retrieved
      then overwritten with `1.0f` unconditionally, with no `cmp eax,0xbad00000` guard, unlike
      every other parameter), `DLSSNR.InputWidth`/`InputHeight` (inert).
- [ ] For any parameter claimed live: name the **read site** and the **export it is reachable from**.

## 4. Dead code that compiles

```sh
# every build/create/init entry point, and whether anything calls it
for f in $(grep -ohE '\b(nr|sr|dlss_sr|hdr_codec|mvec_decode|rt_census)_[a-z_]*(build|create|init|arm)[a-z_]*' src/*.cpp src/*.hpp | sort -u); do
  n=$(grep -c "\b$f\s*(" src/*.cpp src/*.hpp | awk -F: '{s+=$2} END{print s}')
  [ "$n" -le 1 ] && echo "  SUSPECT (defined, never called?): $f"
done
```
- [ ] No `build`/`create`/`init`/`arm` function is defined and never called. `mvec_decode::build`
      and `::create` were never called; the whole feature was dead while compiling and logging.
- [ ] No give-up latch that can never trip. `clip_fail_streak` was cleared **before** the
      plausibility test it gated, so `>= 30` evaluated `1 >= 30` forever and the error path was
      unreachable — while the log claimed reconstruction was off.
- [ ] Each feature has a log line that fires **only** on a real success, not on arming or intent.

## 5. ini, code defaults and the deployed file agree

```sh
python3 - <<'PY'
import re
cfg=open('src/addon_config.hpp').read(); ini=open('stray_dlssnr.ini').read()
dec={m.group(2):m.group(3).strip() for m in re.finditer(r'^\s+(bool|uint32_t|int32_t|float|uint64_t)\s+([a-z0-9_]+)\s*=\s*([^;]+);', cfg, re.M)}
for k,v in re.findall(r'^\s*([a-z0-9_]+)\s*=\s*(\S+)', ini, re.M):
    d=dec.get(k)
    if d is None: continue
    norm=lambda s: s.replace('true','1').replace('false','0').rstrip('f').rstrip('0').rstrip('.')
    if norm(d)!=norm(v): print(f"  DISAGREE {k}: code={d}  ini={v}")
PY
```
- [ ] No key where the shipped ini disagrees with the code default without a stated reason.
      `copy_back` shipped `0` against a code default of `true` **twice** — which makes DLSS evaluate
      perfectly into a texture nobody displays, i.e. "the add-on does nothing".
- [ ] The ini on the box has every key the build knows, or the omissions are intentional and the
      code defaults are the intended values.

## 6. Docs do not contradict measurements

- [ ] No claim survives that a later measurement overturned. Retractions that must be reflected:
      - the renodx HDR graft does **not** recover soft-clipped highlights — it is luminance-
        *identical* to ours (max relative difference 3.3e-07 over 200k pixels); the real difference
        is **chroma**, and theirs neutralises saturated highlights;
      - flip metering (`NvAPI_D3D12_SetFlipConfig`) is **not** an FG blocker at 2x — it is an R570
        multi-frame-generation mechanism;
      - `DLSS.Use.HW.Depth` is **absent from the NR snippet**, so it was never a shipping-NR bug;
      - the NR tuning parameters **are** read per-evaluate (`fn 0x1b280`, `fn 0x1d5f0` from
        `EvaluateFeature`) — the earlier "Init_Ext only" verdict was wrong.
- [ ] Every `[ASSUMED]` is still assumed, or settled and updated. Grep for them and triage.

## 7. Cross-feature interaction

Six things now share one accepted TAA dispatch: NR, SR, the HDR codec, the mvec decode, the RT
census and the overlay snapshot. Interactions are where the remaining bugs live.

- [ ] With each feature **off**, behaviour is bit-identical to before it existed. State what still
      executes when the key is 0.
- [ ] The descriptor-heap cache resync covers every path that calls `push_descriptors` — it had to
      widen from `codec_encoded` to `codec_encoded || mvec_used` once already.
- [ ] Guide extents are right for **each** consumer. NR's mvec guide is at the output extent, SR's
      at the render extent; a mismatch is a silent 2x scale error, not an error.
- [ ] The camera-cut `Reset` reaches every feature that needs it, on the same frame.
- [ ] Fallback ladders compose: every rung of every feature lands on something that renders. No
      path produces a black or stale frame.

## 8. Load-bearing invariants

```sh
./build.sh 2>&1 | grep -c "OK NVSDK"      # must be 18
```
- [ ] **18/18** trampoline forwarders `call=1 tailjmp=0` (9 slot A + 9 slot B). A tail jump reuses
      the caller's return address, so the snippet's caller check sees the add-on and every gated
      export returns `0xbad00002`.
- [ ] `init_complete` is release-stored **last**, only on full success; everything after
      `pd_create` holds `st->mutex`. This fixed a critical race and a merge can quietly undo it.
- [ ] Lock order: `nr_try_run` takes `st->mutex` then `g.mutex`; the `on_present` census uses
      atomics and never takes `st->mutex`.
- [ ] The two deliberately deleted latch lines (`codec_failed`, `mvec_failed`) are still deleted —
      restoring them would retry a broken `D3DCompile` forever.
- [ ] HDR codec identity: `transfer_strength = 0` is still a bit-exact no-op.
- [ ] Defaults that must stay OFF: `dlss_sr`, `rt_census`, and any chain/experimental key.
- [ ] No view is created on a game resource; nothing holds an `ID3D12Resource*` across passes
      (UE4's `FRenderTargetPool` recycles the object itself — pointer, desc and all).

## 9. Resource lifetime

- [ ] Every pipeline, root signature, texture and NGX feature created is destroyed on device
      teardown. Leaks found before: the mvec PSO and root signature were never destroyed.
- [ ] Nothing is freed that a render thread may still be using.
- [ ] A failed rebuild leaves the **previous working state**, never a half state.

## 10. The deployed build is the build we think it is

```sh
./build.sh >/dev/null 2>&1 && md5 -q stray_dlssnr.addon64 remix_nvngx.dll
ssh root@proxmox.lan "pct exec 113 -- md5sum <win64>/stray_dlssnr.addon64 <win64>/remix_nvngx.dll"
```
- [ ] Deployed md5s match a fresh build of `main`.
- [ ] Present next to the exe: `stray_dlssnr.addon64`, `remix_nvngx.dll` (**dual-slot**),
      `nvngx_dlssnr.dll`, `nvngx_dlss.dll`, `d3dcompiler_47.dll`, `dxgi.dll`.
- [ ] `ReShade.ini` has `DisabledAddons=` **empty**. A disabled add-on is indistinguishable from a
      broken build and cost an entire session once.
- [ ] Stale `.dxbc` caches cleared if any shader source changed (they are keyed by source hash, so
      a stale one is skipped rather than wrong — but a missing recompile hides a compile failure).
- [ ] Ownership is `deck:deck`. A root-owned add-on **silently fails to load**.

## 11. Claims about hardware came from hardware

- [ ] Every `[HW]` traces to a log line or command output, not an inference.
- [ ] Every claim of the form "X does not exist" names the method and its blind spots.

---

## 12. Then, and only then: the code review

With the checklist clean, run one review over the whole add-on — not a re-check of the boxes above,
but a fresh read for what a checklist cannot see: unsound abstractions, comments that no longer
describe the code, error paths nobody would notice failing, and complexity that no longer pays for
itself. Report findings ranked by severity with a concrete failure scenario each; an empty result is
a real outcome and padding it is worse than useless.

---

## Appendix: branches retired, and why

`§1` requires every branch to be merged or **abandoned with the reason recorded**. This is that record.

| branch | disposition |
|---|---|
| `mvec-decode`, `overlay-ui`, `rr-census`, `rr-experiments`, `parity-fixes`, `dlss-sr`, `renodx-hdr`, `nr-sr-chain`, `live-settings` | merged into `main` |
| `overlay-ui-concurrency-probe` | **retired unmerged.** A throwaway that explored the live-settings concurrency shape under both toolchains. Its two unique files, `src/live_settings.hpp` and `src/live_settings_probe.cpp`, were superseded by `abi/overlay_ui_probe.cpp`, which ships on `main` and is gated in CI. The branch was cut from an ancient `main` (its diff against current `main` is -25,492 lines), so nothing on it can be merged without reverting most of the project. |
| `nr-tuning-live` | open at time of writing — the five NR tuning controls |
