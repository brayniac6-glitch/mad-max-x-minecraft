#include "HideMax.h"

#include <MinHook.h>

// Max's own model out of the picture while Minecraft drives him (Steve is drawn instead), the way
// SkyCraft hides the Skyrim player's meshes. Max stays in the game (physics, AI targeting, missions);
// only his character render blocks skip drawing.
//
// Every character mesh is drawn by NGraphicsEngine::CRenderBlockCharacter (vftable 14129B878): slot 27
// (FUN_1408E81B0) and slot 28 (FUN_1408E7C30) take (block, context, instance); the instance carries
// its world matrix at +0x20 (translation at +0x50) (sheet/hooks.tsv H13).
namespace madcraft::HideMax
{
	namespace
	{
		using DrawFn = void(__fastcall*)(void*, void*, void*);
		DrawFn origMain = nullptr;
		DrawFn origOther = nullptr;

		std::atomic<bool>  hiding{ false };
		std::atomic<float> maxX{ 0 }, maxY{ 0 }, maxZ{ 0 };
		std::atomic<int>   logged{ 0 };

		// SEH-guarded: if a slot's parameters aren't what slot 28's are, a bad read just means "not Max".
		bool ReadPosition(const void* a_instance, float (&a_out)[3])
		{
			__try {
				const auto* t = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(a_instance) + 0x50);
				a_out[0] = t[0], a_out[1] = t[1], a_out[2] = t[2];
				return std::isfinite(a_out[0]) && std::isfinite(a_out[1]) && std::isfinite(a_out[2]);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool IsMax(void* a_instance)
		{
			if (!hiding || reinterpret_cast<std::uintptr_t>(a_instance) < 0x10000) {
				return false;
			}
			float t[3];
			if (!ReadPosition(a_instance, t)) {
				return false;
			}
			const float dx = t[0] - maxX, dy = t[1] - maxY, dz = t[2] - maxZ;
			if (logged < 6) {
				++logged;
				logger::info("hide Max: character instance at ({:.2f}, {:.2f}, {:.2f}), Max at ({:.2f}, {:.2f}, {:.2f})", t[0], t[1], t[2], maxX.load(), maxY.load(), maxZ.load());
			}
			return dx * dx + dz * dz < 0.8f * 0.8f && dy > -0.6f && dy < 2.2f;
		}

		void __fastcall HookMain(void* a_block, void* a_context, void* a_instance)
		{
			if (!IsMax(a_instance)) {
				origMain(a_block, a_context, a_instance);
			}
		}

		void __fastcall HookOther(void* a_block, void* a_context, void* a_instance)
		{
			if (!IsMax(a_instance)) {
				origOther(a_block, a_context, a_instance);
			}
		}

		void* Address(const char* a_key)
		{
			const auto text = IniString("Hooks", a_key, "");
			const auto plus = text.find('+');
			if (text.empty() || plus == std::string::npos) {
				return nullptr;
			}
			HMODULE module = ::GetModuleHandleA(text.substr(0, plus).c_str());
			try {
				return module ? reinterpret_cast<std::uint8_t*>(module) + std::stoull(text.substr(plus + 1), nullptr, 16) : nullptr;
			} catch (...) {
				return nullptr;
			}
		}
	}

	void Install()
	{
		void* main = Address("CharacterDrawMain");
		void* other = Address("CharacterDrawOther");
		bool  ok = main && other;
		ok = ok && MH_CreateHook(main, reinterpret_cast<void*>(&HookMain), reinterpret_cast<void**>(&origMain)) == MH_OK && MH_EnableHook(main) == MH_OK;
		ok = ok && MH_CreateHook(other, reinterpret_cast<void*>(&HookOther), reinterpret_cast<void**>(&origOther)) == MH_OK && MH_EnableHook(other) == MH_OK;
		logger::info("hide Max: character draws {}", ok ? "hooked" : "not hooked ([Hooks] CharacterDraw* missing or hook failed)");
	}

	void Update(bool a_hide, const Vec3& a_feet)
	{
		maxX = a_feet.x;
		maxY = a_feet.y;
		maxZ = a_feet.z;
		hiding = a_hide;
	}
}
