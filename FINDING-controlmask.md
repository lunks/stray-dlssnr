# RESOLVED: the ControlMask hypothesis is dead, and the real gate is a `dynamic_cast`

Raised 2026-08-30 by the parent session; closed the same day against the deployed snippet,
`nvngx_dlssnr.dll` md5 `eea91faf55a8993656c66815f0497b3b`. Kept as a record of a refuted
hypothesis, because the *shape* of the mistake recurred twice more in the same investigation.

## The hypothesis

We write `DLSSNR.ControlMask` as an explicit null on every evaluate; renodx never writes the key
at all (`strings -a renodx-dlss5-v2.5.addon64 | grep -c "DLSSNR.ControlMask"` → 0). If the
snippet's test were "did `GetResource` succeed" rather than "is the pointer non-null", the null
would read as *present*, force `UseAutoMask = 0`, and bypass both structure strengths.

## Why it is refuted

Two independent mechanisms, either one sufficient:

```asm
0x180019f7d  movups xmmword ptr [rdi+0x60], xmm0   ; zeroed on entry, before the key is read
0x18001a4c6  mov    qword ptr [rdi+0x60], r14      ; the ABSENT path stores a null itself
0x18001aa4b  cmp    qword ptr [rdi+0x60], 0
0x18001aa50  je     0x18001aa59                    ; NULL -> the force-off is SKIPPED
0x18001aa52  mov    dword ptr [rdi+0xf0], r12d     ; only a NON-NULL mask sets UseAutoMask := 0
```

Absent leaves the zero in place; present-but-null stores a zero over a zero. Both reach
`0x18001aa4b` with `[rdi+0x60] == 0`. The two are exactly equivalent, so the write is safe and
removing it to match renodx would change nothing. `tools/ngx_paramblock_selftest.cpp` sections 4,
6 and 7 keep all three cases covered.

## What actually gates the three controls

Not the mask. `use_auto_mask` only *selects* what the effective structure pair at `+0xf8`/`+0xfc`
becomes. That pair is consumed at exactly one site, behind two `dynamic_cast` null tests
(`0x18007f5cc` is `__RTDynamicCast`):

| gate | cast at | tested at | target type |
|---|---|---|---|
| network | `0x180021cc8` | `0x18002253f` | `.?AVCCNetwork@HNetCpp@@` |
| layer | `0x18003f5e8` | `0x18003f5f3` | `.?AVCCTinlayoutFusedPreBlockSwin1HLayer@HNetCpp@@` |

If either returns null, `call 0x180061710` — the pure setter that stores the pair at `cb+0x98` /
`cb+0x9c` — never runs, and Local Structure, Skin Structure and Automatic Mask are inert
**together**. See `src/addon_config.hpp` for the full derivation.

## The recurring mistake, stated plainly

All three wrong answers in this investigation shared one shape: **a path was traced and reported
as a behaviour.**

1. "present-but-null makes the mask bound" — assumed the *test*, never read it.
2. "Intensity ≥ 1.0 skips the pass" — read `0x18001f51a`'s `je` and stopped, without checking
   that its caller stores the result to a flag at `0x1800191bd` and falls through.
3. "the structure strengths are proven to reach the network" — walked the call edges
   `0x19f30 → 0x21bb0 → 0x3f490 → 0x61710` and never looked at the guards sitting on them.

Reachability is not liveness. A call edge existing is not the edge being taken. Before claiming a
parameter is live, read every `test`/`cmp` between the parameter and its consumer.
