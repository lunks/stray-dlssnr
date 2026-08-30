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
static const char *const kFixture =
    "; stray_dlssnr.ini - hand-written, do not reflow.\r\n"
    ";\r\n"
    "; A second comment line, indented oddly:\r\n"
    ";      keep me exactly as I am.\r\n"
    "\r\n"
    "[stray_dlssnr]\r\n"
    "\r\n"
    "; The identification pins. The overlay must never round-trip these.\r\n"
    "shader_hash = 0x1708ec956099e259\r\n"
    "srv_velocity = 2\r\n"
    "enabled = 1\r\n"
    "hdr_codec = 1\r\n"
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
    "; ui_correction is deliberately ABSENT, so the append path is exercised too.\r\n"
    "totally_unknown_key = 42\r\n";

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
    check(has_line(out, "shader_hash = 0x1708ec956099e259"), "shader_hash was NOT round-tripped");
    check(has_line(out, "srv_velocity = 2"), "srv_velocity was NOT round-tripped");
    check(has_line(out, "enabled = 1"), "enabled (load-only) was NOT round-tripped");
    check(has_line(out, "hdr_codec = 1"), "hdr_codec (load-only) was NOT round-tripped");
    check(has_line(out, "totally_unknown_key = 42"), "an unrecognised key survived untouched");
    check(has_line(out, "; ui_correction is deliberately ABSENT, so the append path is exercised too."),
          "the comment above the absent key survived");

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

        // THE RANGE CLAMP, on the path that actually matters: a hand-edited ini, or one written
        // by an older build that offered 0..2 sliders. A value above 1.0 there is not merely out
        // of range, it is INERT - the snippet skips the intensity pass at >= 1.0 and clamps local
        // tone to 1.0 - so without the clamp the range fix would miss exactly the users who
        // already hit the bug. These are the values the bug was reported with.
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
                std::printf("  FAIL  could not write the stale-range fixture\n");
                return 1;
            }
            cfg::config st;
            cfg::load(st, sdir, [](const char *) {});
            check(st.intensity == 1.0f,                "clamp: a stale intensity = 2.00 is pulled to 1.0");
            check(st.local_tone_strength == 1.0f,      "clamp: a stale local_tone_strength = 1.55 is pulled to 1.0");
            check(st.local_structure_strength == 1.0f, "clamp: a stale local_structure_strength = 1.93 is pulled to 1.0");
            check(st.skin_structure_strength == -1.0f, "clamp: a negative skin strength normalises to the -1 inherit sentinel");
        }
        // The pins must be exactly what the fixture said, not what the overlay's defaults are.
        check(back.shader_hash == 0x1708ec956099e259ull, "reparse: shader_hash is untouched");
        check(back.srv_velocity == 2u,                   "reparse: srv_velocity is untouched");
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
