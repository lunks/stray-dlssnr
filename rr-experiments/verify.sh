#!/usr/bin/env bash
# Read-only verification for the Stray RR experiment matrix.
#
# Makes NO writes to the box.  Every command is ls / cat / grep / wc.
# Safe to run while the game is running.
#
#   ./verify.sh            full report
#   ./verify.sh census     just the raygen census
#   ./verify.sh config     just the live Engine.ini / launch options
set -uo pipefail

BOX_HOST="${BOX_HOST:-root@proxmox.lan}"
CT="${CT:-113}"
DUMP="${DUMP:-/home/deck/rtdump}"
LOG="${LOG:-/home/deck/steam-1332010.log}"
CFG="${CFG:-/home/deck/.local/share/Steam/steamapps/compatdata/1332010/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Config/WindowsNoEditor}"

box() { ssh -o BatchMode=yes "$BOX_HOST" "pct exec $CT -- bash -lc \"$1\""; }
hdr() { printf '\n=== %s ===\n' "$1"; }

do_config() {
    hdr "Engine.ini  (mode MUST be -r--r--r--)"
    box "ls -la $CFG/Engine.ini"
    hdr "Engine.ini  [SystemSettings]  (this is what the game will actually read)"
    box "sed -n '/^\\[SystemSettings\\]/,\\\$p' $CFG/Engine.ini | grep -v '^;' | grep -v '^\\\$'"
    hdr "GameUserSettings.ini  [RayTracing]  (must be ABSENT, or RT is off)"
    box "grep -c '^\\[RayTracing\\]' $CFG/GameUserSettings.ini || true"
    hdr "Steam launch options"
    box "grep -A20 '\\\"1332010\\\"' /home/deck/.local/share/Steam/userdata/*/config/localconfig.vdf | grep -m1 LaunchOptions"
}

do_census() {
    hdr "Dump channel: total files / DXR export files"
    box "printf 'total   %s\\nlib.spv %s\\n' \\\"\\\$(ls $DUMP 2>/dev/null | wc -l)\\\" \\\"\\\$(ls $DUMP/*.lib.*.spv 2>/dev/null | wc -l)\\\""
    echo "  total 0        -> the dump directory is empty or absent: either the census"
    echo "                    launch has not happened yet, or the path form is wrong."
    echo "                    Either way this is NOT a negative result about ray tracing."
    echo "  total >0, lib 0-> dump works, no RT pipeline was built this session."

    hdr "THE CENSUS: raygen / miss / hit-group exports, by name"
    box "ls $DUMP 2>/dev/null | sed -n 's/^[0-9a-f]*\\.lib\\.\\(.*\\)\\.spv\\\$/\\1/p' | sort | uniq -c | sort -rn"
    cat <<'NOTE'
  Reading it:
    OcclusionRGS                     -> RT shadows.  POSITIVE CONTROL: expect it always.
    AmbientOcclusionRGS              -> RT ambient occlusion.  Clean binary marker.
    RayTracingReflectionsRGS         -> RT reflections.  BUT see the count rule below.
    RayTracingDeferredReflectionsRGS -> RT reflections, experimental deferred path.
    GlobalIlluminationRGS            -> RTGI.  Contaminated unless FinalGather.SortMaterials=0.
    RayTracingPrimaryRaysRGS         -> RT translucency == THE E2 CHANNEL CANARY.
    SkyLightRGS                      -> RT sky light (expect absent; default off).

  COUNT RULE, for a run WITHOUT r.RayTracing.Reflections.SortMaterials=0 (i.e. E1):
    2+ RayTracingReflectionsRGS files -> reflections genuinely ON  (Gather + Shade)
    exactly 1                         -> deferred-material contamination only; OFF
    With SortMaterials=0 + Hybrid=0 set, presence alone is decisive.
NOTE

    hdr "Where vkd3d thought it was writing (resolves a wrong path form)"
    box "grep -m3 'Dumping blob to' $LOG || echo '  (no dump lines - VKD3D_SHADER_DUMP_PATH not set this launch)'"

    hdr "Weak corroboration: acceleration-structure VA-mutation warnings"
    box "grep -c place_acceleration_structure $LOG || true"
    echo "  Probabilistic, not deterministic: this WARN fires only on VA reuse across"
    echo "  TLAS/non-TLAS, not on every build.  Zero is suggestive, never conclusive."

    hdr "Disk headroom for the dump"
    box "df -h /home | tail -1; du -sh $DUMP 2>/dev/null || true"
}

case "${1:-all}" in
    census) do_census ;;
    config) do_config ;;
    all)    do_config; do_census ;;
    *)      echo "usage: $0 [all|census|config]" >&2; exit 2 ;;
esac
