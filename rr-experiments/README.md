# Stray RR experiment matrix

Four experiments, ordered by decisiveness per game restart. Each restart is the scarce
resource, so every experiment below either answers a question no cheaper one can, or is
not here.

Read [`VERDICT-CRITERIA.md`](VERDICT-CRITERIA.md) **before** running anything. It is
pre-registered and it says plainly what this matrix can and cannot prove.

---

## If you only run one: **E1**.

E1 needs **no `Engine.ini` edit at all** — one Steam launch-option change, play for a
few minutes, then `ls` a directory. It answers the RR-critical question directly (does
Stray run a ray-traced *radiance* pass today, i.e. does the signal RR needs exist at
all), it is the only experiment here with no way to produce an ambiguous result, and it
calibrates the baseline that makes every later experiment attributable.

If E1 shows `RayTracingReflectionsRGS`, the demodulated radiance D2 described is already
in the frame today and no ini experiment was ever needed for that question.

---

## 0. The measurement channel, and why it is not the add-on

D1 wanted a census of which ray generation shaders exist, and concluded it needed an
add-on deploy. It does not. vkd3d-proton will hand us the same census for an environment
variable:

* `VKD3D_SHADER_DUMP_PATH=<dir>` makes vkd3d write every shader it compiles into `<dir>`
  [SRC vkd3dsrc README.md:291].
* For a **DXR library**, the SPIR-V for each export is written as
  `<16-hex-hash>.lib.<EXPORT_NAME>.spv`
  [SRC vkd3dsrc/libs/vkd3d-shader/vkd3d_shader_main.c:159-177, formatted at :32-38].
* `<EXPORT_NAME>` is the **demangled** entry point — i.e. the literal HLSL name
  [SRC dxil.c:1980-1981, fed from `dxil_spv_parsed_blob_get_entry_point_demangled_name`
  at :2410]. UE 4.27 renames its exports back to the original entry name
  (D1, `D3D12RayTracing.cpp:163-186`), so the filenames read
  `….lib.RayTracingReflectionsRGS.spv`, `….lib.OcclusionRGS.spv`, and so on.
* Setting the variable **forces `PIPELINE_LIBRARY_IGNORE_SPIRV`**
  [SRC vkd3dsrc/libs/vkd3d/device.c:1254-1260], so the cached pipeline library cannot
  suppress the compile — the dump is guaranteed to happen.

### Why the dump is exactly a per-effect enable census

`FDeferredShadingSceneRenderer::DispatchRayTracingWorldUpdates` builds one raygen array
by calling `PrepareRayTracingReflections`, `…Shadows`, `…AmbientOcclusion`,
`…GlobalIllumination`, `…Translucency`, … in sequence, then creates the material RTPSO
from whatever accumulated [SRC ue427rt/DeferredShadingRenderer.cpp:1125-1147]. **Each
`Prepare…` self-gates on its own `ShouldRender…` predicate and returns early otherwise**
— verified individually:

| effect | gate | cite |
|---|---|---|
| reflections | `if (!ShouldRenderRayTracingReflections(View)) return;` | RayTracingReflections.cpp:434 |
| ambient occlusion | `if (!ShouldRenderRayTracingAmbientOcclusion(View)) return;` | RayTracingAmbientOcclusion.cpp:108 |
| shadows | `ShouldRenderRayTracingEffect(r.RayTracing.Shadows > 0)` | RayTracingShadows.cpp:183-188 |
| global illumination | `if (!ShouldRenderRayTracingGlobalIllumination(View)) return;` | RayTracingGlobalIllumination.cpp:656 |

So the set of raygen names in the dump is the set of enabled effects. **With two
exceptions that will lie to you** — see §3.

### The raygen names to look for

All twelve are present in `Stray-Win64-Shipping.exe` as UTF-16 [HW]:

| name | means | source |
|---|---|---|
| `OcclusionRGS` | RT shadows | RayTracingShadows.cpp:170 |
| `AmbientOcclusionRGS` | RT ambient occlusion | RayTracingAmbientOcclusion.cpp:104 |
| `RayTracingReflectionsRGS` | RT reflections | RayTracingReflections.cpp:345 |
| `RayTracingDeferredReflectionsRGS` | RT reflections, experimental deferred path | RayTracingDeferredReflections.cpp:317 |
| `GlobalIlluminationRGS` | RTGI, brute force | RayTracingGlobalIllumination.cpp:454 |
| `RayTracingCreateGatherPointsRGS` / `…TraceRGS` / `RayTracingFinalGatherRGS` | RTGI, final gather | :531 / :598 / :652 |
| `SkyLightRGS` | RT sky light | RayTracingSkyLight.cpp:268 |
| `RayTracingPrimaryRaysRGS` | RT translucency — **the E2 canary** | (RayTracingPrimaryRays.cpp) |
| `RayTracingDebugMainRGS`, `PathTracingMainRG` | editor view modes; expect never | — |

### Cost, and the two ways to get this wrong

* **Empty the dump directory before every run.** `vkd3d_shader_dump_blob` opens with
  `"wbx"` — exclusive create [SRC vkd3d_shader_main.c:43]. A stale file from the previous
  experiment is never overwritten and is indistinguishable from a fresh result. This is
  the single most likely way to produce a wrong answer here.
* **Expect a heavy first-run stutter and a large directory.** `PIPELINE_LIBRARY_IGNORE_SPIRV`
  makes every pipeline recompile. `/home` had 23 GB free at the time of writing [HW];
  keep an eye on it, and do not leave the variable set after the experiment.

---

## 1. E1 — baseline RT census · **no ini edit** · [`ini/E1-baseline.ini`](ini/E1-baseline.ini)

`ini/E1-baseline.ini` is the *revert target*, not something to install. For E1, leave
`Engine.ini` exactly as it is.

**Do:** add `VKD3D_SHADER_DUMP_PATH` to the launch options (§5), start the game, play
until you are outside in a lit street scene for a couple of minutes, quit.

**Tests:** which ray-traced effects Stray runs today, with no intervention. This is D1's
still-open #1 and #2.

**Expected, if D1's reading of the stock `FPostProcessSettings` defaults is right**
(`ReflectionsType = RayTracing`, `RayTracingAO = 1`, both CVars at -1 so the PPV decides):
`OcclusionRGS` **and** `RayTracingReflectionsRGS` **and** `AmbientOcclusionRGS`.

| outcome | means |
|---|---|
| `RayTracingReflectionsRGS` present | Ray-traced reflections run **today**. The specular-albedo-demodulated radiance D2 described already exists in every frame. Skip E2 entirely; go to E3. |
| Only `OcclusionRGS` (+ possibly `AmbientOcclusionRGS`) | Shadows/AO only. A level `PostProcessVolume` is overriding `ReflectionsType`, which is precisely the one thing D1 could not rule out. Go to E2. |
| No `*.lib.*.spv` at all, but plenty of `.dxbc`/`.spv` | No RT pipeline was built this session. Either the session never reached RT content, or something bigger is wrong. Re-run before concluding. |
| Directory empty or missing | Measurement failure, not a result. Fix the path (§5) and re-run. |

**Positive control:** `OcclusionRGS` must be present. `r.RayTracing.Shadows` defaults to
1 [SRC ue427rt/LightRendering.cpp:60-65] and nothing in the shipped inis overrides it,
and acceleration structures are demonstrably being built right now
(117 `vkd3d_va_map_place_acceleration_structure` warnings in the live Proton log [HW]).
If `OcclusionRGS` is missing while other names are present, stop — the model is wrong.

**Abort condition:** none. E1 cannot damage anything; it makes no writes to the game
config.

**Informs DLSS-SR:** the *method* does — this is how to see which passes SR's target
frame contains without deploying anything. The result does not.

---

## 2. E2 — force reflections on, with a channel canary · [`ini/E2-force-reflections.ini`](ini/E2-force-reflections.ini)

Run **only** if E1 showed no `RayTracingReflectionsRGS`.

**Tests two orthogonal things in one launch:**

1. **Canary — does `[SystemSettings]` reach the render thread at all?**
   `r.RayTracing.Translucency=1` should make `RayTracingPrimaryRaysRGS` appear, a name
   E1 measured as absent. It is a fully independent gate
   [SRC ue427rt/RayTracingTranslucency.cpp:131-142], so it cannot be confounded with the
   reflections result.
2. **Can reflections be forced past a PostProcessVolume override?**
   `r.RayTracing.Reflections=1` beats the volume because a non-negative CVar value wins
   over `View.FinalPostProcessSettings.ReflectionsType`
   [SRC ue427rt/RayTracingReflections.cpp:396-406].

**Why the canary is `r.RayTracing.Translucency` and not `r.Tonemapper.Sharpen`:**
D2 proposed `r.Tonemapper.Sharpen=8`. **Do not use it.** `r.Tonemapper.Sharpen` is one of
exactly six CVar names that appear in the shipping binary inside Stray's own
`/Script/Hk_project.HKGameUserSettings` string cluster — alongside `r.MotionBlur.Amount`,
`r.SkeletalMeshLODBias`, `r.ViewDistanceScale`, `r.StaticMeshLODDistanceScale` and
`r.ScreenPercentage` [HW, `exe_u16.txt` lines 8888-8899]. The game's own settings code
writes it. If it writes at `ECVF_SetByCode` (0x08) that beats `SetBySystemSettingsIni`
(0x04) and the canary silently loses — a false "the channel is dead" that would abort
this whole matrix for no reason. **No `r.RayTracing.*` CVar appears in that cluster** [HW].

| outcome | means | next |
|---|---|---|
| `RayTracingPrimaryRaysRGS` **and** `RayTracingReflectionsRGS` appear | Channel live, reflections forceable. | E3 |
| `RayTracingPrimaryRaysRGS` appears, `RayTracingReflectionsRGS` does not | Channel live; reflections genuinely cannot be made to run. **This is the DEAD branch** — confirm with E4 before calling it. | E4 |
| Neither appears | **The channel did not deliver.** Every negative in this launch is void. | E5 |
| `RayTracingPrimaryRaysRGS` absent but `RayTracingReflectionsRGS` appears | Contradiction — the ini reached one CVar and not another. Stop and re-derive. | — |

**Also visible on screen:** RT reflections replace SSR entirely — they are mutually
exclusive, `bScreenSpaceReflections = !RayTracingReflectionOptions.bEnabled && …`
[SRC ue427rt/IndirectLightRendering.cpp:735]. So off-screen geometry starts appearing in
Stray's wet-street and neon reflections. That is the classic SSR tell and it corroborates
the dump.

**Abort condition:** if the game does not reach the main menu, revert to
`ini/E1-baseline.ini` (§5) and stop. RT translucency is the most invasive line in this
matrix and is the first thing to drop.

**Informs DLSS-SR:** no.

---

## 3. E3 — raw undenoised demodulated radiance · [`ini/E3-raw-radiance.ini`](ini/E3-raw-radiance.ini)

The RR-decisive experiment. Run once reflections are known to run.

**Tests:** whether the engine denoiser can be switched off, leaving the raw Monte-Carlo
ray generation output as `ReflectionsColor` — the signal class RR consumes.
`r.Reflections.Denoiser=0` ⇒ `bDenoise = false`
[SRC ue427rt/IndirectLightRendering.cpp:754] ⇒ the final `else` branch assigns
`ReflectionsColor = DenoiserInputs.Color` [:838], with no screen-space denoiser between
the rays and the composite.

**Expected observable:** gross Monte-Carlo noise in every reflective surface, at 1 spp
with `MaxRoughness=1.0`. Stray's wet streets and neon are close to a best case for
seeing it. It should be unmistakable, not a matter of taste — if you are squinting, treat
that as the DEAD-pending-recheck case in `VERDICT-CRITERIA.md` §1.

| outcome | means |
|---|---|
| Reflections become visibly noisy | The undenoised, specular-albedo-demodulated radiance texture exists in-frame **on hardware**, not just in the source reading. RR's input signal class is real in this title. **NOT DEAD.** |
| Reflections stay clean, off-screen geometry still reflecting | Reflections ran but `r.Reflections.Denoiser=0` did not take. Since E2's canary already proved the channel, this points at something specific to that CVar — re-check the fragment actually on disk before drawing conclusions. |
| Reflections revert to SSR-looking (nothing off-screen) | The reflections lines did not take this launch, even though they did in E2. Suspect the file was rewritten — check the mode is still `444` (§5). |

**Attribution note:** there is no dump-based observable for the denoiser. The SSD passes
are compute shaders and vkd3d dumps those without names [SRC dxil.c:1472 — plain `.dxbc`,
no export tag]. The visual is the observable. That is acceptable *because* the change is
gross, and because E2 has already carried the channel proof.

**Abort condition:** frame rate unplayable → drop `MaxRoughness` to 0.6 and re-run. The
test still works; the noise is just confined to smoother surfaces.

**Informs DLSS-SR:** no.

---

## 4. E4 — RTGI, the diffuse-demodulated signal · [`ini/E4-rtgi.ini`](ini/E4-rtgi.ini)

Lowest priority. Worth a launch only if E3 succeeded and the diffuse guide is wanted too,
or if E2 said reflections cannot run and RTGI is the last candidate before declaring RR
dead.

**Tests:** whether RTGI can be forced on, giving the diffuse-albedo-demodulated
counterpart of E3's specular signal.

**The trap:** `ShouldRenderRayTracingGlobalIllumination` returns false when
`GetRayTracingGlobalIlluminationSamplesPerPixel(View) <= 0` **before** it reaches
`ShouldRenderRayTracingEffect` (D2 §3). So `r.RayTracing.GlobalIllumination=1` alone does
nothing, and neither does `r.RayTracing.ForceAllRayTracingEffects=1`. The
`SamplesPerPixel=2` line is load-bearing, not decoration.

| outcome | means |
|---|---|
| `GlobalIlluminationRGS` appears | RTGI forceable; the diffuse guide is available in principle. |
| Absent, canary having fired in E2 | RTGI cannot be made to run. Combined with an E2 reflections negative, this is the **DEAD** verdict. |

**Informs DLSS-SR:** no.

---

## 5. E5 — control: all RT off · [`ini/E5-control-all-rt-off.ini`](ini/E5-control-all-rt-off.ini)

Tiebreaker only, when E2's canary did not fire and you cannot tell a dead channel from a
blocked effect. Full reasoning is in the fragment's header comment. Also the cheapest way
to learn what ray tracing currently costs Stray: compare frame time against E1.

---

## 6. Editing `Engine.ini` — exact commands

**The game rewrites this directory on exit.** Every other `.ini` in it was rewritten at
`Aug 30 00:59`; `Engine.ini` alone still carries its `Aug 29 16:30` mtime, because it is
`-r--r--r--` [HW]. That is direct proof the `chmod 444` guard works — and equally that it
is mandatory.

**Rules:** game fully closed before touching it. `chmod 444` after. Never leave it 644.

Set once per shell:

```sh
BOX='ssh root@proxmox.lan pct exec 113 -- bash -lc'
CFG='/home/deck/.local/share/Steam/steamapps/compatdata/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor'
```

(The `GamesLinux` compatdata directory is a decoy; the live prefix is this one.)

### Back up and unlock

```sh
$BOX "cp -a $CFG/Engine.ini $CFG/Engine.ini.bak-\$(date +%s) && chmod 644 $CFG/Engine.ini && ls -la $CFG/Engine.ini*"
```

### Install a fragment

The `[Core.System]` block at the top of the live file is the engine's own content-path
list and **must be preserved** — only the `[SystemSettings]` section is replaced. Do it
by keeping everything above `[SystemSettings]` and appending the fragment:

```sh
$BOX "sed '/^\[SystemSettings\]/,\$d' $CFG/Engine.ini > /tmp/eng.head"
cat rr-experiments/ini/E2-force-reflections.ini | \
  ssh root@proxmox.lan "pct exec 113 -- bash -lc 'cat > /tmp/eng.tail'"
$BOX "cat /tmp/eng.head /tmp/eng.tail > $CFG/Engine.ini && chown deck:deck $CFG/Engine.ini && chmod 444 $CFG/Engine.ini"
```

### Verify before launching — do not skip this

```sh
$BOX "ls -la $CFG/Engine.ini && echo '--- [SystemSettings] ---' && sed -n '/^\[SystemSettings\]/,\$p' $CFG/Engine.ini | grep -v '^;' | grep -v '^\$'"
```

Mode must read `-r--r--r--`. The CVar lines must be exactly the ones you intended.
A fragment that did not land is the second most likely way to get a wrong answer here.

### Revert

```sh
$BOX "chmod 644 $CFG/Engine.ini"
sed -n '/^\[SystemSettings\]/,$p' rr-experiments/ini/E1-baseline.ini | \
  ssh root@proxmox.lan "pct exec 113 -- bash -lc 'cat > /tmp/eng.tail'"
$BOX "cat /tmp/eng.head /tmp/eng.tail > $CFG/Engine.ini && chown deck:deck $CFG/Engine.ini && chmod 444 $CFG/Engine.ini"
```

### Launch options

Current, read from `localconfig.vdf` [HW]:

```
PROTON_LOG=1 VKD3D_DEBUG=warn %command% -dx12 -log -ExecCmds="r.RayTracing.Reflections, r.RayTracing.GlobalIllumination, r.RayTracing.Shadows, r.RayTracing.AmbientOcclusion, r.Reflections.Denoiser, r.Shadow.Denoiser"
```

For the census launches, use:

```
PROTON_LOG=1 VKD3D_DEBUG=warn VKD3D_SHADER_DUMP_PATH=Z:/home/deck/rtdump %command% -dx12 -log
```

* **Change this through the Steam UI, not by editing `localconfig.vdf`** — Steam
  rewrites that file from memory and will clobber a live edit.
* The `-ExecCmds` block is dropped because it is dead weight: those are bare *queries*
  with no `=value`, and their output goes to a log that a Shipping build compiles out
  (`USE_LOGGING_IN_SHIPPING=0`, D2 §6). They could never have done anything visible. This
  is a more direct explanation of "the command line does not work" than D2's ReadOnly
  argument, which is about *setting* rather than querying — both are true, this one is
  what is actually on the line.
* `VKD3D_DEBUG=warn` is left alone: vkd3d's `INFO` is emitted at `warn` anyway —
  `info:vkd3d-proton:d3d12_device_determine_ray_tracing_tier: DXR 1.1 support enabled`
  appears in the current log with `warn` set [HW]. So the "Dumping blob to …" lines will
  be logged without changing anything. (D1's note that vkd3d "is already logging at
  `info`" describes the *emitted* level, not `VKD3D_DEBUG`, which is `warn`.)
* Create the dump directory first, and **empty it between every experiment**:
  ```sh
  $BOX "rm -rf /home/deck/rtdump && mkdir -p /home/deck/rtdump && df -h /home | tail -1"
  ```
* **Path form.** vkd3d-proton's `d3d12.dll` is a PE running under Wine, so its `fopen`
  follows Wine path rules and a bare `/home/deck/rtdump` resolves against the prefix's
  drive, not the real filesystem. `Z:/home/deck/rtdump` is unambiguous (Wine maps `Z:` to
  `/`) and needs no backslash escaping. If it is wrong you will know immediately and
  exactly: `vkd3d_shader_dump_blob` logs `INFO("Dumping blob to %s.\n", filename)`
  **before** it opens the file [SRC vkd3d_shader_main.c:38-40], so the Proton log shows
  the resolved path even when the write fails. Compare it against what is on disk.

---

## 7. Verification greps

`./verify.sh` runs all of these read-only over the `pct exec` route. Manually:

```sh
DUMP=/home/deck/rtdump
LOG=/home/deck/steam-1332010.log

# (a) Did the dump channel work at all?  Positive control for everything else.
$BOX "ls $DUMP | wc -l; ls $DUMP/*.lib.*.spv 2>/dev/null | wc -l"

# (b) THE CENSUS.  Ray generation / miss / hit-group exports, by name and count.
$BOX "ls $DUMP 2>/dev/null | sed -n 's/^[0-9a-f]*\.lib\.\(.*\)\.spv\$/\1/p' | sort | uniq -c | sort -rn"

# (c) Where did vkd3d think it was writing?  Resolves a wrong path form.
$BOX "grep -m5 'Dumping blob to' $LOG"

# (d) Weak corroboration only — acceleration-structure activity.
$BOX "grep -c place_acceleration_structure $LOG"
```

### Reading (b): "the CVar was ignored" vs "it applied and the effect is off"

This distinction is the whole value of the matrix. The rules:

1. **If the dump directory is empty → nothing was measured.** Not a negative result.
   Fix the path and re-run. `ls $DUMP | wc -l` returning zero always outranks every
   other reading.
2. **If `.dxbc`/`.spv` files exist but no `*.lib.*.spv` → no RT pipeline was built.**
   The dump worked; ray tracing did not run. That is a real result about RT, and it is
   the expected result for E5.
3. **If `OcclusionRGS` is present → RT pipelines are being built and dumped.** From here
   on, an absent name means that effect's `Prepare…` gate returned false — *not* that the
   measurement failed.
4. **Given (3), "did the ini apply?" is answered by the canary alone.**
   `RayTracingPrimaryRaysRGS` present in E2 and absent in E1 ⇒ `[SystemSettings]`
   reached the render thread this launch. With that established, an absent
   `RayTracingReflectionsRGS` means the engine decided not to render reflections, which
   is a fact about Stray, not about our plumbing. Without it, the same absence means
   nothing at all.
5. **Never read a reflections or GI result from a fragment lacking the
   `SortMaterials=0` lines** — see §3 below.

### The two names that will lie to you

Both are false positives, both are neutralised by lines already present in the fragments:

* `PrepareRayTracingReflectionsDeferredMaterial` adds `RayTracingReflectionsRGS`
  **without** checking `ShouldRenderRayTracingReflections`. It checks only
  `ShouldRayTracedReflectionsSortMaterials`, which is
  `(Hybrid != 0 || SortMaterials != 0)` and is **true at defaults**
  [SRC ue427rt/RayTracingReflections.cpp:414-417, :487-500; defaults at :139-145 and
  :147-154]. So at stock settings the name appears **even with reflections fully
  disabled**. `r.RayTracing.Reflections.SortMaterials=0` + `…Hybrid=0` make that function
  return early, restoring `present ⟺ enabled`.
* `PrepareRayTracingGlobalIlluminationDeferredMaterial` is worse — it has **no**
  `ShouldRender` gate at all, only `FinalGather.SortMaterials` (default 1)
  [SRC ue427rt/RayTracingGlobalIllumination.cpp:715-724, :170-176]. Same fix.

If E1's baseline dump shows `RayTracingReflectionsRGS`, note that E1 runs at stock
settings and is therefore subject to this contamination. **The clean read is the count:**
with reflections genuinely on, `PrepareRayTracingReflections` adds *two* permutations,
Gather and Shade [SRC :451-470], plus the deferred-material one — with reflections off,
only the deferred-material one exists. Different permutations are separate DXIL libraries
with separate hashes, so they are separate files sharing one export name. So in E1:
**two or more `*.lib.RayTracingReflectionsRGS.spv` files ⇒ reflections on; exactly one ⇒
contamination only, reflections off.** If E1 gives exactly one, treat it as "off" and run
E2, whose fragment removes the ambiguity outright.

---

## 8. What this matrix does *not* touch

* **`DLSS.Use.HW.Depth`** — see [`DLSS-USE-HW-DEPTH.md`](DLSS-USE-HW-DEPTH.md). It is a
  shipping-add-on defect, it is **not** an RR item, and the brief's framing of it is half
  wrong: the parameter does not exist in `nvngx_dlssnr.dll` at all, so it is not an NR
  bug — but it *is* present and live in `nvngx_dlss.dll`, which makes it a real bug for
  the DLSS-SR work in flight. That file is the actionable one.
* Anything requiring a deploy. No add-on change is proposed or needed here.
* D5's cross-pass capture problem, which is what stands between "the signal exists" and
  "RR works".

## 9. Overlap with DLSS-SR

| item | shared with SR? |
|---|---|
| `VKD3D_SHADER_DUMP_PATH` census method (§0) | **yes** — the general way to see a frame's pass inventory with no deploy |
| `r.BasePassOutputsVelocity=1` retained in every fragment | **yes** — SR needs velocity; must not be lost |
| `DLSS.Use.HW.Depth` | **yes, and it is the SR-relevant finding of this workflow** |
| every `r.RayTracing.*` line, the denoiser CVars, the census contamination rules | no — RR only |
