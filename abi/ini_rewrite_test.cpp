// ini_rewrite_test.cpp - RUN the overlay's stray_dlssnr.ini writer for real, under both
// toolchains, and check what it did to the file.
//
// WHY THIS IS A TEST AND NOT JUST A COMPILE.
//   Everything else the overlay does is either measured by the ABI probes or is a widget call.
//   The ini writer is the one part that touches the USER'S data, and its failure mode is silent
//   and permanent: a rewrite that drops a comment block destroys the documentation the user
//   actually reads, and a rewrite that truncates makes every key after the cut take its built-in
//   default (addon_config.hpp:219-223) - which is worse than having no ini at all. That has to be
//   exercised, not argued.
//
//   It also has to be exercised on WINDOWS, because the writer is Win32: _wfopen, and
//   MoveFileExW(REPLACE_EXISTING) for the temp-file swap. A macOS or Linux stand-in would prove
//   nothing about the part most likely to go wrong.
//
// Exits non-zero on the first failed check. Prints one line per check either way.

#include "../src/overlay_ui.hpp"

#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

static std::wstring make_temp_dir()
{
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    wchar_t name[MAX_PATH] = {};
    std::swprintf(name, MAX_PATH, L"%sdlssnr_ini_%lu\\", base, (unsigned long)GetTickCount64());
    CreateDirectoryW(name, nullptr);
    return std::wstring(name);
}

static bool write_file(const std::wstring &path, const std::string &body)
{
    FILE *f = _wfopen(path.c_str(), L"wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    return ok;
}

static std::string read_file(const std::wstring &path)
{
    std::string out;
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr)
        return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

static bool has_line(const std::string &hay, const std::string &needle)
{
    // Exact line match, so "intensity = 1.5" does not satisfy a check for "intensity = 1.55".
    size_t start = 0;
    while (start <= hay.size())
    {
        const size_t nl = hay.find('\n', start);
        std::string line = (nl == std::string::npos) ? hay.substr(start) : hay.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == needle)
            return true;
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return false;
}

// A fixture with every hazard the shipped ini actually has: a header comment block, a section
// header, CRLF endings, column-aligned keys, a key we do NOT own, a trailing comment on a value
// line, the British spelling alias, and one owned key that is absent entirely.
//
// WHAT THE OVERLAY OWNS CHANGED WITH THE RECONFIGURE LADDER, and this fixture changed with it.
// shader_hash, srv_*, enabled, hdr_codec and the rest used to be checked here as keys the writer
// must NEVER round-trip - which was the right test while they were load-only, because clobbering
// a hand-measured identification pin from a panel that could not change it would only ever be a
// bug. They are live controls now, so the writer owns them, and the property worth testing is the
// opposite one: a pin the user CHANGED must reach the file, and a pin they did not change must
// come back byte-identical. app_id is the one setting that is still not a control at all (see
// overlay_ui.hpp's ladder: NGX cannot be re-inited in-process with any evidence that it survives
// a second Init_Ext), so it takes over as the key that proves the not-owned path still works.
static const char *const kFixture =
    "; stray_dlssnr.ini - hand-written, do not reflow.\r\n"
    ";\r\n"
    "; A second comment line, indented oddly:\r\n"
    ";      keep me exactly as I am.\r\n"
    "\r\n"
    "[stray_dlssnr]\r\n"
    "\r\n"
    "; The identification pins. Live controls now, so the overlay owns them.\r\n"
    "shader_hash = 0x1708ec956099e259\r\n"
    "srv_velocity = 2\r\n"
    "enabled = 1\r\n"
    "hdr_codec = 1\r\n"
    "\r\n"
    "; NOT owned by the overlay, in either direction. It must come back byte for byte.\r\n"
    "app_id = 0x24480451\r\n"
    "\r\n"
    "copy_back = 1   ; a trailing comment on an owned key\r\n"
    "history_restore = 1\r\n"
    "restore_graphics_root = 1\r\n"
    "depth_inverted = 1\r\n"
    "colour_strength = 1.0\r\n"
    "paper_white_scale = 1.0\r\n"
    "transfer_strength = 1.0\r\n"
    "mvec_scale_x = 0\r\n"
    "mvec_scale_y = 0\r\n"
    "intensity                = 1.0\r\n"
    "local_tone_strength      = 1.0\r\n"
    "local_structure_strength = 1.0\r\n"
    "skin_structure_strength  = -1.0\r\n"
    "style = 0\r\n"
    "use_auto_mask = 1\r\n"
    "\r\n"
    "; ui_correction and hdr_graft are deliberately ABSENT, so the append path is exercised too.\r\n"
    "totally_unknown_key = 42\r\n"
    "\r\n"
    "; DLSS-SR. dlss_sr and sr_perf_quality are PRESENT so the in-place rewrite path is exercised\r\n"
    "; for the new keys; sr_shader_hash and sr_suppress_taa are deliberately ABSENT so the append\r\n"
    "; path is exercised for them too. dlss_nr is present and UNCHANGED - it is the launch-time key,\r\n"
    "; and a key that only takes effect next launch is exactly the one a dropped Save would lose\r\n"
    "; without any symptom until the relaunch.\r\n"
    "; dlss_chain and the DLSS-SR create flags are ABSENT for the same reason hdr_graft is: they\r\n"
    "; arrived with the chain/graft work and the append path is the one that would silently drop\r\n"
    "; them.\r\n"
    "dlss_sr = 0\r\n"
    "dlss_nr = 1\r\n"
    "sr_perf_quality  = 0\r\n";

int main()
{
    // UNBUFFERED. If a check crashes the process, a buffered stdout takes every line printed so
    // far with it and CI shows an empty step - which is exactly what happened the first time this
    // ran, and it cost a round trip to work out why.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::wstring dir  = make_temp_dir();
    const std::wstring path = dir + L"stray_dlssnr.ini";

    std::printf("temp dir written to by the test\n");

    // ---------------------------------------------------------------- case A: rewrite in place
    std::printf("\ncase A - rewrite an existing, commented, CRLF, column-aligned ini\n");
    if (!write_file(path, kFixture))
    {
        std::printf("  FAIL  could not write the fixture\n");
        return 1;
    }

    cfg::config c;   // built-in defaults
    overlay_ui::seed_from_config(c, dir);

    overlay_ui::live_block &l = overlay_ui::live();
    // 0.25, not 1.25: intensity lives on [0,1] and the loader clamps above 1.0 - see the
    // tuning-knob note in addon_config.hpp. Same width, so the alignment check below is
    // unaffected. The clamp itself is asserted separately at the end of the reparse block.
    l.intensity.store(0.25f, std::memory_order_relaxed);
    l.local_structure_strength.store(0.5f, std::memory_order_relaxed);
    l.skin_structure_strength.store(-1.0f, std::memory_order_relaxed);
    l.color_strength.store(0.75f, std::memory_order_relaxed);
    l.copy_back.store(false, std::memory_order_relaxed);
    l.style.store(2u, std::memory_order_relaxed);
    l.mvec_scale_x.store(1.5f, std::memory_order_relaxed);
    l.ui_correction.store(1u, std::memory_order_relaxed);
    // The reconfigure ladder's keys: one identification pin the user CHANGED, and one they did
    // not. Both have to survive correctly, and for opposite reasons.
    l.srv_velocity.store(3u, std::memory_order_relaxed);
    l.hdr_codec.store(false, std::memory_order_relaxed);
    // ---- DLSS-SR. Four shapes in one go: a bool rewritten in place, a uint32 rewritten in place
    // WITH column alignment, a 64-bit hex pin appended, and a bool appended. If any of these is in
    // the live_block and the widget list but missing from OVERLAY_OWNED_FIELDS, owned_value() or
    // owned_keys(), exactly one of the checks below fails and names it - which is the whole point:
    // a key the panel can change and Save silently drops is a control that lies, just more slowly.
    l.dlss_sr.store(true, std::memory_order_relaxed);
    l.sr_perf_quality.store(5u, std::memory_order_relaxed);
    l.sr_shader_hash.store(0x901e041a7cadc9dbull, std::memory_order_relaxed);
    l.sr_suppress_taa.store(true, std::memory_order_relaxed);
    // The graft selector. Absent from the fixture, so this exercises the append path AND proves
    // the newest owned key is actually owned - a key the writer forgets is a setting that silently
    // reverts to its default on the next launch while the panel still shows the user's choice.
    l.hdr_graft.store(1u, std::memory_order_relaxed);
    // ---- CHAIN MODE AND THE REST OF DLSS-SR, all of which arrived with the chain/graft merge and
    // every one of which is appended rather than rewritten. One of each remaining SHAPE is set to
    // a non-default: a bool, a float and a uint32. The bug this catches is the merge's own most
    // likely one - a key added to live_block and to a widget, and forgotten in one of the three
    // places Save reads (OVERLAY_OWNED_FIELDS, owned_value, owned_keys).
    l.dlss_chain.store(true, std::memory_order_relaxed);
    l.sr_hdr.store(false, std::memory_order_relaxed);
    l.sr_jitter_scale_x.store(-1.0f, std::memory_order_relaxed);
    l.sr_group_tile.store(16u, std::memory_order_relaxed);
    l.sr_out_width.store(3840u, std::memory_order_relaxed);

    check(overlay_ui::dirty(), "dirty() reports the pending edits");

    std::string err;
    const bool saved = overlay_ui::save_ini(err);
    if (!saved)
        std::printf("  save_ini error: %s\n", err.c_str());
    check(saved, "save_ini succeeded");
    check(!overlay_ui::dirty(), "dirty() is clear after a successful save");

    const std::string out = read_file(path);
    check(!out.empty(), "the rewritten file is not empty");

    // --- nothing we do not own may change ---
    check(has_line(out, "; stray_dlssnr.ini - hand-written, do not reflow."), "the header comment survived");
    check(has_line(out, ";      keep me exactly as I am."), "an oddly indented comment survived byte for byte");
    check(has_line(out, "[stray_dlssnr]"), "the section header survived");
    check(has_line(out, "app_id = 0x24480451"), "app_id (not an overlay control) was NOT round-tripped");
    check(has_line(out, "totally_unknown_key = 42"), "an unrecognised key survived untouched");
    check(has_line(out, "; ui_correction and hdr_graft are deliberately ABSENT, so the append path is exercised too."),
          "the comment above the absent keys survived");

    // --- what we do own must change, without reflowing ---
    check(has_line(out, "intensity                = 0.25"), "intensity took the new value AND kept its column alignment");
    check(has_line(out, "local_structure_strength = 0.5"), "local_structure_strength kept its alignment");
    check(has_line(out, "skin_structure_strength  = -1"), "a negative sentinel round-trips as -1");
    check(has_line(out, "style = 2"), "style took the new value");
    check(has_line(out, "mvec_scale_x = 1.5"), "mvec_scale_x took the new value");
    check(has_line(out, "copy_back = 0   ; a trailing comment on an owned key"),
          "copy_back changed and its trailing comment survived, spacing included");
    check(has_line(out, "colour_strength = 0.75"),
          "the British spelling was updated in place rather than duplicated");
    check(out.find("color_strength = ") == std::string::npos,
          "no duplicate American-spelling key was appended alongside it");
    check(has_line(out, "ui_correction = 1"), "the absent owned key was appended");
    check(has_line(out, "hdr_graft = 1"), "hdr_graft was appended");

    // --- the reconfigure ladder's keys ---
    check(has_line(out, "srv_velocity = 3"), "a CHANGED identification pin reached the file");
    check(has_line(out, "shader_hash = 0x1708ec956099e259"),
          "an UNCHANGED shader hash came back byte for byte, in hex, not as a decimal");
    check(has_line(out, "hdr_codec = 0"), "hdr_codec, now a live control, took the new value");
    check(has_line(out, "enabled = 1"), "enabled, now a live control, kept its value");
    check(has_line(out, "srv_colour = 5"), "an owned key absent from the file was appended");
    check(has_line(out, "diagnostics = 1"), "diagnostics was appended");
    check(out.find("srv_color = ") == std::string::npos,
          "no duplicate American spelling of srv_colour was appended");

    // --- DLSS-SR's keys ---
    check(has_line(out, "dlss_sr = 1"), "dlss_sr took the new value in place");
    check(has_line(out, "dlss_nr = 1"), "dlss_nr, the launch-time key, came back unchanged");
    check(has_line(out, "sr_perf_quality  = 5"),
          "sr_perf_quality took the new value AND kept its column alignment");
    check(has_line(out, "sr_shader_hash = 0x901e041a7cadc9db"),
          "the appended sr_shader_hash is hex, zero-padded, like shader_hash");
    check(has_line(out, "sr_suppress_taa = 1"), "the appended sr_suppress_taa took the new value");
    check(has_line(out, "sr_mvec_decode = 1"), "an untouched DLSS-SR default was appended");
    check(has_line(out, "dlss_chain = 1"), "the appended dlss_chain took the new value");
    check(has_line(out, "sr_hdr = 0"), "an appended DLSS-SR create flag took the new value");
    check(has_line(out, "sr_jitter_scale_x = -1"), "an appended DLSS-SR float round-trips as -1");
    check(has_line(out, "sr_group_tile = 16"), "an appended DLSS-SR uint32 took the new value");
    check(has_line(out, "sr_out_width = 3840"), "the appended output-width pin took the new value");
    check(has_line(out, "sr_copy_back = 1"), "an untouched DLSS-SR default was appended");
    check(has_line(out, "sr_optimal_settings = 0"), "the arm-time DLSS-SR key was appended too");

    check(out.find("\r\n") != std::string::npos, "CRLF line endings survived on the lines we rewrote");

    // --- and it must still parse back to exactly what we set ---
    {
        cfg::config back;
        cfg::load(back, dir, [](const char *) {});
        check(back.intensity == 0.25f,                "reparse: intensity");
        check(back.local_structure_strength == 0.5f,  "reparse: local_structure_strength");
        check(back.skin_structure_strength == -1.0f,  "reparse: skin_structure_strength sentinel");
        check(back.color_strength == 0.75f,           "reparse: colour_strength alias");
        check(back.copy_back == false,                "reparse: copy_back");
        check(back.style == 2u,                       "reparse: style");
        check(back.mvec_scale_x == 1.5f,              "reparse: mvec_scale_x");
        check(back.ui_correction == 1u,               "reparse: ui_correction");
        check(back.hdr_graft == 1u,                   "reparse: hdr_graft");

        // THE LOADER MUST NOT REWRITE THE TUNING VALUES, on the path that actually matters: a
        // hand-edited ini, or one written by an older build that offered 0..2 sliders.
        //
        // AN EARLIER REVISION CLAMPED ALL FOUR HERE and this test asserted the clamp. Both were
        // wrong. Intensity 2.0 and LocalTone 1.55 drive exactly the same snippet code as 1.0
        // (tools/ngx_paramblock_selftest.cpp section 10 replays both transforms), so clamping
        // them changed no behaviour and only rewrote the user's file. The two STRUCTURE strengths
        // were worse: the snippet does not clamp them anywhere - 0x180061710 is a pure setter -
        // so clamping on load removed values the binary is willing to accept, which is the one
        // thing a range fix must never do. The sliders carry the conventional [0,1] domain; the
        // ini is the unclamped escape hatch, and this is the test that keeps it one.
        {
            // A DETERMINISTIC subdirectory, deliberately not a second make_temp_dir(): that
            // helper names the directory from GetTickCount64(), whose resolution is ~15ms, so a
            // second call here would almost certainly hand back case A's directory - and this
            // fixture would then be sitting in it when case B asserts that no ini exists yet.
            const std::wstring sdir = dir + L"stale\\";
            CreateDirectoryW(sdir.c_str(), nullptr);
            const char *stale =
                "intensity                = 2.0\r\n"
                "local_tone_strength      = 1.55\r\n"
                "local_structure_strength = 1.93\r\n"
                "skin_structure_strength  = -0.5\r\n";
            if (!write_file(sdir + L"stray_dlssnr.ini", stale))
            {
                std::printf("  FAIL  could not write the out-of-range fixture\n");
                return 1;
            }
            cfg::config st;
            cfg::load(st, sdir, [](const char *) {});
            check(st.intensity == 2.0f,                "load: intensity = 2.00 survives verbatim");
            check(st.local_tone_strength == 1.55f,     "load: local_tone_strength = 1.55 survives verbatim");
            check(st.local_structure_strength == 1.93f,"load: local_structure_strength = 1.93 is NOT clamped");
            check(st.skin_structure_strength == -0.5f, "load: a negative skin strength keeps its exact value");
        }
        // The pins must be exactly what the fixture said, not what the overlay's defaults are.
        check(back.shader_hash == 0x1708ec956099e259ull, "reparse: shader_hash round-tripped exactly");
        check(back.srv_velocity == 3u,                   "reparse: the changed srv_velocity pin");
        check(back.hdr_codec == false,                   "reparse: hdr_codec");
        check(back.enabled == true,                      "reparse: enabled");
        check(back.srv_colour == 5u,                     "reparse: the appended srv_colour");
        check(back.app_id == 0x24480451ull,              "reparse: app_id is untouched");
        // The whole DLSS-SR round trip, through the real parser in addon_config.hpp.
        check(back.dlss_sr == true,                      "reparse: dlss_sr");
        check(back.dlss_nr == true,                      "reparse: dlss_nr");
        check(back.sr_perf_quality == 5u,                "reparse: sr_perf_quality");
        check(back.sr_shader_hash == 0x901e041a7cadc9dbull, "reparse: sr_shader_hash in hex");
        check(back.sr_suppress_taa == true,              "reparse: sr_suppress_taa");
        check(back.sr_mvec_decode == true,               "reparse: the appended sr_mvec_decode");
        check(back.dlss_chain == true,                   "reparse: dlss_chain");
        check(back.sr_hdr == false,                      "reparse: the sr_hdr create flag");
        check(back.sr_jitter_scale_x == -1.0f,           "reparse: sr_jitter_scale_x sign A/B");
        check(back.sr_group_tile == 16u,                 "reparse: sr_group_tile");
        check(back.sr_out_width == 3840u,                "reparse: sr_out_width");
        check(back.sr_copy_back == true,                 "reparse: the appended sr_copy_back default");
        check(back.sr_optimal_settings == false,         "reparse: the appended sr_optimal_settings");
    }

    // ---------------------------------------------------------------- case B: no file at all
    std::printf("\ncase B - no stray_dlssnr.ini exists yet\n");
    DeleteFileW(path.c_str());
    l.transfer_strength.store(0.0f, std::memory_order_relaxed);
    overlay_ui::bump(overlay_ui::k_plain);

    std::string err2;
    const bool saved2 = overlay_ui::save_ini(err2);
    if (!saved2)
        std::printf("  save_ini error: %s\n", err2.c_str());
    check(saved2, "save_ini created a new file");
    {
        cfg::config back;
        cfg::load(back, dir, [](const char *) {});
        check(back.ini_found,                     "the created file is found by the parser");
        check(back.transfer_strength == 0.0f,     "reparse: transfer_strength = 0 (the exact-bypass value)");
        check(back.intensity == 0.25f,            "reparse: intensity survived into the new file");
        check(back.style == 2u,                   "reparse: style survived into the new file");
        check(back.hdr_graft == 1u,               "reparse: hdr_graft survived into the new file");
    }

    // ---------------------------------------------------------------- case C: unwritable target
    std::printf("\ncase C - the ini cannot be written\n");
    {
        overlay_ui::ini_dir() = L"Z:\\definitely\\not\\a\\real\\path\\";
        std::string err3;
        const bool saved3 = overlay_ui::save_ini(err3);
        check(!saved3, "save_ini reports failure rather than claiming success");
        check(!err3.empty(), "save_ini fills in a reason the UI can show");
        if (!err3.empty())
            std::printf("        reason: %s\n", err3.c_str());
    }

    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
