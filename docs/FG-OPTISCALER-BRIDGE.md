# DLSS‑SR × OptiScaler → DLSS‑G in Stray

## 1. Verdict

**DECIDED‑BY‑ONE‑EXPERIMENT — and the experiment needs none of our code.**

Not NO. The UE 4.27 backbuffer story is worse than either read described, but it is *not* the clean project‑closer we hoped for, because it cannot be settled by reading: `sl.dlss_g.dll` is a closed binary and the thing in question is its internal virtual‑backbuffer index policy.

What is now certain from source:

* `FGOutput=DLSSG` is **only** activated under `FGInput=Upscaler` [SRC `framegen/dlssg/DLSSG_Dx12.cpp:600-601`]. The configuration we want is not a hack — it is the only way OptiScaler exposes DLSS‑G at all, and the shipped ini documents it as the worked example [SRC `OptiScaler.ini:44`].
* The game's `IDXGISwapChain` becomes **the Streamline interposer proxy itself** (not a wrapper around it) — verifier finding 4, and it wins over Read 2's claim that `This` at `FG_Hooks.cpp:1205` is "not the game's object". **Read 2 was wrong there.**
* With DLSS‑G loaded, that proxy hands the app **off‑screen** buffers from `GetBuffer`, and NVIDIA says so explicitly ("host has no access to the swap-chain buffers directly"). UE 4.27 calls `GetBuffer(0..2)` **once per resize**, caches the three, and rotates with a purely local `++ % 3` counter that only advances when `PresentChecked()` succeeded [SRC `D3D12Viewport.cpp:214`, `:868-875`; `WindowsD3D12Viewport.cpp:203,207,380-389`]. It never asks the proxy which buffer to use.
* `eFailGetCurrentBackBufferIndexNotCalled` exists precisely for this, with the comment *"D3D integrations must use SwapChain::GetCurrentBackBufferIndex API"* [SRC `external/streamline/sl_dlss_g.h:137-138`], and `eIDXGISwapChain_GetCurrentBackBufferIndex` / `eIDXGISwapChain_GetBuffer` are both plugin‑overridable hook points [SRC `external/streamline/sl_hooks.h:56,60`].
* OptiScaler makes the call and throws the value away, once per presented frame while FG is active [SRC `hooks/FG_Hooks.cpp:1205`]. Read 2 asserted this is "purely to satisfy" the failure bit; there is **no comment saying so**, and it sits inside the Reflex/PCL marker block. The verifier is right — that purpose is `[ASSUMED]`.
* **OptiScaler never reads `DLSSGState::status`.** `slDLSSGGetState` is called twice, at swapchain creation, and only `numFramesToGenerateMax` / `bIsDynamicMFGSupported` are consumed [SRC `DLSSG_Dx12.cpp:118-126, 227-235`]. `dlssgDetectedInterpolationCount` is fed only from the *nvngxfg‑input* path [SRC `inputs/NVNGX_DLSS_Dx12.cpp:1113`], which Stray never takes. **Every DLSS‑G refusal in this stack is silent.** No log line, no toast, just an unchanged frame rate.

Why this is still live rather than dead: UE4's index policy (start at 0 after resize, +1 per successful Present, mod BufferCount) is the *same* policy a swapchain proxy's own virtual index would naturally follow. If both start at zero and both advance once per app Present, they stay in lockstep by coincidence of design, and OptiScaler's blind call clears the "was called" bit. That is a plausible story, not a proven one. `[ASSUMED]`

Why it is genuinely at risk: **at least four other components call `GetCurrentBackBufferIndex` on the same proxy** — OptiScaler's ImGui overlay [`menu/menu_overlay_dx.cpp:444`], its hudless compare [`shaders/hudless_compare/HC_Dx12.cpp:232`], its UI render pass [`shaders/render_ui/RUI_Dx12.cpp:175`], and ReShade's own D3D12 backend. If SL's hook has latching side effects, extra calls desync it. Add that OptiScaler ships `[FrameGen] ModifySCIndex` whose entire job is *"Trying to reset backbuffer index: {} with {} present calls"* [SRC `hooks/FG_Hooks.cpp:686-694`] — a workaround that only exists because index desync is a known, real failure class in this codebase.

**The experiment that decides it costs zero of our code and about an hour: run OptiScaler with `FGInput=upscaler, FGOutput=dlssg` on any other DX12 UE4/UE5 game you own that already ships DLSS or FSR2 upscaling, under this same Proton prefix.** That one run tests, simultaneously: UE‑generic backbuffer proxying, Streamline under vkd3d‑proton, HAGS‑less DLSS‑G on Ada, and synthetic‑Reflex sufficiency. If it generates frames there, Stray is an integration job. If it does not, the project closes and we never touch `wt-sr`.

---

## 2. The blocker nobody flagged, and it is live *today*

**`remix_nvngx.dll` is eaten by OptiScaler the moment OptiScaler is in the process — and it breaks the shipping DLSS‑NR feature, not just the SR work in progress.**

Mechanism, fully source‑verified:

* `DEFINE_NAME_VECTORS(nvngx, "nvngx", "_nvngx")` expands to `{"nvngx.dll", "nvngx", "_nvngx.dll", "_nvngx"}` [SRC `DllNames.h:9-32,125`].
* `CompareFileNameW` is a case‑insensitive **suffix** match applied to the *entire string* passed to `LoadLibrary`, path included [SRC `DllNames.h:173-186, 203-211`; `hooks/Kernel_Hooks.cpp:54,73,333`].
* `"…\Binaries\Win64\remix_nvngx.dll"` ends with `"nvngx.dll"`. Match.
* Branch taken: `EnableDlssInputs` (default **true** [SRC `Config.h:627`]) → `LOG_INFO("nvngx call: {0}, returning this dll!"); return dllModule;` [SRC `hooks/LibraryLoad_Hooks.cpp:72-81`].

Our loader does `LoadLibraryW(directory + L"remix_nvngx.dll")` [SRC `wt-sr/src/ngx_interop.hpp:711-712`] and gets **OptiScaler's module**. `GetProcAddress(…, "RemixNgxTrampoline_SetSnippet")` returns null, `trampoline_module` is nulled, and the feature aborts with the actively misleading message *"remix_nvngx.dll is present but does not export … so it is an OUT‑OF‑DATE trampoline. Rebuild and redeploy"* [SRC `ngx_interop.hpp:727-743`]. That will burn an afternoon.

`HookOriginalNvngxOnly=true` does **not** save you: the guard is `pos == npos` where `pos = libName.rfind(exePath)` with `exePath` lowercased and `libName` left in original case [SRC `LibraryLoad_Hooks.cpp:64-73`]. On a mixed‑case Wine path (`Z:\run\media\deck\GamesLinux\SteamLibrary\…`) the `rfind` misses, `pos == npos`, and the branch fires anyway.

**The only reliable lever is `[Inputs] EnableDlssInputs=false`.** That symbol is referenced in exactly one place in the whole tree — this branch [SRC grep: `Config.cpp:732,1511` (io) + `LibraryLoad_Hooks.cpp:72`]. It does **not** gate OptiScaler's own `NVSDK_NGX_D3D12_*` exports, which stay fully live for us to call by address. Turning it off costs nothing we need.

`nvngx_dlssnr.dll` and `nvngx_dlss.dll` are safe — neither suffix‑matches `nvngx.dll` or `_nvngx.dll`. Only the trampoline collides, because we deliberately named it to contain `nvngx.dll` for NVIDIA's caller gate. The trick that satisfies NVIDIA is the trick OptiScaler claims.

---

## 3. Corrections to the two reads

| Claim | Verdict |
|---|---|
| R1: "our parameter block satisfies every key the harvest reads" | **Holds**, and it is the good news. `MotionVectors`, `Depth`, `Jitter.Offset.X/Y`, `MV.Scale.X/Y`, `Reset`, `Width/Height/OutWidth/OutHeight`, `DLSS.Feature.Create.Flags`, `DLSS.Render.Subrect.Dimensions.*` are all already written correctly. |
| R1: "the harvest is only reachable from inside OptiScaler's own EvaluateFeature" | **Holds** [SRC `inputs/NVNGX_DLSS_Dx12.cpp:1044,1051,1054`]. This is the architectural cost: calling OptiScaler means **OptiScaler upscales**. |
| R1 G2: "needs the driver NGX core, or remix_nvngx redeployed as nvngx.dll" | **Resolved favourably, and the second half must be dropped.** `nvngx.dll` and `_nvngx.dll` are both in the prefix's `system32`, plus `/usr/lib/nvidia/wine/` [HW, `pct exec 113`]. `NVNGXProxy::InitNVNGX` will find them. Shipping our own `nvngx.dll` into `MainDllPath` would **shadow** that working core (`Util::LoadProxyLibrary` searches `MainDllPath` before the system path) and silently downgrade the backend to FSR 2.1.2 [SRC `inputs/NVNGX_DLSS_Dx12.cpp:1027-1039`]. Do not do it. |
| R2: "`This` at FG_Hooks.cpp:1205 is the SL swapchain, NOT the game's object" | **Wrong.** They are the same object. The verifier is right, and this is what makes §1 bite. |
| R2: "one Present per frame ⇒ UE4's counter is never desynced" | **Non‑sequitur.** Premise true, conclusion unearned. Once DLSS‑G is loaded, UE4's counter indexes DLSS‑G's off‑screen buffers, not the real swapchain's rotation; the Present count is irrelevant. |
| R2: "the UE 4.27 blocker does not apply" | **Not established. Default to false until measured.** |
| R2: "OptiScaler substitutes itself only for nvngx/_nvngx, so our stack is invisible to it" | **Half wrong, in the dangerous direction.** The `nvngx_dlss` half holds. The "invisible" half is false: it does not ignore our stack, it decapitates it (§2). |
| Both, implicit: "our SR calls the snippet, so the caller gate needs remix_nvngx" | **On the bridged path, remix_nvngx.dll is not needed for SR at all.** Chain is: our add‑on → OptiScaler's export → `feature->Evaluate` → `NVNGXProxy::D3D12_EvaluateFeature()` [SRC `upscalers/dlss/DLSSFeature_Dx12.cpp:52-53,88`] → the **NGX core** → the snippet. The return address the gate resolves belongs to the core, whose path is literally `nvngx.dll`. OptiScaler's install name is irrelevant to the gate. Slot B becomes dead code on this route. |
| R1: "one‑frame‑stale hudless because UpscaleEnd records the copy before Evaluate" | **Ordering holds** [SRC `:1051` before `:1054`]; intent is `[ASSUMED]`. |

---

## 4. What actually happens, per frame

```
UE4 RHI thread
  └─ our TAA-dispatch takeover (stray_dlssnr.addon64)
      └─ NVSDK_NGX_D3D12_EvaluateFeature   ← OptiScaler's export, resolved by address
          ├─ IFeature::UpdateOutputResolution
          ├─ UpscalerInputsDx12::UpscaleStart(InCmdList, params, feature)
          │    • fg->StartNewFrame()                       ← FG frame counter +1
          │    • reads Reset, MV.Scale.X/Y, Jitter.Offset.X/Y
          │    • reads FSR.cameraNear/Far/FovAngleVertical ← WE DO NOT SET THESE
          │    • Get("MotionVectors") → barrier→COPY_SOURCE, CopyResource, barrier back
          │    • Get("Depth")         → same
          │    • may block: "Waiting for present!" on OwnedMutex owner 2
          ├─ UpscalerInputsDx12::UpscaleEnd → Get("Output") → Hudfix hudless candidate
          └─ feature->Evaluate  →  DLSSFeatureDx12  →  NVNGXProxy  →  nvngx.dll core
                                                                   →  nvngx_dlss.dll

... later, on the app's single Present ...
FGHooks::FGPresent (Detour on SL proxy vtable slots 8/22)
  ├─ GetCurrentBackBufferIndex()  [return value discarded]
  ├─ slGetNewFrameToken(frameId = fg->FrameCount())
  ├─ PCLSetMarker(ePresentStart)
  ├─ DLSSG_Dx12::Present → Dispatch → slSetTagForFrame(Depth, MVec, HUDLessColor, UIColorAndAlpha)
  │                                 → slSetConstants → slDLSSGSetOptions
  ├─ o_FGSCPresent(This, ...)      [exactly once]
  └─ PCLSetMarker(ePresentEnd) → slReflexSleep
```

Three consequences worth internalising:

1. **`sl::Constants` is synthesised, and mostly wrong by default.** `clipToPrevClip` and `prevClipToClip` are set to **identity** [SRC `DLSSG_Dx12.cpp:462,471-474`]; camera pos/up/right/fwd are never supplied because `SetCameraData()` has no caller on the upscaler path, so the `else` branch feeds pos=(0,0,0), up=(0,0,1), right=(0,1,0), fwd=(1,0,0) [SRC `IFGFeature.cpp:230-240`; `DLSSG_Dx12.cpp:416,434-440`]. `cameraViewToClip` is rebuilt as `XMMatrixPerspectiveFovRH(vFov, aspect, near, far)`. **Our recovered real `ClipToPrevClip` has no channel into DLSS‑G.** That is a permanent quality ceiling on this path, not a bug we can fix.
2. **With no `FSR.camera*` keys set**, near/far fall back to ini `0.1 / 100000` **swapped by `DepthInverted()`** [SRC `Upscaler_Inputs_Dx12.cpp:54-71`], and vFOV to a hard‑coded **60°** [`:85`]. For UE4 reversed‑Z in centimetres that projection is nonsense — and for `FGOutput=dlssg` that projection *is* the entire camera model.
3. **`fg->StartNewFrame()` only fires inside `UpscaleStart`.** `GetDispatchIndex` returns −1 when `_frameCount == _lastDispatchedFrame` [SRC `IFGFeature.cpp:140-141`]. Every frame our SR bails is a frame with **no generated frame**. Two evaluates in one frame trip *"Frame count jumped too much"* [`:40-46`].

---

## 5. Concrete change list for `/private/tmp/.../scratchpad/wt-sr`

Ordered by how painful each is to retrofit. **The first three are cheap now and expensive later — say them to whoever is writing SR right now.**

### 5.1 `src/dlss_sr.hpp` — add three floats to `evaluate_desc` **today** ⚠ painful to retrofit

`evaluate_desc` is filled by `stray_dlssnr.cpp` at the dispatch site. Adding fields after that caller is finalised means reopening the plumbing.

```
// in evaluate_desc
float camera_near      = 0.0f;   // UE4 GNearClippingPlane, from the View CB
float camera_far       = 0.0f;
float camera_vfov_rad  = 0.0f;

// in evaluate_feature(), alongside the existing set_f32 block:
if (e.camera_vfov_rad > 0.0f) {
    ngx::set_f32(p, "FSR.cameraNear",             e.camera_near);
    ngx::set_f32(p, "FSR.cameraFar",              e.camera_far);
    ngx::set_f32(p, "FSR.cameraFovAngleVertical", e.camera_vfov_rad);   // RADIANS
}
```

Harmless on the direct path (the snippet ignores unknown keys); load‑bearing on the bridged one. `FsrUseFsrInputValues` defaults **true**, and the values are used iff *not both zero* [SRC `Upscaler_Inputs_Dx12.cpp:51-71`]. `ue4_jitter.hpp` already maps the View CB these come from.

### 5.2 `src/dlss_sr.hpp` / `src/stray_dlssnr.cpp` — one gate for "do we evaluate this frame" ⚠ painful to retrofit

The bail paths are currently scattered — jitter read failure, geometry moved, `sr_latched_off`, `pending_teardown`, menus with no TAA dispatch. On the bridged path each one silently drops an FG frame. Funnel them **now** into a single `should_evaluate()` predicate with a latch, so that later you can implement either policy without archaeology:

* *policy A*: on bail, skip (FG cadence stutters), or
* *policy B*: on bail, latch FG off entirely for N frames rather than generating an interpolated frame from stale inputs.

Retrofitting a single gate over a dozen scattered `return`s is the expensive version of this.

### 5.3 `src/ngx_interop.hpp` — verify the trampoline is actually ours ⚠ cheap now, an afternoon later

After `LoadLibraryW(directory + L"remix_nvngx.dll")`, confirm the module you got is the file you asked for:

```
wchar_t got[MAX_PATH]; GetModuleFileNameW(s.trampoline_module, got, MAX_PATH);
// compare against trampoline_path (case-insensitive, lexically normal)
```

If they differ, emit the **right** diagnostic — *"another in-process module claimed the LoadLibrary of remix_nvngx.dll. This is OptiScaler's `[Inputs] EnableDlssInputs` redirect: it suffix-matches any path ending in nvngx.dll. Set `EnableDlssInputs=false` in OptiScaler.ini."* — instead of "OUT‑OF‑DATE trampoline". **This applies to DLSS‑NR slot A too**, i.e. the feature that runs today, and it fires the first time OptiScaler is installed regardless of whether the SR bridge is ever built.

A structural alternative worth considering, if the snippet's gate is a `contains` test and not an `endswith` test (our own comment in `ngx_interop.hpp:743` says *contains* — **re‑verify against the disassembly before relying on it**): put the trampoline at `Win64\nvngx.dll\remix_trampoline.dll` — a *directory* named `nvngx.dll`. Path still contains `nvngx.dll`, filename no longer suffix‑matches anything OptiScaler claims, and the collision is gone permanently regardless of ini.

### 5.4 `src/ngx_interop.hpp` — a third resolve route

Add `spec_optiscaler()` alongside `spec_dlssnr()` / `spec_dlsssr()`:

* **Do not** find OptiScaler via `LoadLibraryW(L"nvngx.dll")` — that trick relies on the exact hook you are about to disable. Enumerate loaded modules (PSAPI `EnumProcessModules`, or walk the PEB) and take the first that exports `NVSDK_NGX_D3D12_Init_Ext` **and** `NVSDK_NGX_D3D12_EvaluateFeature` **and** is neither `s.snippet_module` nor `s.trampoline_module` nor our own add‑on. Log its `GetModuleFileNameW` so the log says which file answered.
* `require_trampoline = false`, `trampoline_prefix = "NVSDK_NGX_D3D12_"`.
* Signatures already match — no ABI change: `Init_Ext(u64, const wchar_t*, ID3D12Device*, version, const void*)`, `CreateFeature(cmdList, u32 featureId, void* params, void** outHandle)` with `*outHandle == nullptr` so OptiScaler allocates, `EvaluateFeature(cmdList, handle, params, nullptr)`, `ReleaseFeature(handle)`, `Shutdown1(device)` [SRC `inputs/NVNGX_DLSS_Dx12.cpp:148,418,730,805,1076`]. Our typedefs at `ngx_interop.hpp:554-560` line up.
* **`InApplicationDataPath` must be non‑null**: `State::NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath)` is unguarded [SRC `:164`]. We already pass `dir.c_str()`. A null `FeatureCommonInfo` is fine [`:157-158`].
* `Init_Ext` is what sets `UpscalerInputsDx12::_device` — without it `UpscaleStart` bails immediately [SRC `:218`; `Upscaler_Inputs_Dx12.cpp:11-17,96-97`]. **The NGX Init call is mandatory, not optional.**
* Never call `NVSDK_NGX_D3D12_DestroyParameters` on our hand‑laid block. It is safe (missing alloc‑type key → *"Leaking. return false"* [SRC `NVNGX_Parameter.h:255-289`]) but pointless.

### 5.5 `src/addon_config.hpp` — `sr_route`

`sr_route = direct | optiscaler`, default `direct`. Not just an evaluate‑time switch: it changes which module owns feature creation, handle lifetime, and release. Structure the feature struct so the route is a property of the created feature, not a global read at each call.

### 5.6 Things you can now stop worrying about

* Every `DLSS.Input.*.Subrect.Base.*` / `DLSS.Output.Subrect.Base.*` key is **ignored** by OptiScaler — those strings appear only in `Logger.h`'s name table. Our zeros make it moot.
* `DLSS.Render.Subrect.Dimensions.Width/Height` **is** read, and it overwrites `_renderWidth/_renderHeight`, which are then the dimensions the FG is told for Velocity and Depth [SRC `upscalers/IFeature.cpp:176-179,227-228`]. Our view‑rect‑vs‑texture‑extent distinction survives into FG. Keep writing it.
* With backend = DLSS, OptiScaler forwards **our own parameter block object** verbatim, rewriting only create flags, W/H/OutW/OutH, `Sharpness`→0, and (only if `RenderPresetOverride=true`, default false) the preset slots [SRC `upscalers/dlss/DLSSFeature.cpp:33-108`; `DLSSFeature_Dx12.cpp:52-53,92`]. `DLSS.Use.HW.Depth`, `DLSS.Enable.Output.Subrects`, `FreeMemOnReleaseFeature`, the ratio‑selected preset hint — all reach the snippet unchanged. The whole disassembly‑derived create contract survives, **as long as the backend is DLSS**.
* `ExposureTexture`: only required when the AutoExposure create flag is clear. `sr_auto_exposure` defaults true [SRC `wt-sr/src/addon_config.hpp:423`]. Leave it.
* No caller‑module, device‑provenance, or app‑id gate anywhere on OptiScaler's NGX path.

---

## 6. On-disk layout and load order

Current state of `…/Stray/Hk_project/Binaries/Win64/` [HW]:

```
dxgi.dll                 5,592,064   ← ReShade
remix_nvngx.dll             15,872
nvngx_dlss.dll          58,956,400
nvngx_dlssnr.dll       165,840,496
stray_dlssnr.addon64       567,296
ReShade.ini / stray_dlssnr.ini
```

Target:

```
Win64/
  dxgi.dll                 ← OptiScaler, renamed from OptiScaler.dll   [OUTER]
  OptiScaler.ini
  ReShade64.dll            ← the CURRENT dxgi.dll, renamed             [INNER]
  ReShade.ini              (unchanged)
  stray_dlssnr.addon64
  stray_dlssnr.ini
  nvngx_dlssnr.dll
  nvngx_dlss.dll
  remix_nvngx.dll          ← still required for DLSS-NR slot A; unused by bridged SR
  streamline/              ← HARDCODED folder name, no search [SRC Streamline_Proxy.h:84-88]
    sl.interposer.dll
    sl.common.dll
    sl.dlss_g.dll
    sl.reflex.dll
    sl.pcl.dll
    nvngx_dlssg.dll        ← copy of /usr/lib/nvidia/wine/nvngx_dlssg.dll (9,289,784 B, drv 610.43.02) [HW]
```

`LoadReshade` loads **`ReShade64.dll` from the exe's parent directory** — exactly `Win64/` [SRC `dllmain.cpp:1135-1145`; `hooks/Dxgi_Hooks.cpp:46,84`]. The rename is mandatory; the name is hardcoded.

Load order:

1. Wine resolves `dxgi.dll` from the exe directory → **OptiScaler**.
2. OptiScaler reads `OptiScaler.ini`, Detours `kernel32!LoadLibrary*`, `ntdll` loader, DXGI and D3D12 entry points.
3. `[Plugins] LoadReshade=true` → `LoadLibrary(Win64\ReShade64.dll)` → ReShade initialises → ReShade loads `stray_dlssnr.addon64`.
4. `D3D12CreateDevice` → `D3D12Hooks::HookDevice` → with `activeFgOutput == DLSSG`, `slInit` (appId `0x0F71CA1E`, features `{DLSS_G, Reflex, PCL}`, `eUseManualHooking | eUseFrameBasedResourceTagging | eUseDXGIFactoryProxy`, `eAllowOTA`/`eLoadDownloadedPlugins` **cleared**) [SRC `Streamline_Proxy.h:296-348`; `D3D12_Hooks.cpp:2220-2223`].
5. UE4 `CreateSwapChain` → `FGHooks::CreateSwapChain` → `new DLSSG_Dx12()` (driven by ini, **not** by our NGX call) [SRC `FG_Hooks.cpp:120-122,177`] → `slUpgradeInterface`'d factory creates the swapchain → returned **unwrapped** to UE4 → UE4's `SwapChain1`/`SwapChain4` are the SL proxy.
6. Per frame: our add‑on → OptiScaler's `NVSDK_NGX_D3D12_Init_Ext` (once) / `CreateFeature` / `EvaluateFeature`.
7. UE4 Presents once → `FGHooks::FGPresent` → DLSS‑G dispatch and its own async present queue.

### `OptiScaler.ini` — the non-optional set

```ini
[Upscalers]
Dx12Upscaler=dlss                  ; default (auto) is XeSS; DLSS must be named
[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=dlssg
FGNvngxReplacement=None
SkipResizeBuffers=false            ; leave default; true injects presents into UE4's counter
ModifySCIndex=false                ; ditto — and its GetBuffer loop has a real bug (FG_Hooks.cpp:679)
[OptiFG]
HUDFix=true                        ; default false; without it Output is never captured as hudless
HUDFixExtended=false               ; flip only if the format check rejects our Output
EnableDepthScale=false             ; escape hatch for R32G8X24_TYPELESS depth
[Inputs]
EnableDlssInputs=false             ; MANDATORY — see §2
[Plugins]
LoadReshade=true
[Hotfix]
MotionVectorResourceBarrier=64     ; D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
DepthResourceBarrier=<measure>     ; 64 = NON_PIXEL_SHADER_RESOURCE, 8 = UNORDERED_ACCESS, 192 = ALL_SHADER_RESOURCE
RestoreComputeSignature=false
RestoreGraphicSignature=false
[Log]
LogToFile=true
LogLevel=1
[FSR]
CameraNear / CameraFar / VerticalFov   ; leave auto once §5.1 lands; static fallback otherwise
```

`MotionVectorResourceBarrier=64` is not optional for us specifically. Stray matches OptiScaler's Unreal detection (`-win64-shipping.exe` [SRC `Util.cpp:271-288`]; exe is `Stray-Win64-Shipping.exe` [HW]), and **for Unreal titles with that key unset OptiScaler auto‑assumes `UNORDERED_ACCESS`** [SRC `upscalers/ffx/FFXFeature_Dx12.cpp:249-256`] — while our decoded guide is left in `shader_resource_non_pixel` right before evaluate [SRC `wt-sr/src/stray_dlssnr.cpp:3185-3186`]. Wrong state ⇒ D3D12 validation error on the game's command list, because OptiScaler records `barrier(state→COPY_SOURCE); CopyResource; barrier(back)` on **our** list [SRC `IFGFeature_Dx12.cpp:521-536`].

One free win worth noting: OptiScaler patches `sl.dlss_g`'s config JSON with `["external"]["hws"]["required"] = false` [SRC `Streamline_Hooks.cpp:1014-1015`], removing the HAGS requirement — which does not exist under Proton. On a 4090, `StreamlineSpoofing` does nothing (arch is spoofed only *below* Ada [SRC `Streamline_Hooks.cpp:774-784`]), so the JSON patch is the only thing carrying us there.

---

## 7. Bring-up ladder

Everything below is **queued** — Stray is running, no writes to the box.

**Rung 0 — the decider. Zero of our code.** Install OptiScaler on a *different* DX12 UE4/UE5 game in this prefix that already ships DLSS or FSR2. `FGInput=upscaler, FGOutput=dlssg`, `LogLevel=1`.
* Frame rate roughly doubles → **§1 is resolved YES**, proceed to rung 1.
* Frame rate unchanged, no errors in `OptiScaler.log` → DLSS‑G is refusing silently. Since OptiScaler never reads `DLSSGState::status`, you need an external probe: check whether `sl.dlss_g`'s own verbose log (`logLevel = eVerbose` with a callback is already set [SRC `Streamline_Proxy.h:296-348`], so it lands in OptiScaler's log) names a failure bit. `eFailGetCurrentBackBufferIndexNotCalled` → **project closes**. `eFailReflexNotDetectedAtRuntime` → rung 0b. `eFailCommonConstantsInvalid` → the camera constants, fixable.
* Crash at startup with a `ResizeBuffers` failure → the `ALLOW_TEARING` mismatch: OptiScaler unconditionally ORs `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` into the creation desc [SRC `DLSSG_Dx12.cpp:103,214`] while forwarding UE4's own flags verbatim to `ResizeBuffers` [SRC `FG_Hooks.cpp:711-758`]; UE4 sets it only if `CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` succeeds [SRC `WindowsD3D12Viewport.cpp:19,71-98`], and its `ResizeBuffers` goes through `VERIFYD3D12RESULT_LAMBDA` [`:376`] — a fatal error, not a fallback.

**Rung 0b — Reflex.** Only `ePresentStart` / `ePresentEnd` / `slReflexSleep` are synthesised; there is no synthetic `eSimulationStart/End` or `eRenderSubmitStart/End` [SRC `FG_Hooks.cpp:1210,1285-1294`]. Whether that clears `eFailReflexNotDetectedAtRuntime` **under Wine** is the second biggest unverified risk. In our favour: this prefix already has `DXVK_NVAPI_VKREFLEX=1`, the `dxvk-nvapi-vkreflex-layer` implicit layer is loaded, and `/home/deck/nvapi64.log` shows `NvAPI_D3D_SetLatencyMarker: OK` [HW].

**Rung 1 — OptiScaler alongside our stack, FG OFF.** Install OptiScaler as `dxgi.dll`, ReShade as `ReShade64.dll`, `LoadReshade=true`, `EnableDlssInputs=false`, `[FrameGen] Enabled=false`. Verify DLSS‑NR still works exactly as today. If the add‑on log says *"OUT‑OF‑DATE trampoline"*, `EnableDlssInputs` did not take — §2.

**Rung 2 — SR through OptiScaler, FG still OFF.** `sr_route=optiscaler`, `Dx12Upscaler=dlss`, `Enabled=false`. Check `OptiScaler.log` for `calling NVNGXProxy::D3D12_Init_Ext result: 1`. Image should be DLSS‑quality. If it looks like FSR, the DLSS backend failed to init and OptiScaler fell back to **FSR 2.1.2** [SRC `inputs/NVNGX_DLSS_Dx12.cpp:1027-1039`] — check `NVNGXProxy::InitNVNGX` in the log.

**Rung 3 — FG on, no hudfix.** `Enabled=true`, `FGInput=upscaler`, `FGOutput=dlssg`, `HUDFix=false`. Expect the HUD to smear. If frame rate does not move, `UpscaleStart` is bailing: check `_device` (Init_Ext reached?), `FGEnabled`, `currentSwapchain != nullptr`, and the `"Depth or Velocity is not ready, skipping"` line [SRC `DLSSG_Dx12.cpp:327-334`]. If you get D3D12 validation errors, it is the barrier states.

**Rung 4 — hudfix.** `HUDFix=true`. Note the ordering property: the hudless `CopyResource(Output→captureBuffer)` is recorded on our command list **before** `feature->Evaluate` writes `Output` [SRC `:1051` then `:1054`], so the captured "hudless" is the previous frame's contents. Whether that reads correctly is only knowable by looking. `HUDLimit=1` means first candidate wins; the format must match the swapchain unless `HUDFixExtended=true` [SRC `Hudfix_Dx12.cpp:404-423`].

**Rung 5 — camera constants.** Land §5.1, A/B the motion quality with and without. This is where "FG that technically runs" becomes "FG that looks right".

---

## 8. Cost, risk, and the case for shipping SR alone

**Cost of the bridge:** roughly 200–300 lines in `wt-sr` (a third resolve route, three parameter writes, a route switch, one gate), plus an install layout with five extra DLLs, plus a second ImGui overlay and a second set of input hooks in the process (OptiScaler ships its own menu — that conflict is unresolved and becomes real the moment `FGInput=upscaler` is on).

**What the bridge costs you architecturally, permanently:**
* **OptiScaler does the upscale, not us.** Our `dlss_sr.hpp` evaluate becomes a data‑provider and dispatch site. The create contract survives only on the DLSS backend.
* **`clipToPrevClip` is forced to identity** into DLSS‑G. Our recovered real matrix — one of the better pieces of work in this tree — has no channel and is thrown away for FG purposes.
* Camera basis vectors are fed as the degenerate `(0,0,0)/(0,0,1)/(0,1,0)/(1,0,0)` fallback.
* Every silent failure mode is genuinely silent, because `DLSSGState::status` is never read.

**The honest counter‑argument, and it is strong: measure the frametime first.** Stray is a small UE 4.27 title. On a 4090 rendering **1920×1080 internal** it is very likely engine‑ or CPU‑bound well north of 100 fps before FG. Frame generation below roughly 60 fps base is where it earns its latency; above the display's refresh it is pure added latency and pure added artifact surface, for zero perceived smoothness. **That measurement has not been made, and it is the cheapest thing on this entire list.** If SR alone at 1080p→4K lands Stray at or above the panel's refresh, the correct answer is: ship SR, do not build the bridge, and the whole of §5 collapses to items 5.1 and 5.3 (three floats and a correct diagnostic, both worth having anyway).

Recommendation: **do rung 0 and the frametime measurement before writing any bridge code.** They are independent, both cheap, and either one can close the project.

---

## 9. What remains [ASSUMED], and the cheapest measurement for each

| # | Assumption | Cheapest measurement |
|---|---|---|
| A1 | `sl.dlss_g`'s virtual backbuffer index advances 1‑per‑app‑Present from 0 after resize, so UE4's independent `++ % 3` stays in lockstep. Closed binary; not derivable. | **Rung 0.** Any DX12 UE4/UE5 game with an upscaler, `FGOutput=dlssg`. Watch for stale/duplicated frames, not just fps. |
| A2 | `ePresentStart` + `ePresentEnd` + `slReflexSleep` alone clear `eFailReflexNotDetectedAtRuntime` **under Wine/vkd3d‑proton**. Evidently works on Windows; nobody has shown it on Linux. | Rung 0, plus grep OptiScaler's log for the SL verbose callback naming the bit. |
| A3 | OptiScaler's blind `GetCurrentBackBufferIndex()` inside Present is what satisfies the DLSS‑G requirement. No comment says so; it sits in the Reflex block. | Same run. If DLSS‑G generates, it is satisfied by *something*; if not, this is the first suspect. |
| A4 | The FFX/Streamline runtimes accept a verbatim `R32G8X24_TYPELESS` depth copy (desc preserved, `ALLOW_DEPTH_STENCIL` included) [SRC `IFGFeature_Dx12.cpp:454-504`]. The format mapping lives in the external FFX/SL runtime. | Rung 3. If depth is rejected, set `[OptiFG] EnableDepthScale=true` → `R32_FLOAT` UAV at display res [SRC `shaders/depth_scale/DS_Dx12.cpp:16`]. |
| A5 | Stray draws its UMG/HUD **after** the TAAU dispatch we take over, so `Output` is a valid hudless source. Standard UE4 ordering, not verified against Stray's render graph. | Rung 4, visually. Or one RenderDoc capture offline. |
| A6 | The pre‑upscale hudless capture ordering (`UpscaleEnd` before `Evaluate`) is intentional one‑frame‑back behaviour, not a latent lag. | Rung 4. HUD ghosting by exactly one frame is the tell. |
| A7 | `DXGI_FEATURE_PRESENT_ALLOW_TEARING` is supported by this vkd3d‑proton/DXVK build, so UE4 and OptiScaler agree on the swapchain flag. | One line in ReShade's or OptiScaler's log at startup, or a trivial probe. Cheap, and it is a hard crash if wrong. |
| A8 | ReShade self‑initialises correctly when loaded as `ReShade64.dll` by another module rather than as the DXGI proxy. Well‑established in practice, unverified here. | Rung 1. |
| A9 | The snippet's caller gate is a `contains "nvngx.dll"` test, not `endswith` — this decides whether the `Win64\nvngx.dll\remix_trampoline.dll` directory trick is available as a permanent fix for §2. | Re‑read the gate in the existing disassembly notes. No run needed. |
| A10 | Stray's actual frametime at 1080p→4K with SR alone on this 4090, versus the panel's refresh. **This one decides whether any of the above matters.** | Ten minutes with the existing overlay, next time the user is not mid‑playthrough. |