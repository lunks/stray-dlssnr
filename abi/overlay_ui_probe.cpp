// overlay_ui_probe.cpp - compile and LINK the whole overlay under both toolchains, GATING.
//
// WHY THIS EXISTS SEPARATELY FROM THE ADD-ON BUILD.
//   src/overlay_ui.hpp is compiled by both add-on builds already - but the mingw add-on job is
//   marked continue-on-error, because the MSVC build is the one being moved to. That means a
//   break in the overlay under mingw would be NON-GATING, and mingw is the toolchain of the
//   binary running on the user's machine today. This probe is in the gating `abi` job, so the
//   overlay has to hold under both compilers before anything merges.
//
//   Linking matters for the same reason it does in overlay_calls_probe.cpp: reshade_overlay.hpp
//   defines inlines only for entries imgui_function_table_19250 actually carries, while imgui.h
//   declares hundreds more, so a call to something the table lacks compiles clean and dies at
//   link. Built as a DLL to force that.
//
// Nothing here is ever executed.

#include "../src/overlay_ui.hpp"

extern "C" __declspec(dllexport) void overlay_ui_draw_probe(reshade::api::effect_runtime *rt)
{
    overlay_ui::draw(rt);
}

extern "C" __declspec(dllexport) bool overlay_ui_install_probe()
{
    return overlay_ui::install();
}

// The render-thread half, with exactly the signature src/stray_dlssnr.cpp's hook uses. If either
// side of that contract changes without the other, this stops compiling.
extern "C" __declspec(dllexport) bool overlay_ui_pass_probe()
{
    static cfg::config             c;
    // The per-device snapshot scratch. nr_state owns one of these; begin_pass builds into it
    // under the overlay's seqlock and only commits to `c` when the panel did not move.
    static cfg::config             scratch;
    static overlay_ui::seen_epochs seen_pass;
    static bool                    need_reset = false;
    // DLSS-SR's own Reset flag. Two out-params, not one: see begin_pass's header for why the two
    // features' Reset flags are raised together on an edge and never mirrored level-to-level.
    static bool                    sr_need_reset = false;
    static uint64_t                pending_res = 0;

    overlay_ui::seed_from_config(c, L"C:\\nowhere\\");

    const bool run = overlay_ui::begin_pass(c, scratch, seen_pass, need_reset, sr_need_reset,
                                            pending_res, true, false, true);

    overlay_ui::publish_evaluate(0u, "Success", true, 1920u, 1080u,
                                 "r16g16b16a16_float", "r16g16b16a16_float",
                                 1920u, 1080u, 1.0f, 1.0f, true, 0u, 0u);

    (void)overlay_ui::live_copy_back();
    (void)overlay_ui::live_history_restore();
    (void)overlay_ui::live_mvec_decode();
    (void)overlay_ui::live_mvec_reconstruct();
    (void)overlay_ui::live_diagnostics();
    (void)overlay_ui::live_enabled();
    (void)overlay_ui::live_hdr_codec();
    (void)overlay_ui::live_require_trampoline();
    (void)overlay_ui::live_populate_parameters();
    (void)overlay_ui::live_rt_census();
    (void)overlay_ui::live_rt_census_frames();
    // DLSS-SR's accessors, for the same reason the eleven above are here: the add-on build
    // compiles them, but the mingw add-on job is continue-on-error and mingw is the toolchain of
    // the binary on the user's machine today. This job is gating on both.
    (void)overlay_ui::live_dlss_sr();
    (void)overlay_ui::live_dlss_nr();
    (void)overlay_ui::live_sr_mvec_decode();
    (void)overlay_ui::live_sr_mvec_reconstruct();
    (void)overlay_ui::live_sr_suppress_taa();
    (void)overlay_ui::live_sr_perf_quality();
    (void)overlay_ui::live_sr_render_preset();
    (void)overlay_ui::live_sr_shader_hash();
    // want_hash() is the whole of the identification merge: it is the one function both the
    // DLSS-NR pin and the DLSS-SR re-pin go through, and it must be reachable and coherent.
    (void)overlay_ui::want_hash(overlay_ui::read_ident());
    (void)overlay_ui::dirty();
    return run;
}

// The RECONFIGURE contract, with exactly the shapes nr_service_reconfigure uses. Same argument as
// above, and it matters more here: this half runs on the PRESENT thread, so a signature drift
// between the two would otherwise only show up as a compile error inside the add-on build - which
// is continue-on-error under mingw, the toolchain shipping on hardware today.
extern "C" __declspec(dllexport) unsigned overlay_ui_reconfigure_probe()
{
    static overlay_ui::seen_epochs seen_service;

    const overlay_ui::ident_view id = overlay_ui::read_ident();

    overlay_ui::request(overlay_ui::a_teardown | overlay_ui::a_clear_failed |
                        overlay_ui::a_clear_clip | overlay_ui::a_apply_census |
                        overlay_ui::a_reconcile | overlay_ui::a_apply_populate,
                        "overlay_ui_reconfigure_probe", overlay_ui::k_rebuild);

    const overlay_ui::reconfig_request r = overlay_ui::take_reconfigure(seen_service);

    overlay_ui::publish_reconfig_pending(r.bits != 0);
    overlay_ui::publish_reconfigure(true, "overlay_ui_reconfigure_probe");
    overlay_ui::publish_populate(overlay_ui::live_populate_parameters());
    // The service seeds nr_state::seen_service with this before it does any work.
    overlay_ui::adopt_epochs(seen_service);

    return r.bits ^ id.epoch ^ (unsigned)id.shader_hash ^ (r.ident_changed ? 1u : 0u);
}

// The ini writer, so its Win32 surface (MoveFileExW, _wfopen) is type-checked on both toolchains
// too. Never called: it would write a file.
extern "C" __declspec(dllexport) bool overlay_ui_save_probe(char *out, int n)
{
    std::string err;
    const bool ok = overlay_ui::save_ini(err);
    std::snprintf(out, static_cast<size_t>(n), "%s", err.c_str());
    return ok;
}
