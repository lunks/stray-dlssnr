# STAGING — the DXR dispatch census

Copy-pasteable. **Nothing here has been run**: the game is running and this branch deploys nothing.
Every path below was read off the box read-only and is tagged `[HW]`.

The census answers D1 by measurement instead of argument: **which ray tracing effects Stray
compiles, whether `DispatchRays` actually happens, how often, and at what resolution.**

---

## 0. What you are deploying, and what it does when you do nothing

`stray_dlssnr.addon64` gains one new ini key, **`rt_census`, which defaults to `0`**.

The deployed `stray_dlssnr.ini` does not contain the key `[HW]`, so **after step 2 the census is
still off and the add-on behaves exactly as the build you are running today.** It only starts
measuring after step 3 adds the key.

With `rt_census = 0`, every census entry point returns after one relaxed atomic load — the
`init_pipeline` hook, the `SetPipelineState1` hook, the `DispatchRays` handler (which then returns
`false`, so ReShade issues the game's dispatch exactly as with no add-on present), the `present`
hook and the `destroy_device` hook. Nothing is counted, named, logged or allocated. The census
allocates nothing at any time, on or off: every table it keeps is a fixed-size array.

With `rt_census = 1` it is still read-only — it creates no resource, issues no command, and
suppresses nothing. It writes to `ReShade.log` and touches nothing else.

---

## 1. Paths, verified on the box

| what | where |
|---|---|
| game binaries dir (`S:\` inside the prefix) | `/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64` `[HW]` |
| the add-on | `…/Win64/stray_dlssnr.addon64`, owner `deck:deck`, mode `755` `[HW]` |
| the add-on's ini | `…/Win64/stray_dlssnr.ini`, owner `deck:deck` `[HW]` |
| ReShade config | `…/Win64/ReShade.ini` `[HW]` |
| ReShade log | `…/Win64/ReShade.log` — ReShade `6.8.0.2155` (64-bit) `[HW]` |
| game Engine.ini | `/home/deck/.local/share/Steam/steamapps/compatdata/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor/Engine.ini`, mode `444`, backups `.orig` and `.bak-pre-cvars` beside it `[HW]` |

```sh
# Set these once, then paste the blocks below verbatim.
BIN='/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64'
CT=113
```

**`chown deck:deck` is not optional.** `pct push` writes as root by default, ReShade runs inside
the Proton prefix as `deck` (uid 1001 `[HW]`), and a root-owned `.addon64` fails to load with
nothing useful in the log — indistinguishable from the `DisabledAddons` failure mode.

---

## 2. Deploy the add-on

Build locally (`./build.sh`, or download the `stray-dlssnr-msvc` artifact from the green CI run),
then:

```sh
# a) local -> Proxmox host
scp stray_dlssnr.addon64 root@proxmox.lan:/tmp/stray_dlssnr.addon64.new

# b) Proxmox host -> CT 113, into the SAME DIRECTORY as the live file, owned by deck.
#    --user/--group/--perms are supported by this pct  [HW: `pct help push`]
ssh root@proxmox.lan "pct push $CT /tmp/stray_dlssnr.addon64.new \
  '$BIN/stray_dlssnr.addon64.new' --user deck --group deck --perms 0755"

# c) belt and braces: assert the ownership rather than trusting the flags
ssh root@proxmox.lan "pct exec $CT -- chown deck:deck '$BIN/stray_dlssnr.addon64.new'"
ssh root@proxmox.lan "pct exec $CT -- chmod 755        '$BIN/stray_dlssnr.addon64.new'"
ssh root@proxmox.lan "pct exec $CT -- ls -l '$BIN/stray_dlssnr.addon64.new'"   # expect: -rwxr-xr-x deck deck

# d) keep a rollback, then swap ATOMICALLY within the same directory
ssh root@proxmox.lan "pct exec $CT -- cp -a '$BIN/stray_dlssnr.addon64' '$BIN/stray_dlssnr.addon64.bak-precensus'"
ssh root@proxmox.lan "pct exec $CT -- mv -f '$BIN/stray_dlssnr.addon64.new' '$BIN/stray_dlssnr.addon64'"
ssh root@proxmox.lan "pct exec $CT -- ls -l '$BIN/'"
```

`mv` inside one directory is a `rename(2)`: the file is never half-written, and if the game is
running it keeps the old inode until it exits.

**Rollback:** `mv -f "$BIN/stray_dlssnr.addon64.bak-precensus" "$BIN/stray_dlssnr.addon64"`.

---

## 3. Turn the census on

Two edits, both required.

**a) `stray_dlssnr.ini` — add the key.** It is absent today, so the census would otherwise stay off.

```sh
ssh root@proxmox.lan "pct exec $CT -- cp -a '$BIN/stray_dlssnr.ini' '$BIN/stray_dlssnr.ini.bak-precensus'"
ssh root@proxmox.lan "pct exec $CT -- bash -c \"printf '\nrt_census = 1\nrt_census_frames = 600\n' >> '$BIN/stray_dlssnr.ini'\""
ssh root@proxmox.lan "pct exec $CT -- tail -4 '$BIN/stray_dlssnr.ini'"
```

`rt_census_frames` is presents between summary blocks — 600 is about ten seconds at 60 fps. A
summary is also written at device teardown.

**b) `ReShade.ini` — re-enable the add-on.** It is currently disabled `[HW]`:

```
[ADDON]
DisabledAddons=STRAY DLSS-NR@stray_dlssnr.addon64
```

Change that line to `DisabledAddons=` (empty). Nothing in the census runs while the add-on is
disabled — ReShade never loads the DLL.

```sh
ssh root@proxmox.lan "pct exec $CT -- cp -a '$BIN/ReShade.ini' '$BIN/ReShade.ini.bak-precensus'"
ssh root@proxmox.lan "pct exec $CT -- sed -i 's/^DisabledAddons=.*/DisabledAddons=/' '$BIN/ReShade.ini'"
ssh root@proxmox.lan "pct exec $CT -- grep -n 'DisabledAddons' '$BIN/ReShade.ini'"
```

**c)** Optional but recommended for a clean read: truncate the log first, then relaunch Stray with
`-dx12`, load a save, and play for a minute or two so the periodic summary fires several times.

```sh
ssh root@proxmox.lan "pct exec $CT -- truncate -s 0 '$BIN/ReShade.log'"
```

If you only want the measurement and not the denoiser, also set `enabled = 0` in
`stray_dlssnr.ini`. The census is gated **only** by `rt_census`, deliberately — it runs
independently of the DLSS-NR pass.

---

## 4. The greps that answer D1

```sh
LOG="$BIN/ReShade.log"
R() { ssh root@proxmox.lan "pct exec $CT -- bash -c \"$1\""; }
```

### 4.1 One-line yes/no: does ray tracing execute?

```sh
R "grep -c 'the FIRST DispatchRays was observed' '$LOG'"
```

`1` ⇒ **DXR is executing.** `0` ⇒ go to §4.5.

### 4.2 The COMPILED set — which effects are *enabled*

```sh
R "grep -E '^ +(raygen|miss|closesthit|anyhit|intersect|callable) ' '$LOG' | sort -u"
```

Every line is an entry-point name ReShade read straight out of the DXIL `RDAT` reflection, i.e.
the engine's own string, with its effect spelled out. Example of what a positive D1 looks like:

```
    raygen     OcclusionRGS                             x6   -> RT SHADOWS  (one dispatch per shadow-casting light)
    raygen     RayTracingReflectionsRGS                 x4   -> RT REFLECTIONS (classic path)
    raygen     AmbientOcclusionRGS                      x4   -> RT AMBIENT OCCLUSION
    miss       RayTracingLightingMS                     x1   -> miss shader: RT reflections lighting
    closesthit MaterialCHS                              x212 -> hit group: FULL material closest hit
```

**This is effect enablement, not a guess.** Every `FDeferredShadingSceneRenderer::PrepareRayTracingXxx`
returns early on its own `ShouldRenderXxx` before adding a raygen shader — verified for reflections
(`RayTracingReflections.cpp:434`), AO (`RayTracingAmbientOcclusion.cpp:108`) and shadows
(`RayTracingShadows.cpp:183`). A raygen name is here only because some pass asked for it.

- `RayTracingReflectionsRGS` **or** `RayTracingDeferredReflectionsRGS` present ⇒ **D1 is right:
  reflections run, so demodulated specular radiance genuinely exists in-frame**, and the RR
  question becomes "can we reach it", not "does it exist".
- Only `OcclusionRGS` ⇒ the level's PostProcessVolume overrides `ReflectionsType` / `RayTracingAO`
  after all, and the pessimistic reading of D1 wins. Go to §5.

### 4.3 The DISPATCHED set — which effects actually *ran*, how often, how big

```sh
R "grep -E 'rg_slot=|shape:|extent range:' '$LOG' | tail -60"
```

Each bucket is one distinct `DispatchRays` signature:

- `count` / `frames` / `peak/frame` — how often. **Shadows dispatch once per shadow-casting light
  per frame**; reflections, AO and GI dispatch once each (plus their gather passes).
- `extent=WxHxD` — `1920x1080` is `View.ViewRect`: shadows, AO or debug. Anything smaller is
  `ScreenPercentage`-scaled: reflections, GI or sky light. **`height=1` is unmistakable** — that is
  a sorted deferred-material gather.
- `shape:` — `hit_stride=0` means `bAllowHitGroupIndexing=false`, a single default hit record
  (materials-off shadows, or a gather pass). A non-zero stride with hundreds or thousands of
  records is the full material SBT.
- `rg_slot=` — the raygen's index in UE's shader binding table. **UE gathers that table in a fixed
  order** (`DeferredShadingRenderer.cpp:1125-1136`):
  `Reflections, WaterReflections, Shadows, AmbientOcclusion, SkyLight, GI, GIPlugin, Translucency,
  Debug, PathTracing`, restricted to the enabled effects. So slot 0 belongs to the first name in
  that list that appears in §4.2, slot 1 to the second, and so on — that is the join between the
  two sets.

The slot index is derived from `raygen_offset - miss_offset`, which is independent of where the
SBT buffer happens to live, so a bucket survives UE reallocating the table. `rg_rel` and
`rtpso=0x…` are printed for identity; addresses are never part of the bucket key.

### 4.4 Everything at once

```sh
R "sed -n '/RT CENSUS FINAL/,/end RT CENSUS/p' '$LOG'"          # the teardown block
R "grep -n 'RT CENSUS summary' '$LOG' | tail -3"                # periodic blocks
R "grep -n 'RT dispatch: NEW signature' '$LOG'"                 # first sighting of each bucket
R "grep -n 'probe census @' '$LOG' | tail -2"                   # note the new rt= field
```

The old `dxil=` counter on the `probe census` line now carries `PS/CS ONLY - says NOTHING about
DXR`, and the line ends in `rt=MEASURED` or `rt=NOT MEASURED`. **`dxil=0` never meant "no ray
tracing"** — `on_init_pipeline` skipped every non-PS/CS sub-object, so a DXIL ray tracing library
could not reach that counter. That is fixed; the number can no longer be misread.

### 4.5 If the census reports nothing

```sh
R "grep -n 'RT CENSUS' '$LOG' | head"
```

| what you see | what it means | next |
|---|---|---|
| no `RT CENSUS is ON/OFF` line at all | the add-on did not load | check `DisabledAddons=` is empty, and `ls -l` shows `deck deck` on the `.addon64` |
| `RT CENSUS is OFF` | `rt_census` key missing or `0` | redo §3a |
| `COMPILED SET: EMPTY` **and** dispatches > 0 | this ReShade is a `RESHADE_ADDON==1` build, or a DXIL library had no `RDAT` part — `CreateStateObject`'s event is behind `#if RESHADE_ADDON >= 2` while `DispatchRays` is not | trust the dispatched set; the compiled set is unavailable |
| `COMPILED SET` non-empty **and** `DISPATCHED SET: EMPTY` | the expensive failure D1 flagged: the TLAS is built every frame and nothing traces against it | run the §5 A/B — RT is pure cost here |
| both empty | ray tracing genuinely does not run in this session | run the §5 A/B to confirm the log responds to the control |

Note the linked ray tracing PSO itself never reaches `init_pipeline` on this ReShade, by design of
ReShade's own code (`d3d12_device.cpp:2733-2736` bails when a `RAYTRACING_PIPELINE` desc has no
shader groups, which is exactly the shape UE 4.27 builds — `D3D12RayTracing.cpp:1976-1987`). That
is why the census reports two sets and joins them by slot order rather than by handle. Seeing
`with libraries=` non-zero in the summary would mean that assumption changed.

---

## 5. Queued follow-ups the census is the readout for

Each needs a config write and a restart — **not while the game is running.**

**a) Force every effect on.** Append to `[SystemSettings]` in `Engine.ini` (mode `444`, so
`chmod 644` first, edit, `chmod 444` back; `.orig` and `.bak-pre-cvars` are already beside it
`[HW]`):

```
r.RayTracing.ForceAllRayTracingEffects=1
```

Restart, re-read §4.2 and diff the raygen name set. **Do not also set
`r.RayTracing.UseTextureLod=1`** — only non-raycone material permutations were cooked (D2).

**b) The clean negative control.** Add to `GameUserSettings.ini` (which has no `[RayTracing]`
section today `[HW]`):

```
[RayTracing]
r.RayTracing.EnableInGame=False
```

Restart. The census must go completely silent, and `vkd3d_va_map_place_acceleration_structure`
must stop appearing in a fresh Steam log. This is also the cheapest measurement of what ray
tracing currently costs Stray: compare the frame time with and without.

**c) If §4.2 shows reflections.** The next question is whether `RayTracingReflections` and
`RayTracingReflectionsHitDistance` can be reached: `PF_FloatRGBA` at the `ScreenPercentage`-scaled
extent paired with a `PF_R16F` at the same extent is a distinctive pair. That is where D5's
cross-pass lifetime problem bites, and D5's answer stands — **carry pixels, not pointers.**
