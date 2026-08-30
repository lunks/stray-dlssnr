// hdr_source_variants_test.cpp - the decode has TWO source variants, and this proves the second
// one is real.
//
// WHY IT EXISTS. Both graft modes live in ONE shader and ONE compile. Without a fallback, a
// d3dcompiler that cannot build mode 1's OkLab/AP1 matrices - under Proton that may be Wine's
// builtin, whose SM5 coverage varies by version - would fail the decode outright, latch
// codec_failed, and take the DEFAULT graft down with the experiment: the user who plays on mode 0
// every day would get the darkened pre-codec frame back. hdr_codec::build therefore retries with
// hdr_codec::full_source_decode(false), which flips exactly one character.
//
// A FALLBACK THAT IS NEVER EXERCISED IS NOT A FALLBACK. CI compiles both variants with fxc, but
// that gate reimplements the substitution in PowerShell; this one calls the REAL function, so a
// marker that moved, a replace that missed, or a "with_graft" that quietly returns the same text
// is caught here rather than on the one day it matters.
//
//     c++ -std=c++17 -O2 -I include tools/hdr_source_variants_test.cpp   (Windows toolchains only:
//                                                                        hdr_codec.hpp needs
//                                                                        <d3dcompiler.h>)
// Expected: 0 failed.

#include "../src/hdr_codec.hpp"

#include <cstdio>
#include <string>

static int g_fail = 0, g_pass = 0;
static void ck(bool c, const char *what, const char *detail = "")
{
	if (c) { ++g_pass; std::printf("  ok   %s %s\n", what, detail); }
	else   { ++g_fail; std::printf("  FAIL %s %s\n", what, detail); }
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("hdr_source_variants_test - the decode's two source variants\n");

	const std::string plain = hdr_codec::full_source(hdr_codec::kDecodeSource);
	bool changed_on = true;
	const std::string with = hdr_codec::full_source_decode(true, &changed_on);
	bool changed_off = false;
	const std::string without = hdr_codec::full_source_decode(false, &changed_off);

	// (1) The shipping variant must be full_source(kDecodeSource) EXACTLY. The on-disk cache is
	//     named by the FNV-1a of this text and CI's fxc gate compiles this text; if they diverge,
	//     the blob that ships is not the blob that was tested.
	ck(with == plain, "full_source_decode(true) == full_source(kDecodeSource), byte for byte");
	ck(!changed_on, "full_source_decode(true) reports no substitution");
	ck(hdr_codec::source_hash(with) == hdr_codec::source_hash(plain),
	   "...and therefore names the same cache file");

	// (2) The survival variant must actually be different, and different by exactly one character.
	ck(changed_off, "full_source_decode(false) reports that it substituted");
	ck(without != with, "the two variants are not the same text");
	ck(without.size() == with.size(), "they differ in CONTENT, not length - it is a 1 -> 0 flip");
	{
		size_t diffs = 0, at = 0;
		for (size_t i = 0; i < with.size() && i < without.size(); ++i)
			if (with[i] != without[i]) { ++diffs; at = i; }
		char d[128];
		std::snprintf(d, sizeof(d), "(%zu differing byte(s), first at offset %zu: '%c' -> '%c')",
		              diffs, at, with.empty() ? '?' : with[at], without.empty() ? '?' : without[at]);
		ck(diffs == 1, "exactly ONE byte differs between the variants", d);
	}

	// (3) The markers themselves, so a rename of the macro cannot silently disable the fallback.
	ck(with.find("#define NR_RDX_GRAFT 1") != std::string::npos,
	   "the shipping variant defines NR_RDX_GRAFT 1");
	ck(without.find("#define NR_RDX_GRAFT 0") != std::string::npos,
	   "the survival variant defines NR_RDX_GRAFT 0");
	ck(without.find("#define NR_RDX_GRAFT 1") == std::string::npos,
	   "...and does not still define it as 1 somewhere else");
	ck(with.find("#if NR_RDX_GRAFT") != std::string::npos,
	   "the source really branches on the macro");

	// (4) The graft's own symbols must all sit INSIDE the #if region. The substitution only
	//     changes a macro's VALUE - the text of both branches is still in the string either way -
	//     so what has to be proved is that the preprocessor will actually remove every reference:
	//     one graft symbol used from main() or from the stub, and the retry would fail for exactly
	//     the same reason as the first attempt and the whole fallback would be theatre.
	{
		const size_t if_at   = with.find("\n#if NR_RDX_GRAFT\n");
		const size_t else_at = with.find("\n#else   // NR_RDX_GRAFT == 0");
		const size_t end_at  = with.find("\n#endif  // NR_RDX_GRAFT");
		ck(if_at != std::string::npos && else_at != std::string::npos && end_at != std::string::npos,
		   "the #if / #else / #endif directives are all present, at line starts");
		ck(if_at < else_at && else_at < end_at, "...and in that order");

		static const char *const graft_symbols[] = {
			"float3x3", "nrRdxToOkLab", "nrRdxFromOkLab", "nrRdxClampAp1", "nrRdxHueOkLab",
			"nrRdxUpgradeToneMap", "nrRdxCbrtSigned", "nrRdxLuminance",
		};
		for (const char *sym : graft_symbols)
		{
			size_t hits = 0, outside = 0;
			for (size_t pos = 0; (pos = with.find(sym, pos)) != std::string::npos; ++pos)
			{
				// A mention in prose is harmless; only a use the compiler sees matters.
				const size_t ls = with.rfind('\n', pos);
				const size_t line_start = (ls == std::string::npos) ? 0 : ls + 1;
				const std::string before = with.substr(line_start, pos - line_start);
				if (before.find("//") != std::string::npos)
					continue;
				++hits;
				if (pos < if_at || pos > else_at)
					++outside;
			}
			char d[192];
			std::snprintf(d, sizeof(d), "(%s: %zu code use(s), %zu of them outside the #if block)",
			              sym, hits, outside);
			ck(hits > 0 && outside == 0, "every code use of the graft symbol is inside the #if", d);
		}

		// And the one symbol that must be OUTSIDE it, because main() calls it in both variants.
		size_t stub_defs = 0;
		for (size_t pos = 0; (pos = with.find("float3 nrRdxGradeDisplay(", pos)) != std::string::npos; ++pos)
			++stub_defs;
		ck(stub_defs == 2, "nrRdxGradeDisplay is defined exactly twice - once per branch");
		const size_t call_at = with.find("nrRdxGradeDisplay(originalDisplay");
		ck(call_at != std::string::npos && call_at > end_at,
		   "main() calls it from OUTSIDE the #if, so main() is identical in both variants");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
