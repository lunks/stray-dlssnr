# Verified difference against the reference: we write ControlMask, renodx never does

Measured by the parent session, 2026-08-30, before the diagnosis workflow reported.

| | |
|---|---|
| **ours** | `nr_clear_resource(p, s_mask)` at `src/stray_dlssnr.cpp:4438`, called on **every accepted evaluate**. It does `ngx::set_res(p, "DLSSNR.ControlMask", (ID3D12Resource*)nullptr)` plus four `set_u32` zeros for the subrect family. |
| **renodx** | `strings -a renodx-dlss5-v2.5.addon64 \| grep -c "DLSSNR.ControlMask"` → **0**. The string is not in the binary. It never sets the key in any form. |

## Why this is the leading hypothesis

`nr_clear_resource`'s own comment already documents the mechanism, and a previous author wrote the
null **specifically to avoid it**:

> *"Leaving the previous frame's pointer there both dangles and, for the control mask specifically,
> keeps the snippet forcing UseAutoMask to 0 (which kills BOTH structure strengths) long after the
> mask went away."*

That fix rests on an unverified assumption: **that a present-but-null entry is equivalent to an
absent one.** If the snippet's test is "did `GetResource` succeed" rather than "is the returned
pointer non-null", then writing null every frame makes the mask *present*, forces
`UseAutoMask = 0`, and takes the constant path that bypasses both structure strengths —
matching the user's report exactly, including which controls survived.

Disassembly that motivates it (`nvngx_dlssnr.dll`, md5 `eea91faf…`):
```asm
cmp  QWORD PTR [rdi+0x60], 0
je   keep
mov  DWORD PTR [rdi+0xf0], r12d   ; r12d = 0  ->  UseAutoMask := 0
keep:
cmp  DWORD PTR [rdi+0xf0], 0
je   use_constant                 ; bypasses BOTH structure strengths
```

## What must be established, not assumed

1. **Is `+0x60` actually the ControlMask slot?** The parent session assumed it. Prove it by finding
   where `[reg+0x60]` is written and correlating with the resource-getter call for that key.
2. **How does the snippet distinguish absent from present-but-null?** Look at the getter's return
   handling at the call site: `and eax,0xfff00000 / cmp eax,0xbad00000` is the failure test used
   everywhere else. If the ControlMask read uses that test and null still returns success, the
   hypothesis holds.
3. **`Intensity` at `+0xdc` had ZERO `movss` sites** — it may not live where assumed. Pin it
   separately; it is one of the five dead controls and may have a different cause.

## The cheap experiment

Stop writing the key at all when no mask is bound — i.e. match renodx exactly — and keep the
subrect zeros only if they are separately required. If the five controls come alive, done.

**Check every other parameter written as an explicit null or zero for the same hazard** before
concluding. This is a class of bug, not one instance.
