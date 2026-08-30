# Addendum 2 — `DLSS.Use.HW.Depth` must be set **at CreateFeature**, not at Evaluate

Upgraded from `[ASSUMED]` to `[HW]` by disassembly of `nvngx_dlss.dll` (2026-08-30). The parameter
has exactly **one** rip-relative xref to its string in the whole image, in executable code, and it
is on the **feature-create** path:

```
0x180041632   xor   r14d, r14d
0x1800417d4   lea   r8,  [rbx + 0x24]            ; out = create_params + 0x24
0x1800417db   lea   rdx, [rip + 0xf1526]         ; -> "DLSS.Use.HW.Depth"
0x180041804   call  qword ptr [rip + 0xebbee]
0x18004180d   je    0x180041813
0x18004180f   mov   dword ptr [rbx + 0x24], r14d ; ABSENT -> store 0
```

Consequences, in order of how easy each is to get wrong:

1. **Set it at `CreateFeature`. A set at Evaluate is a no-op** — the value is latched into the
   create-params struct at `+0x24` and never re-read. This is the failure that looks like the flag
   "not working" when it was simply set too late.
2. **The default is Linear (0)**, proven by the `absent -> store 0` branch above.
   `NVSDK_NGX_DLSS_Depth_Type_Linear = 0`, `_HW = 1`
   **[SRC** `dlsssdk/nvsdk_ngx_defs_dlssd.h:29-33` **]**.
3. **UE 4.27 hands DLSS the hardware depth buffer** — post-projection, reverse-Z. STRAY's `t0` is
   `r32_g8_typeless` and `DepthInverted=1` is measured **[HW]**. So the correct value is **1**, and
   leaving it at the default is silently wrong. NVIDIA's own helper sets it unconditionally on
   every create **[SRC** `nvsdk_ngx_helpers_dlssd.h:548`, `:242` **]**.
4. `DepthInverted` (create-flag bit 3) is a **separate** concern. Keep setting it. The two are not
   alternatives.

**Honest limit:** that the parameter is read, and defaults wrong, is solid **[HW]**. *What the
network does differently at 0 vs 1 has not been traced.* Set it because it is free and it is what
the SDK contract asks for — **not** because any artefact has been attributed to it. A/B it once SR
renders.

Related, also **[HW]**: `DLSS.Roughness.Mode` exists in `nvngx_dlss.dll` and `nvngx_dlssd.dll` but
is **absent from `nvngx_dlssnr.dll`**, so any "pack roughness into normals.w" scheme is an SR/RR
option only and has no meaning on the NR path.
