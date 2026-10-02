#include "HideMax.h"

#include "Addresses.h"

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

		// Max's gear (canteen, holster, ...) is drawn by NGraphicsEngine::CRenderBlockGeneralMM like any
		// prop, so only what sits ON his body goes: close beside him and between hip and head height. A
		// crate he stands on or a barrel next to him stays.
		DrawFn            origGearMain = nullptr;
		DrawFn            origGearOther = nullptr;
		std::atomic<int>  loggedGear{ 0 };

		bool IsMaxGear(void* a_instance)
		{
			if (!hiding || reinterpret_cast<std::uintptr_t>(a_instance) < 0x10000) {
				return false;
			}
			float t[3];
			if (!ReadPosition(a_instance, t)) {
				return false;
			}
			const float dx = t[0] - maxX, dy = t[1] - maxY, dz = t[2] - maxZ;
			const bool  gear = dx * dx + dz * dz < 0.6f * 0.6f && dy > 0.2f && dy < 2.0f;
			if (gear && loggedGear < 6) {
				++loggedGear;
				logger::info("hide Max: hiding gear at ({:.2f}, {:.2f}, {:.2f}), {:.2f} m up", t[0], t[1], t[2], dy);
			}
			return gear;
		}

		void __fastcall HookGearMain(void* a_block, void* a_context, void* a_instance)
		{
			if (!IsMaxGear(a_instance)) {
				origGearMain(a_block, a_context, a_instance);
			}
		}

		void __fastcall HookGearOther(void* a_block, void* a_context, void* a_instance)
		{
			if (!IsMaxGear(a_instance)) {
				origGearOther(a_block, a_context, a_instance);
			}
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
			return text.empty() ? nullptr : reinterpret_cast<void*>(Addresses::FromText(text));
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
		void* gearMain = Address("GearDrawMain");
		void* gearOther = Address("GearDrawOther");
		bool  gearOk = gearMain && gearOther;
		gearOk = gearOk && MH_CreateHook(gearMain, reinterpret_cast<void*>(&HookGearMain), reinterpret_cast<void**>(&origGearMain)) == MH_OK && MH_EnableHook(gearMain) == MH_OK;
		gearOk = gearOk && MH_CreateHook(gearOther, reinterpret_cast<void*>(&HookGearOther), reinterpret_cast<void**>(&origGearOther)) == MH_OK && MH_EnableHook(gearOther) == MH_OK;
		logger::info("hide Max: gear draws {}", gearOk ? "hooked" : "not hooked ([Hooks] GearDraw* missing or hook failed)");
	}

	void Update(bool a_hide, const Vec3& a_feet)
	{
		maxX = a_feet.x;
		maxY = a_feet.y;
		maxZ = a_feet.z;
		hiding = a_hide;
	}
}
