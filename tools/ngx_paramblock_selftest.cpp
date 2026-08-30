// ngx_paramblock_selftest.cpp - drive OUR NVSDK_NGX_Parameter block exactly the way the deployed
// nvngx_dlssnr.dll drives it, and replay the snippet's own parameter-read function against it.
//
// WHY THIS EXISTS
//   src/ngx_interop.hpp lays out a 17-slot MSVC vtable BY HAND. Nothing in the type system checks
//   it. If a slot ever moved, the symptom would not be a compile error or a crash - it would be a
//   parameter that silently reads back as "absent", the snippet substituting its own fallback, and
//   a control in the overlay that moves nothing. That is a bug you can stare at for a day.
//
//   So this test never names a getter. It loads the vtable pointer out of the object and calls
//   through the BYTE OFFSET taken from the snippet's disassembly, which is what the snippet
//   itself does:
//       [BIN 0x18001a92c]  mov rax, qword ptr [rax + 0x70]   ; Get(const char*, float*)
//       [BIN 0x18001a9f7]  mov rax, qword ptr [rax + 0x58]   ; Get(const char*, int*)
//       [BIN 0x18001a402]  mov rax, qword ptr [rax + 0x40]   ; Get(const char*, void**)
//       [BIN 0x18001aabc]  mov rax, qword ptr [rax + 0x60]   ; Get(const char*, unsigned*)
//   and, on the write side, the offsets the renodx reference add-on uses: +0x08 Set(ID3D12
//   Resource*), +0x20 Set(unsigned), +0x30 Set(float).
//
//   replay_19f30() below is a line-by-line transcription of the snippet's fn 0x180019f30 - the
//   ONE function reached from NVSDK_NGX_D3D12_EvaluateFeature that reads the tuning keys
//   [BIN 0x1800159c0 -> 0x180018620 (call at 0x1800186e4) -> 0x180019f30]. Because it is a
//   transcription and not a guess, running our real block through it shows what the snippet's own
//   stack struct ends up holding for a given add-on configuration.
//
// ALL OFFSETS ARE FROM THE DEPLOYED SNIPPET, md5 eea91faf55a8993656c66815f0497b3b, which is the
// file measured on the target machine at
//   .../steamapps/common/Stray/Hk_project/Binaries/Win64/nvngx_dlssnr.dll
// Its .text is byte-identical to the stock snippet apart from the caller-gate patches, so these
// offsets are stable across the patched and unpatched copies.
//
// WHAT IT CAN AND CANNOT PROVE
//   It proves a value SET by the add-on is the value the snippet READS. It cannot prove the
//   network acted on that value - no host-side test can, and the only evidence for that is a
//   pixel difference between two captures at different settings.
//
// This is deliberately NOT in src/: build.sh compiles src/*.cpp into the add-on, and this has a
// main(). Run it with:
//
//     tools/run_ngx_paramblock_selftest.sh
//
// Expected: ALL TESTS PASSED (0 failures).
#include "ngx_interop.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if(!(cond)) { ++g_fail; std::printf("  FAIL  "); std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)

// ---- raw vtable dispatch, byte offsets exactly as the snippet uses them --------------------
static inline const void *slot(void *obj, unsigned byte_off)
{
	const void *const *vt = *reinterpret_cast<const void *const *const *>(obj);
	return vt[byte_off / 8];
}
typedef ngx::Result (*fn_get_pp)(void *, const char *, void **);
typedef ngx::Result (*fn_get_i) (void *, const char *, int *);
typedef ngx::Result (*fn_get_u) (void *, const char *, unsigned *);
typedef ngx::Result (*fn_get_f) (void *, const char *, float *);
typedef void (*fn_set_f)(void *, const char *, float);
typedef void (*fn_set_u)(void *, const char *, unsigned);
typedef void (*fn_set_r)(void *, const char *, ID3D12Resource *);

// [BIN 0x18001a402] mov rax,[rax+0x40]  -> Get(void**)
static ngx::Result GET_PP(void *o, const char *k, void **v){ return ((fn_get_pp)slot(o,0x40))(o,k,v); }
// [BIN 0x18001a9f7] mov rax,[rax+0x58]  -> Get(int*)
static ngx::Result GET_I (void *o, const char *k, int *v)  { return ((fn_get_i) slot(o,0x58))(o,k,v); }
// [BIN 0x18001aabc] mov rax,[rax+0x60]  -> Get(unsigned*)
static ngx::Result GET_U (void *o, const char *k, unsigned *v){ return ((fn_get_u)slot(o,0x60))(o,k,v); }
// [BIN 0x18001a92c] mov rax,[rax+0x70]  -> Get(float*)
static ngx::Result GET_F (void *o, const char *k, float *v){ return ((fn_get_f) slot(o,0x70))(o,k,v); }
// renodx parity: Set(float) +0x30, Set(uint) +0x20, Set(ID3D12Resource*) +0x08
static void SET_F(void *o, const char *k, float v)          { ((fn_set_f)slot(o,0x30))(o,k,v); }
static void SET_U(void *o, const char *k, unsigned v)       { ((fn_set_u)slot(o,0x20))(o,k,v); }
static void SET_R(void *o, const char *k, ID3D12Resource *v){ ((fn_set_r)slot(o,0x08))(o,k,v); }

static bool is_fail(ngx::Result r){ return (r & 0xFFF00000u) == 0xBAD00000u; }

// ---- a faithful replay of fn 0x180019f30, transcribed from the disassembly ------------------
struct eval_params    // the stack struct at rbp+0x1c0
{
	void *control_mask;      // +0x60
	float mvec_scale_x;      // +0xd8
	float mvec_scale_y;      // +0xdc
	float intensity;         // +0xe0
	float local_tone;        // +0xe4
	float local_structure;   // +0xe8
	int   use_auto_mask;     // +0xf0
	float skin_structure;    // +0xf4
	float eff_skin;          // +0xf8
	float eff_local;         // +0xfc
	float scaling_ratio;     // +0x120
	unsigned style;
};

static eval_params replay_19f30(void *p)
{
	eval_params s{};
	// 0x180019f4f..0x180019fb3: the whole struct is zeroed first.
	s.control_mask = nullptr;
	// 0x180019fbd..0x18001a024: then defaulted.
	s.mvec_scale_x = s.mvec_scale_y = s.intensity = s.local_tone = s.local_structure = 1.0f;
	s.use_auto_mask = 0;
	s.skin_structure = s.eff_skin = s.eff_local = -1.0f;

	// 0x18001a402: DLSSNR.ControlMask through Get(void**). On FAIL the snippet jumps past the
	// whole sub-block, leaving the zeroed null in place.
	void *cm = nullptr;
	if (!is_fail(GET_PP(p, ngx::kParamControlMask, &cm))) s.control_mask = cm;

	// 0x18001a8c4 / 0x18001a8f8 / 0x18001a930 / 0x18001a993 / 0x18001a9c7 : Get(float*), and on
	// FAIL each stores 1.0f.  0x18001a9fb: UseAutoMask via Get(int*), FAIL -> 0.
	// 0x18001aa2f: SkinStructureStrength via Get(float*), FAIL -> -1.0f.
	float f;
	if (is_fail(GET_F(p, ngx::kParamMVecScaleX, &f))) f = 1.0f; s.mvec_scale_x = f;
	if (is_fail(GET_F(p, ngx::kParamMVecScaleY, &f))) f = 1.0f; s.mvec_scale_y = f;
	if (is_fail(GET_F(p, ngx::kParamIntensity,  &f))) f = 1.0f; s.intensity    = f;
	// 0x18001a964 then 0x18001a96a: ScalingRatio is read and then OVERWRITTEN with 1.0f
	// UNCONDITIONALLY - no 0xbad00000 guard, unlike every other float. It is dead.
	if (is_fail(GET_F(p, ngx::kParamScalingRatio, &f))) f = 1.0f;
	s.scaling_ratio = 1.0f;
	if (is_fail(GET_F(p, ngx::kParamLocalToneStrength,      &f))) f = 1.0f; s.local_tone      = f;
	if (is_fail(GET_F(p, ngx::kParamLocalStructureStrength, &f))) f = 1.0f; s.local_structure = f;
	int i = 0;
	if (is_fail(GET_I(p, ngx::kParamUseAutoMask, &i))) i = 0; s.use_auto_mask = i;
	if (is_fail(GET_F(p, ngx::kParamSkinStructureStrength, &f))) f = -1.0f; s.skin_structure = f;

	// 0x18001aa4b: a NON-NULL ControlMask forces UseAutoMask to 0.
	if (s.control_mask != nullptr) s.use_auto_mask = 0;

	// 0x18001aa59..0x18001aaa1: the effective pair.
	if (s.use_auto_mask != 0)
	{
		float skin = s.skin_structure;
		if (!(skin >= 0.0f)) skin = s.local_structure;   // comiss/jae: skin < 0 inherits local
		s.eff_skin  = skin;
		s.eff_local = s.local_structure;
	}
	else
	{
		s.eff_skin = s.eff_local = -1.0f;                // [BIN 0x1800afc40] = -1.0f
	}
	unsigned u = 0;
	if (is_fail(GET_U(p, ngx::kParamStyle, &u))) u = 0; s.style = u;
	return s;
}

int main()
{
	std::printf("== our parameter block, driven through the snippet's own vtable byte offsets ==\n\n");
	ngx::parameter_block blk;
	void *p = &blk;

	// ---- 1. the table is where we say it is ------------------------------------------------
	CHECK(slot(p,0x40) == (const void*)&ngx::detail::get_voidptr, "+0x40 is not get_voidptr");
	CHECK(slot(p,0x58) == (const void*)&ngx::detail::get_int,     "+0x58 is not get_int");
	CHECK(slot(p,0x60) == (const void*)&ngx::detail::get_uint,    "+0x60 is not get_uint");
	CHECK(slot(p,0x70) == (const void*)&ngx::detail::get_float,   "+0x70 is not get_float");
	CHECK(slot(p,0x08) == (const void*)&ngx::detail::set_d3d12,   "+0x08 is not set_d3d12");
	CHECK(slot(p,0x20) == (const void*)&ngx::detail::set_uint,    "+0x20 is not set_uint");
	CHECK(slot(p,0x30) == (const void*)&ngx::detail::set_float,   "+0x30 is not set_float");
	CHECK(slot(p,0x80) == (const void*)&ngx::detail::reset,       "+0x80 is not reset");
	std::printf("1. vtable byte offsets match the snippet's call sites .......... %s\n",
	            g_fail ? "FAIL" : "ok");

	// ---- 2. an UNWRITTEN key must read back as 0xBAD00000, not Success ---------------------
	{
		float f = 12345.0f; int i = 7; void *pp = (void*)0xdeadbeef;
		CHECK(GET_F(p, ngx::kParamIntensity, &f) == 0xBAD00000u, "unwritten float did not Fail");
		CHECK(GET_I(p, ngx::kParamUseAutoMask, &i) == 0xBAD00000u, "unwritten int did not Fail");
		CHECK(GET_PP(p, "DLSSNR.UI", &pp) == 0xBAD00000u, "unwritten pointer did not Fail");
		CHECK(f == 12345.0f && i == 7, "a MISS clobbered the caller's pre-seeded default");
		std::printf("2. a miss returns 0xBAD00000 and leaves *out untouched ........ ok\n");
	}

	// ---- 3. the five tuning values survive Set->Get through the real slots -----------------
	const float kIntensity = 2.0f, kTone = 1.55f, kStruct = 1.93f, kSkin = 1.0f;
	SET_F(p, ngx::kParamIntensity,              kIntensity);
	SET_F(p, ngx::kParamLocalToneStrength,      kTone);
	SET_F(p, ngx::kParamLocalStructureStrength, kStruct);
	SET_F(p, ngx::kParamSkinStructureStrength,  kSkin);
	SET_U(p, ngx::kParamUseAutoMask,            1u);
	SET_U(p, ngx::kParamStyle,                  0u);
	SET_F(p, ngx::kParamMVecScaleX, 1.0f);
	SET_F(p, ngx::kParamMVecScaleY, 1.0f);
	SET_R(p, ngx::kParamControlMask, static_cast<ID3D12Resource *>(nullptr));
	{
		float f = 0; CHECK(GET_F(p, ngx::kParamIntensity, &f) == 1u && f == kIntensity,
		                   "Intensity round-trip: got %f", f);
		int i = -1;  CHECK(GET_I(p, ngx::kParamUseAutoMask, &i) == 1u && i == 1,
		                   "UseAutoMask Set(uint)->Get(int) cross-type: got %d", i);
		std::printf("3. Set(float)+0x30 -> Get(float*)+0x70 round-trips exactly ..... ok\n");
		std::printf("   Set(uint)+0x20  -> Get(int*)+0x58 cross-type coercion ....... ok\n");
	}

	// ---- 4. the ControlMask null: Success with a null payload, not a miss ------------------
	{
		void *cm = (void *)0xdeadbeef;
		const ngx::Result r = GET_PP(p, ngx::kParamControlMask, &cm);
		CHECK(r == 1u, "explicit null ControlMask did not return Success (got 0x%x)", r);
		CHECK(cm == nullptr, "explicit null ControlMask did not return a null pointer");
		std::printf("4. explicit-null ControlMask -> Success + nullptr .............. ok\n");
		std::printf("   so [rdi+0x60]==0 and the 0x18001aa4b force-off is NOT taken.\n");
	}

	// ---- 5. replay the snippet's own read block --------------------------------------------
	{
		eval_params s = replay_19f30(p);
		std::printf("\n5. replay of fn 0x180019f30 with UseAutoMask=1:\n");
		std::printf("     +0x60 ControlMask      = %p\n", s.control_mask);
		std::printf("     +0xe0 Intensity        = %.4f  (we set %.4f)\n", s.intensity, kIntensity);
		std::printf("     +0xe4 LocalTone        = %.4f  (we set %.4f)\n", s.local_tone, kTone);
		std::printf("     +0xe8 LocalStructure   = %.4f  (we set %.4f)\n", s.local_structure, kStruct);
		std::printf("     +0xf0 UseAutoMask      = %d\n", s.use_auto_mask);
		std::printf("     +0xf4 SkinStructure    = %.4f  (we set %.4f)\n", s.skin_structure, kSkin);
		std::printf("     +0xf8 EFFECTIVE skin   = %.4f\n", s.eff_skin);
		std::printf("     +0xfc EFFECTIVE local  = %.4f\n", s.eff_local);
		std::printf("     +0x120 ScalingRatio    = %.4f  <- clobbered to 1.0 unconditionally, DEAD\n", s.scaling_ratio);
		CHECK(s.intensity == kIntensity, "Intensity did not reach the struct");
		CHECK(s.local_tone == kTone, "LocalTone did not reach the struct");
		CHECK(s.eff_local == kStruct, "effective LocalStructure wrong: %f", s.eff_local);
		CHECK(s.eff_skin  == kSkin,   "effective SkinStructure wrong: %f", s.eff_skin);
		CHECK(s.use_auto_mask == 1, "UseAutoMask was forced off");
	}

	// ---- 6. the UseAutoMask=0 gate, and the skin<0 inherit ---------------------------------
	{
		SET_U(p, ngx::kParamUseAutoMask, 0u);
		eval_params s = replay_19f30(p);
		std::printf("\n6. same block with UseAutoMask=0:\n");
		std::printf("     +0xf8 EFFECTIVE skin   = %.4f\n", s.eff_skin);
		std::printf("     +0xfc EFFECTIVE local  = %.4f   <- BOTH replaced by the -1.0f at 0x1800afc40\n", s.eff_local);
		CHECK(s.eff_skin == -1.0f && s.eff_local == -1.0f,
		      "UseAutoMask=0 did not bypass both structure strengths");
		SET_U(p, ngx::kParamUseAutoMask, 1u);
		SET_F(p, ngx::kParamSkinStructureStrength, -0.5f);
		s = replay_19f30(p);
		std::printf("   with UseAutoMask=1 and SkinStructure=-0.5 (negative = inherit):\n");
		std::printf("     +0xf8 EFFECTIVE skin   = %.4f   <- inherited LocalStructure\n", s.eff_skin);
		CHECK(s.eff_skin == kStruct, "negative skin did not inherit LocalStructure");
		SET_F(p, ngx::kParamSkinStructureStrength, kSkin);
	}

	// ---- 7. a NON-NULL ControlMask really does kill both structure strengths ---------------
	{
		SET_R(p, ngx::kParamControlMask, reinterpret_cast<ID3D12Resource *>(0x1000));
		eval_params s = replay_19f30(p);
		std::printf("\n7. with a NON-NULL ControlMask bound:\n");
		std::printf("     +0xf0 UseAutoMask      = %d      <- forced off at 0x18001aa52\n", s.use_auto_mask);
		std::printf("     +0xfc EFFECTIVE local  = %.4f\n", s.eff_local);
		CHECK(s.use_auto_mask == 0 && s.eff_local == -1.0f,
		      "a bound ControlMask did not force the structure bypass");
		SET_R(p, ngx::kParamControlMask, static_cast<ID3D12Resource *>(nullptr));
	}

	// ---- 8. the trace records what the snippet asked for ------------------------------------
	{
		blk.get_trace.arm();
		eval_params s = replay_19f30(p);
		(void)s;
		void *dummy = nullptr;
		GET_PP(p, "DLSSNR.UI", &dummy);          // a key we never write
		blk.get_trace.disarm();
		const int n = blk.get_trace.stored();
		std::printf("\n8. getter trace: %d Get(s) recorded, %d seen\n", n, blk.get_trace.seen());
		bool saw_intensity = false, saw_ui_miss = false;
		for (int k = 0; k < n; ++k)
		{
			const auto &r = blk.get_trace.records[k];
			if (std::strcmp(r.key, ngx::kParamIntensity) == 0)
			{
				saw_intensity = true;
				CHECK(r.hit && r.slot == 14 && r.numeric == (double)kIntensity,
				      "Intensity trace record wrong");
				std::printf("     %-32s %s -> HIT %.4f\n", r.key, ngx::get_slot_name(r.slot), r.numeric);
			}
			if (std::strcmp(r.key, "DLSSNR.UI") == 0 && !r.hit)
			{
				saw_ui_miss = true;
				std::printf("     %-32s %s -> MISS (snippet falls back)\n", r.key, ngx::get_slot_name(r.slot));
			}
		}
		CHECK(saw_intensity, "the trace did not record DLSSNR.Intensity");
		CHECK(saw_ui_miss,   "the trace did not record the DLSSNR.UI miss");
	}

	// ---- 9. disarmed trace records nothing --------------------------------------------------
	{
		blk.get_trace.arm(); blk.get_trace.disarm();
		float f; GET_F(p, ngx::kParamIntensity, &f);
		CHECK(blk.get_trace.seen() == 0, "a disarmed trace still recorded");
		std::printf("\n9. a disarmed trace records nothing ............................ ok\n");
	}

	std::printf("\n%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED",
	            g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
