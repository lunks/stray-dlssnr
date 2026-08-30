# DLSS-G feasibility — where FG actually sits

## 1. Distance, in the units we've been using

Three different answers, because "FG" is three different projects:

| Thing | Distance | Why |
|---|---|---|
| **Driving `nvngx_dlssg.dll` compute directly** | **days** | Every gate is open. Contract is tiny. Inputs mostly already exist in our tree. |
| **Presenting the generated frame from inside our add-on** | **weeks–months, and probably the wrong architecture** | Not an API prohibition — a ~5k-LOC swapchain-ownership port, plus a UE4-specific killer (§2). |
| **FG on Stray at all, by any means** | **one evening of configuration + one measurement** | And it wouldn't be our add-on doing it (§4). |

The interesting result of this pass is that the second row is **not** the only route, and it is not the one to take. The prior brief's framing — "FG is the fourth feature, blocked by a wall" — is wrong in both halves. FG is not blocked, and it is not our feature.

---

## 2. The wall, named correctly

The earlier claim was "ReShade's add-on API structurally cannot present, because `addon_event::present` is `void`." That fact is real but the inference off it is wrong, and the correction matters.

**What's true** [SRC `wt-mvec/include/reshade_events.hpp:1941-1942`]:
```cpp
RESHADE_DEFINE_ADDON_EVENT_TRAITS(addon_event::present, void, api::command_queue*, api::swapchain*, ...);
RESHADE_DEFINE_ADDON_EVENT_TRAITS(addon_event::finish_present, void, api::command_queue*, api::swapchain*);
```
vs. `create_swapchain, bool` at :1843 and `set_fullscreen_state, bool` at :1943. So you cannot *cancel* the game's present through the event.

**Why that isn't the wall.** FG doesn't need to cancel a present, it needs an *extra* one — and an add-on is handed the raw object to do it with: [SRC `reshade-main/source/reshade_api_object_impl.hpp:79`] `uint64_t get_native() const final { return (uint64_t)_orig; }`, where [SRC `reshade-main/source/d3d12/d3d12_impl_swapchain.hpp:16`] `class swapchain_impl : public api::api_object_impl<IDXGISwapChain3 *, api::swapchain>`. `Present`/`GetBuffer`/`GetCurrentBackBufferIndex` are all callable. ReShade's author has also explicitly written *around* DLSS-FG coexisting with his wrapper [SRC `reshade-main/source/dxgi/dxgi_swapchain.hpp:117`]:

> `// but if this is not the queue the swap chain was created with (DLSS Frame Generation e.g. creates a separate high priority one for presentation), D3D12 removes the device.`

**The actual wall is UE 4.27's backbuffer bookkeeping.** [SRC `scratchpad/ue/ue427/Engine/Source/Runtime/D3D12RHI/Private/D3D12Viewport.cpp:870-875`]:
```cpp
const bool bNativelyPresented = PresentChecked(SyncInterval);
if (bNativelyPresented || (CustomPresent && CustomPresent->NeedsAdvanceBackbuffer()))
{
    CurrentBackBufferIndex_RHIThread++;
    CurrentBackBufferIndex_RHIThread = CurrentBackBufferIndex_RHIThread % NumBackBuffers;
    BackBuffer_RHIThread = BackBuffers[CurrentBackBufferIndex_RHIThread].GetReference();
```
and `grep -rn GetCurrentBackBufferIndex` across the whole UE 4.27 tree returns **0 hits**. Stray never asks the swapchain where it is; it counts locally, mod 3. Any extra `Present` we sneak in advances the real swapchain by one and permanently desyncs UE4's counter — from then on Stray renders into a buffer that isn't the current one. That is exactly why `sl.dlss_g` carries `cloneFakeBuffers` / `tex2d.fake-swapchain-buffer` / `resizeProxyBuffers` [BIN `sl.dlss_g.dll` strings]: the game must be handed *fake* backbuffers and never see the real ring.

So the wall is: **you cannot present an extra frame opportunistically; you must own the buffer ring, which means proxying the swapchain, which means being outside ReShade rather than inside it.** That is a bounded, well-understood port — ~5.7k LOC of it exists in open source (§4) — not an impossibility.

**Secondary wall, unquantified:** flip metering. `sl.dlss_g` ships `flipMetering.cpp` / `flipTracker.cpp` / `vblankTracker.cpp` [BIN]. On Ada that path leans on driver/DWM flip metering; under `gamescope --backend drm` there is no DWM and we don't own the compositor, so pacing almost certainly degrades to the CPU pacer thread. [ASSUMED — not measured; this is the thing most likely to make FG *feel* bad here even when it works.]

---

## 3. What the NGX half looks like (confirming the good news, briefly)

All of this reproduces cleanly and I have no corrections to it:

- Feature ID **11**, `MinHWArchitecture = 0x190` [BIN `nvngx_dlssg.dll` @0x180013415 / @0x180013420; `NVSDK_NGX_GetGPUArchitecture` @0x180013e40 = `mov eax,0x190; ret`]. 0x190 = `NV_GPU_ARCHITECTURE_AD100` [HW `/home/deck/build/proton-ge-custom/dxvk-nvapi/external/nvapi/nvapi.h:2770`]. The 4090 *is* Ada. Cubins are `.target sm_89` ×70 and `sm_120` ×31 only. **No arch gate to patch** — the only one of the four features where that's true.
- Caller-module gate is byte-identical to NR's (`GetModuleHandleEx(FROM_ADDRESS)` → `GetModuleFileNameW` → `wcsstr` on UTF-16 `L"nvngx.dll"`). `remix_nvngx.dll` carries over unchanged.
- No Streamline handshake in the snippet — imports are only KERNEL32/ADVAPI32/USER32/VERSION.
- Required per-Evaluate params: exactly **3** (`DLSSG.Reset`, `ClipToPrevClip`, `PrevClipToClip`). Required at create: **2** (`Width`, `Height`). Everything else defaults.
- 11 resource slots, IDs 67–77; outputs (`OutputInterpolated` 75, `OutputReal` 76) are **textures we allocate**. Zero presentation vocabulary anywhere in the binary.

And the Linux plumbing FG needs is present on this box [HW]:
- Driver ships the Wine PE: `/var/lib/flatpak/runtime/org.freedesktop.Platform.GL.nvidia-610-43-02/.../lib/nvidia/wine/nvngx_dlssg.dll`, 9,289,784 bytes.
- `dxvk-nvapi` implements the exact pacing primitive `sl.dlss_g` calls: `NvAPI_D3D12_NotifyOutOfBandCommandQueue` [HW `dxvk-nvapi/src/nvapi_d3d12.cpp:1088`, forwarding to vkd3d's `ID3D12DeviceExt::NotifyOutOfBandCommandQueue` at :1122] and `NvAPI_D3D12_SetAsyncFrameMarker`.
- FG driver-override keys exist: `NGX_DLSSG_MODE`, `NGX_DLSSG_MULTI_FRAME_COUNT`, `NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX`, `NGX_DLSS_FG_OVERRIDE` [HW `dxvk-nvapi/src/util/util_drs.h:12-17`].

**Stray's actual present target** [HW `Stray/.../ReShade.log`, 02:12 today]: ReShade 6.8.0.2155 loaded as `dxgi.dll`; `ResizeBuffers(BufferCount=3, 3840x2160, NewFormat=24, SwapChainFlags=0x802)`; `SwapEffect = 4`; then `SetFullscreenState(TRUE)`. Format 24 = `R10G10B10A2_UNORM`, which dodges `eDLSSGStatusFailHDRFormatNotSupported — 64bit HDR formats are not supported` [BIN]. Flags 0x802 = `ALLOW_TEARING | ALLOW_MODE_SWITCH`. Display is HDMI-A-1 at 3840x2160 under `gamescope --backend drm --adaptive-sync --hdr-enabled --hdr-itm-enabled` [HW `ps`]. (`vrr_capable` on `card0-HDMI-A-1` read back empty — VRR actually being active is unconfirmed.)

---

## 4. The four paths

### (A) Direct `nvngx_dlssg` + our own present layer, inside the add-on
Build proxy backbuffers, a fence graph, a pacer thread, vblank tracking. **Why not:** it is a from-scratch reimplementation of `dlfgSwapchain.cpp` + `dlfgPresent.cpp` + `pacer.cpp` + `fenceHolder.cpp` + `vblankTracker.cpp` + `vsyncState.cpp`, the add-on stops being an add-on, and there is a real chance it never paces acceptably on Wine→vkd3d→gamescope. Highest cost, highest failure risk, zero reuse.

### (B) Ship Streamline ourselves and fake a game integration
`sl.interposer` + `sl.common` + `sl.dlss_g` + `sl.reflex` + `sl.pcl`, with our add-on doing `slSetTag`/`slSetConstants`/PCL markers. **Why not:** this is precisely path (C) with all the work re-done by us and none of it tested. Streamline also demands Reflex active at runtime, `GetCurrentBackBufferIndex` every frame (which UE 4.27 never calls), VSync clamped to 1, and a single present queue — and `sl.interposer` must be ahead of ReShade in the loader path. Every one of those problems is already solved in (C).

### (C) ★ **OptiScaler as the outer layer, ReShade + our add-on inside it** — recommended
The prior brief's central negative claim — "OptiScaler does not offer DLSS-G as an FG output" — was true of the **v0.9.4-final build in the July log** and is **false of current OptiScaler**. [SRC `scratchpad/optiscaler/OptiScaler/State.h:44-50`]:
```cpp
enum class FGOutput : uint32_t { NoFG, FSRFG, DLSSG, XeFG };
```
and [SRC `optiscaler/OptiScaler.ini:35-39`]:
```
; Selected FG Output
;   dlssg  - requires streamline dlls inside 'OptiScaler/streamline' folder + nvngx_dlssg.dll
; nofg, fsrfg, xefg, dlssg - Default (auto) is nofg
```
The implementation is real: `framegen/dlssg/DLSSG_Dx12.cpp` (1,186 LOC) drives `slInit`, `slUpgradeInterface`, `slSetTag`/`slSetTagForFrame`, `slSetConstants`, `slDLSSGSetOptions`/`slDLSSGGetState`, `slReflexSetOptions`/`slReflexSleep`/`slReflexSetCameraData`, `slPCLSetMarker`, `slGetNewFrameToken`. Around it: `wrapped/wrapped_swapchain.cpp` 1,372, `hooks/DxgiFactory_Hooks.cpp` 1,951, `hooks/FG_Hooks.cpp` 1,449, `hudfix/Hudfix_Dx12.cpp` 930, `framegen/**` 8,709 total. That is the entire subsystem (A) and (B) would have us write, already written, DX12, and already exercised on this box.

Two more facts make this specifically ours:

1. **ReShade coexistence is a supported, documented topology, with OptiScaler outer** [SRC `optiscaler/OptiScaler.ini:1015-1018`]: `LoadReshade` — *"Loads Reshade64.dll from game's exe folder / Rename Reshade dll to ReShade64.dll, put next to OptiScaler and set to true"* — plus [:1479-1483] `CreateD3D12DeviceForLuma`: *"OptiScaler will delay loading of Reshade and create D3D12 device first / This option will prevent warnings/crashes with Luma and some other ReShade mods."* Luma is a ReShade add-on doing exactly the kind of thing we do. Somebody has already walked this.
2. **OptiScaler exports the NGX entry points itself** [SRC `optiscaler/OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp:1076`] `NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(...)`, and `inputs/FG/Upscaler_Inputs_Dx12.cpp` (289 LOC) harvests FG inputs from that dispatch — which is what `FGInput=upscaler` means. `Hudfix_Dx12.cpp` exists specifically to solve the UI-over-interpolated-frame problem for that input mode.

**The bridge, and the honest caveat.** Stray has no upscaler for OptiScaler to hook, so OptiScaler alone gives nothing. The proposed wiring is: our add-on calls **OptiScaler's** `NVSDK_NGX_D3D12_*` exports instead of `remix_nvngx.dll` for SR; OptiScaler sees a DLSS-SR dispatch with colour/depth/mvec/jitter; `FGInput=upscaler` + `FGOutput=dlssg` + `Hudfix` does the rest, and Streamline owns the swapchain with ReShade loaded beneath it. **This specific composition is [ASSUMED] — I verified every component independently, but nobody has run this stack.** It is, however, the only route where the expensive half is someone else's problem.

### (D) FSR3-FG inside our add-on
Same present problem as (A) (`FfxFrameInterpolationSwapChainDX12` replaces the swapchain), plus worse image quality than DLSS-G on an Ada card that has the OFA. **Why not:** all of (A)'s cost, none of (A)'s ceiling.

---

## 5. What `/home/deck/opti.log` actually showed — [HW], read today

50,094,923 bytes, 889,525 lines, timestamped Jul 25 19:11. Session 18:53:30 → 19:11:37, ~18 minutes, clean `DLL_PROCESS_DETACH`.

- **Game:** `FINAL FANTASY VII REMAKE` (`S:\common\FINAL FANTASY VII REMAKE\End\Binaries\Win64\`) — also UE4, also under Proton/Wine. Not Stray.
- **Config** [:13-15]: `Upscalers.Dx11Upscaler: fsr31_12`, `FrameGen.Enabled: true`, `FrameGen.FGInput: Upscaler`.
- **FG never engaged** [:316-317], verbatim:
  ```
  [18:53:36.282187] [W] CheckForFGStatus FGOutput is not set to FSR-FG or XeFG
  [18:53:36.282201] [W] FGHooks::CreateSwapChain Can't init FG Feature or invalid FGOutput setting!
  ```
  `FGOutput` was never set. In v0.9.4-final that message's enumeration is the whole valid set — **DLSSG was not an option in that build**. So this is a config-plus-version miss, **not** evidence that FG-class tech fails here, and **not** evidence that OptiScaler can't do DLSS-G (it can now).
- **Swapchain** [:315]: `DxgiFactoryHooks::CreateSwapChain Width: 3840, Height: 2160, Format: 24, Count: 3, Flags: 42, Hwnd: 200AE, Windowed: 1, SkipWrapping: false`, then [:335-337] `Created new swapchain: 18E38420` / `Created new WrappedIDXGISwapChain4: 18E57BF0`. The wrapper installed fine.
- **Topology it had to fight** [:58-59, :223-230]: `Vulkan_wDx12::Hook Attaching Vulkan vkQueueSubmit hook`, then per-queue-family command pools (`queue family 2`, `queue family 0` ×2). The DXGI swapchain is Vulkan-backed; anything present-time needs a VK shim underneath the D3D12 one.
- **FG runtimes loaded successfully** [:80-82, :109-113]: `amd_fidelityfx_framegeneration_dx12.dll` and `libxess_fg.dll` both loaded from the game dir. The loading machinery worked; only the output selection was empty.
- **No vkd3d/Proton errors** anywhere in 889k lines beyond those two FG warnings and missing optional AMD DLLs.
- The prior pass reported 118.3 fps mean / p99 14.170 ms over 126,931 frames with `SyncInterval 0` — I did not re-derive that; treat as [HW, prior pass, unverified here].

**Net:** the log is a null result on FG, but a *positive* result on everything around it — swapchain wrapping, the Vulkan shim, gamescope accepting free-running presents, and clean shutdown, all on this exact box.

---

## 6. Cheap next measurements, in order

All box work is **QUEUED** — nothing here runs while you're playing.

| # | Measurement | Cost | Decides |
|---|---|---|---|
| 1 | **Frametime baseline for Stray at 4K with SR landed.** MangoHud/present-time capture, 5 min of normal play. | free, no writes beyond a log | Whether FG is worth *any* of this. If Stray already sits at 120+ fps p50 on a 4090, FG buys nothing and costs latency — and the whole question closes here. **Do this first.** |
| 2 | **Prove FG-class present works on this box, in a game that already ships it.** Launch Cyberpunk 2077 or Stellar Blade (both have `nvngx_dlssg.dll` + `.nvngx-backup/…310.7.0.0` deployed [HW]), turn FG on, capture frametime histogram under gamescope. | ~20 min, one game launch, no writes | Whether Ada FG paces acceptably under `gamescope --backend drm` *at all*. If p99 frame-interval variance is bad here even in a shipping integration, routes A/B/C all die on §2's flip-metering risk and the answer is "don't". |
| 3 | **Confirm DLSS-G has ever actually engaged here.** Set NGX `EnableLogPathOverride`/`LogPath` for one known-good game. **WRITE — queued.** | one registry write + one launch | Turns "inferred from DLL deployment" into observed, and produces a *reference log of a working DLSSG parameter set* — free ground truth for any later direct-NGX work. |
| 4 | **Build current OptiScaler, run it on FFVII Remake with `FGOutput=dlssg`.** Same game as the July log, so it's a controlled re-run of a known session. Needs `OptiScaler/streamline/` populated from `/Users/lunks/Downloads/SL 2.13/` + `nvngx_dlssg.dll`. **WRITE — queued.** | ~2 h incl. build | Whether the Streamline-shim-into-a-non-Streamline-game route works on Linux at all. This is the single measurement that decides route (C). |
| 5 | **OptiScaler + ReShade coexistence smoke test**, `LoadReshade=true`, with a trivial add-on, no FG. | ~30 min | Whether our NR/SR/RR survives being loaded *underneath* OptiScaler. If it doesn't, (C) collapses and FG is dead for this project. |
| 6 | Only if 1–5 all pass: **wire our add-on's NGX calls through OptiScaler's exports** and try `FGInput=upscaler` + `FGOutput=dlssg` on Stray. | days | The actual feature. |

Nothing before step 6 requires writing a line of NGX FG code. If steps 1 or 2 come back badly, everything after them is moot — which is the point of the ordering.

---

## 7. Ranking against the in-flight work

**SR (implementing now) — unambiguously first.** It is the same move as NR (intercept a dispatch, substitute a better one), it's mid-flight, and *it is a hard prerequisite for FG mattering*: FG interpolates the presented 4K frame, and without SR there's no reason to be running 1080p internal. Do not divert.

**RR (parked, one killer measurement pending) — second.** Same architecture as NR/SR, and it is one measurement from a yes/no. Cheap to resolve, and resolving it is worth more than any amount of FG analysis.

**UI convergence — third, and it is doing double duty.** NR+SR+RR under one live UI is the north star, and it's also the thing that makes FG *configurable* if FG ever arrives via (C) (OptiScaler's own overlay would otherwise be a second, competing UI in the same process — a real integration problem to think about before, not after).

**FG — fourth, and structurally different from the other three.** Rank it below all of the above, for three independent reasons:

1. **It isn't the same kind of work.** NR/SR/RR are "take over a dispatch." FG is "own the swapchain." Shipping it inside the add-on means the add-on stops being one. Shipping it via (C) means the add-on doesn't ship it at all — OptiScaler does, and we're a passenger.
2. **The north star survives either way, but changes shape.** "One add-on with NR+SR+RR and a live UI" stays intact under (C) — FG becomes a documented *stack configuration*, not a fourth checkbox. That's an honest and defensible outcome, but it should be a deliberate choice, not a surprise.
3. **The payoff is the weakest of the four.** Stray is a 2022 UE4 title on a 4090. Measurement #1 may well show FG has nothing to add but latency. NR, SR and RR all improve the image at every framerate; FG only helps if we're framerate-bound, and we probably aren't.

**Bottom line:** FG is not dead and not a research problem — the snippet is drivable in days and the Linux plumbing is all present. But the half that matters is present control, that half already exists as ~5.7k LOC of open-source DX12 code we did not write, and the correct move is to measure whether it's worth having (step 1, then 2) before doing anything else. If step 1 says Stray is already fast, close the question and finish SR.