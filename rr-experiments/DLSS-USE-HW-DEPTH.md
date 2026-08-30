# `DLSS.Use.HW.Depth` — an add-on bug, but **not the one the brief describes**

Filed separately from the RR experiment matrix on purpose: this is a defect in the
shipping add-on's NGX plumbing, not an RR question, and no `Engine.ini` experiment
touches it.

The brief, following D4, says the flag "is silently wrong for UE4 today, in the shipping
NR path and in SR later." **Half of that is wrong and the other half is worse than
stated.** Measured, not inferred:

| DLL | size | sha256 (first 16) | `DLSS.Use.HW.Depth` |
|---|---|---|---|
| `nvngx_dlssnr.dll` — feature 18, what the add-on drives **today** | 165,840,496 | `c114f0f251dc45a9` | **absent** |
| `nvngx_dlssd.dll` — RayReconstruction (RR) | 79,687,792 | `4b43bc85d7f3c023` | present, file offset `0x11a218` |
| `nvngx_dlss.dll` — Super Resolution (SR) | 58,977,904 | `2ecf41de71383416` | present, file offset `0x132108` |

Byte-level search of the whole file for the literal string, ASCII **and** UTF-16LE
(the encoding mistake that burned earlier agents here) [HW]. The same search finds
`DLSS.Roughness.Mode`, `DLSS.Denoise.Mode` and `HW_Depth` with exactly the same
present/absent pattern.

## 1. It is NOT a bug in the shipping NR add-on

`nvngx_dlssnr.dll` contains **zero** occurrences of `DLSS.Use.HW.Depth`. Its parameter
namespace is `DLSSNR.*` — `DLSSNR.Depth`, `DLSSNR.DepthSubrectBaseX`, … [HW]. Setting
`DLSS.Use.HW.Depth` on that snippet would write into the parameter block and be read by
nobody.

So `nr_ensure_feature` omitting it (`src/stray_dlssnr.cpp:1877-1911`, which sets only
Width/Height/InputWidth/InputHeight/Enabled/RenderPreset/ScalingRatio/node masks/
FreeMemOnRelease) is **correct**, not an oversight. Nothing to fix, and adding the
parameter would be cargo cult.

What *is* true, and is a real risk rather than a real bug: the snippet exposes **no
depth-type selector at all**, so it has one hard-coded interpretation of `DLSSNR.Depth`
and we cannot choose it. If that interpretation is Linear while UE hands it
post-projection reverse-Z, the depth guide is wrong and there is no flag that fixes it.
That is a measurement question for whenever NR output quality is next assessed —
**it is not a settable-flag question.**

## 2. It IS a live bug for DLSS-SR, which is in flight

D4 says "SR has no depth-type field at all (`nvsdk_ngx_params.h:37-43`)". That is true of
the *public header* and false of the *binary*.

`nvngx_dlss.dll` reads it at feature-create. Disassembled, the site is structurally
identical to the RR one D4 traced — same shared NGX-core create-params reader, same
struct layout:

```
0x1800415b0   <function entry>
0x180041632   xor  r14d, r14d              ; r14d is this function's zero register
...
0x1800417d4   lea  r8,  [rbx + 0x24]       ; out-pointer = create_params + 0x24
0x1800417db   lea  rdx, [rip + 0xf1526]    ; -> "DLSS.Use.HW.Depth"  (VA 0x180132d08)
0x180041804   call qword ptr [rip + 0xebbee]
0x18004180a   cmp  eax, 1
0x18004180d   je   0x180041813
0x18004180f   mov  dword ptr [rbx + 0x24], r14d   ; parameter absent -> store 0
```

Exactly one rip-relative reference to the string in the whole image, and it is this one,
in executable code — so it is live, not a leftover in a shared string table [HW].

`NVSDK_NGX_DLSS_Depth_Type_Linear = 0`, `_HW = 1`
[SRC dlsssdk/nvsdk_ngx_defs_dlssd.h:29-33]. **The default is Linear.** UE 4.27 hands DLSS
the hardware depth buffer — post-projection, reverse-Z. So an SR integration that does
not set this parameter is telling the network the depth is view-space linear when it is
not.

NVIDIA's own helper sets it unconditionally on every create
[SRC dlsssdk/nvsdk_ngx_helpers_dlssd.h:548,242 — `NVSDK_NGX_Parameter_SetUI(pInParams,
NVSDK_NGX_Parameter_Use_HW_Depth, …)`], which is consistent with "you are expected to
always state this".

### Action for the SR work

* Set `DLSS.Use.HW.Depth = 1` **at feature create**, not at evaluate — the DLL reads it
  into the create-params struct, so setting it later has no effect.
* Keep `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` (bit 3) for reverse-Z. It is a
  separate concern and D4's reasoning about it stands.
* Same applies to `nvngx_dlssd.dll` the moment RR is wired up. Not urgent, but the
  create path should set it from day one rather than discovering it later.

### Confidence and what is still open

The parameter is read and defaults to 0; that is [HW] and solid. **What the network
actually does differently at 0 vs 1 is not traced** — D4 followed it as far as the
per-graph config structs and stopped, and so did I. It is plausible that a reverse-Z
depth with an infinite far plane is handled well enough either way. So: fix it because
it is free and it is what the SDK contract asks for, not because a specific artefact has
been attributed to it. A/B the flag once SR runs and read the `HW_Depth` telemetry field.

## 3. Also worth knowing while here

`DLSS.Roughness.Mode` is present in `nvngx_dlssd.dll` and `nvngx_dlss.dll` and absent
from `nvngx_dlssnr.dll` [HW]. So D4's suggestion to pack roughness into normals.w and set
`DLSS.Roughness.Mode = 1` is an RR-path option only; it does not exist for the feature-18
snippet.
