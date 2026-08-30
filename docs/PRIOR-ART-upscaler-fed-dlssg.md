# Prior art: upscaler-fed DLSSG working on Proton — OptiScaler issue #1089

**This is the closest existing analogue to what we are proposing, and it works.**
`https://github.com/optiscaler/OptiScaler/issues/1089` — open, 2026-08-03.

## Why it matters

Someone ran **`FGInput = upscaler` + DLSSG on Linux/Proton, native D3D12**, for 84 minutes with no
freeze, ghosting, artefact, crash or GPU reset. That retires the biggest unmeasured risk in our FG
plan — *"does Streamline/DLSS-G initialise at all under Proton/vkd3d?"* — with a positive result
from a real session rather than reasoning.

Their upscaler was **FSR 4.1.1** fed through OptiScaler's upscaler input, with DLSS Enabler /
Streamline providing the DLSSG backend, on an **RX 9070 XT**. So the harvest path does not care
which upscaler produces the dispatch — which is precisely the property our DLSS-SR would rely on.

## Their working configuration

```ini
FGEnabled = true
FGInput   = upscaler
FGOutput  = DLSSGWithNvngx      ; NOTE: not plain "dlssg"
[DLSSG]
InterpolationCount = 2          ; 3x output. We would use 2x.
[OptiFG]
HUDFix = true
DisableHUDFix = false
```

`FGOutput = DLSSGWithNvngx` is the value to start from — the bridge research had assumed plain
`dlssg`. Worth confirming which of the two our OptiScaler build exposes.

## The bug they hit, and why it is OUR bug too

> *"The upscaler-fed path previously called `EvaluateState()` before capturing the current NGX reset
> flag and depth/motion-vector inputs. That allowed DLSSG activation and OptiScaler's per-frame
> resource tracking to cross a resource transition without first observing the new input boundary."*

Symptoms before the fix: repeatable 3-5 second freezes triggered by a HUD panel, accumulating
ghosting, then full-frame artefacts after a scene transition. Disabling FG removed them instantly
while the upscaled render stayed clean — i.e. **the upscaler was fine and FG was the culprit**, a
diagnosis shape worth remembering.

**Directly relevant to us:** the defect is about *when the NGX reset flag is captured relative to
resource transitions*. We have just added camera-cut Reset detection to DLSS-NR
(`TemporalAAJitter.zw == .xy`), and DLSS-SR sets `DLSS.Reset` on the same kind of boundary. If we
feed SR to DLSSG, our reset timing becomes part of this exact interaction.

## Actions

1. Check whether the #1089 patch is merged into the OptiScaler build we would deploy; if not, build
   from that branch or apply it.
2. Start from `FGOutput = DLSSGWithNvngx`, `InterpolationCount = 1` (2x — the user has confirmed 2x
   is all we want, which also sidesteps flip metering entirely; see below).
3. `HUDFix = true`. Note our add-on writes at the TAA pass, upstream of UE4's post chain and UI, so
   what OptiScaler considers "HUD-less" needs checking against that.

## Flip metering — reassessed, NOT a blocker

`NvAPI_D3D12_SetFlipConfig` is genuinely absent from dxvk-nvapi (header and name->id table present,
no implementation in `src/`), and there are **zero** issues or PRs about it upstream. But it is
`\since Release: 570` (DLSS 4 / Blackwell era) and its parameter is `nFramesPerBatch` — it batches
frames so the display engine can space **3x/4x** MFG flips evenly, with `0 = flip metering disabled`
an explicitly supported value. **At 2x there is nothing to meter.** Ada's 2x FG predates the API and
has always paced in software. The earlier "pacing will be the likely disappointment" conclusion was
overstated; `NvAPI_D3D12_NotifyOutOfBandCommandQueue`, which Streamline actually uses for pacing,
**is** implemented in dxvk-nvapi.
