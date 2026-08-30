# VERDICT-CRITERIA — written before any experiment has been run

Pre-registered so that a null result cannot be re-read afterwards as "encouraging".
Nothing in this file may be edited once E1 has been launched. Record outcomes in a
separate results file.

---

## 0. The ceiling on what this matrix can prove

**No `Engine.ini` experiment can show that RR is VIABLE.** State this before reading
anything below.

RR needs a noisy, albedo-demodulated radiance texture *at a pass the add-on intercepts*.
D2 established that when RT reflections run with the engine denoiser off, UE 4.27 puts
exactly such a texture in the frame — `ReflectionsColor = DenoiserInputs.Color`
[SRC ue427rt/IndirectLightRendering.cpp:838] — but it does so **ten-plus passes upstream
of the TAA dispatch we hook**, and D2 says plainly it is "not present at t5, where we
intercept."

So this matrix has exactly two possible verdicts:

* **DEAD** — no ray-traced radiance pass can be made to run in this title at all.
  Then RR has nothing to denoise anywhere in the frame, and the idea is finished.
* **NOT DEAD** — the signal provably exists in-frame. The remaining problem is
  reaching it (D5: copy pixels at a pass that binds the G-buffer, never carry
  pointers), which requires code and a deploy and is **out of scope for every
  experiment here**.

"NOT DEAD" is not "viable". Any write-up that upgrades one to the other is wrong.

---

## 1. RR is DEAD if — and only if — all of these hold together

1. E2 ran with the `[SystemSettings]` channel **proven live** — that is, the E2 canary
   fired: `RayTracingPrimaryRaysRGS` appeared in the shader dump when
   `r.RayTracing.Translucency=1` was set, having been **absent** in E1's baseline dump.
   (Without the canary firing, a negative result is uninterpretable — see §3.)
2. With the channel proven live and `r.RayTracing.Reflections=1`,
   `r.RayTracing.Reflections.SamplesPerPixel=1`,
   `r.RayTracing.Reflections.SortMaterials=0` set, **no** file matching
   `*.lib.RayTracingReflectionsRGS.spv` and none matching
   `*.lib.RayTracingDeferredReflectionsRGS.spv` appears in the dump directory.
3. And E4 likewise produces no `*.lib.GlobalIlluminationRGS.spv` with
   `r.RayTracing.GlobalIllumination=1` and
   `r.RayTracing.GlobalIllumination.SamplesPerPixel=2` and
   `r.RayTracing.GlobalIllumination.FinalGather.SortMaterials=0`.

All three ⇒ neither of the two engine paths that produce demodulated radiance can be
made to run in Stray. **DEAD. Stop work on RR.**

A secondary, weaker death condition: reflections *do* run, but with
`r.Reflections.Denoiser=0` the on-screen reflections are visually **identical** to the
denoised case (no Monte-Carlo noise at 1 spp with `MaxRoughness=1.0`) — meaning we
cannot obtain an undenoised signal even where one nominally exists. Flag this as
**DEAD-pending-recheck** rather than DEAD; it is a subjective observable and deserves a
second look before it kills the project.

## 2. RR is NOT DEAD if both of these hold

1. `*.lib.RayTracingReflectionsRGS.spv` (or `*.lib.RayTracingDeferredReflectionsRGS.spv`)
   is present in the dump — from E1 with no ini change at all, or from E2 after forcing.
   That proves `ShouldRenderRayTracingReflections(View)` is true and the reflection ray
   generation shader is real, cooked and in a pipeline
   [SRC ue427rt/RayTracingReflections.cpp:434 — `PrepareRayTracingReflections` returns
   early otherwise].
2. E3 produces **visible Monte-Carlo noise** in reflective surfaces — wet street, neon,
   glass — that is absent with `r.Reflections.Denoiser=2`. That is the raw
   specular-albedo-demodulated radiance reaching the screen, i.e. the exact signal class
   RR consumes, confirmed on hardware rather than inferred from source.

Both ⇒ **NOT DEAD.** The next question is D5 plumbing, and it is a code question.

## 3. What makes a result UNINTERPRETABLE (neither verdict)

Declare this outcome loudly rather than picking a side:

* The dump directory is **empty or absent** ⇒ the measurement channel failed, not the
  experiment. Nothing about RT was learned. (See README §5 for the path fix.)
* The dump contains thousands of `.dxbc`/`.spv` files but **zero** `*.lib.*.spv` and
  **no** `OcclusionRGS` ⇒ no RT pipeline was created at all this session. Either the
  session never reached a scene with RT, or something changed under us. Re-run E1.
* E2's canary did **not** fire ⇒ the `[SystemSettings]` channel did not deliver. Every
  E2/E3/E4 negative is void. Do not conclude anything about RT. Run E5.
* `OcclusionRGS` is absent while other RT names are present, or vice-versa in a way the
  README's decision table does not cover ⇒ stop and re-derive; the model is wrong.

## 4. The specific traps that would manufacture a false positive

Pre-registered because each one produces a name in the dump that does **not** mean the
effect is enabled:

* `PrepareRayTracingReflectionsDeferredMaterial` does **not** gate on
  `ShouldRenderRayTracingReflections` — it gates only on
  `ShouldRayTracedReflectionsSortMaterials`, which is true by default
  [SRC ue427rt/RayTracingReflections.cpp:487-500, and `r.RayTracing.Reflections.SortMaterials`
  default 1 at :139-145]. So **`RayTracingReflectionsRGS` can appear in the dump with
  reflections fully disabled.** Every experiment that reads this name therefore sets
  `r.RayTracing.Reflections.SortMaterials=0` *and* `r.RayTracing.Reflections.Hybrid=0`,
  which makes that path return early. If those two lines are missing from the fragment
  that was actually used, the reading is void.
* `PrepareRayTracingGlobalIlluminationDeferredMaterial` has the same defect and no
  `ShouldRenderRayTracingGlobalIllumination` gate at all
  [SRC ue427rt/RayTracingGlobalIllumination.cpp:715-724, `FinalGather.SortMaterials`
  default 1 at :170-176]. Same mitigation, same voiding rule.
* Stale files. `vkd3d_shader_dump_blob` opens with `"wbx"` — **exclusive create**
  [SRC vkd3dsrc/libs/vkd3d-shader/vkd3d_shader_main.c:43]. A file left over from the
  previous experiment is never overwritten and reads exactly like a fresh positive.
  **A result from a dump directory that was not emptied first is void.**

## 5. Things this matrix deliberately does not test

Recording these so their absence is not later mistaken for a negative result:

* Whether any `DispatchRays` actually executes. The dump proves a *pipeline* was built
  from a raygen shader, which proves the effect's `Prepare…` self-gate passed — it does
  not prove a dispatch. For reflections and AO the gate and the dispatch share the same
  predicate, so the gap is small, but it is real and it is [ASSUMED] here.
* Whether the reflection texture can be reached from our interception point (D5).
* Whether RR's network tolerates Stray's content at all.
