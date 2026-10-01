#include "PlayerHurt.h"

#include "Combat.h"
#include "Game.h"

#include <MinHook.h>

namespace madcraft::PlayerHurt
{
	namespace
	{
		// CCharacter's damage entry FUN_1401480C0(character, CDamageInflictor*, ?, ?): returns what it
		// took off (float in the low 32 bits) after armour and damage-type modifiers (H10).
		using DamageFn = std::uint64_t(__fastcall*)(void*, void*, void*, void*);
		DamageFn origDamage = nullptr;

		bool  enabled = true;
		float scale = 1.0f;          // Minecraft damage = Mad Max damage / Max's max health * 20 * scale
		bool  deathLinked = true;    // Minecraft's player dying kills Max

		std::uintptr_t PlayerCharacter()
		{
			std::uintptr_t matrix = 0;
			return MadMax::PlayerMatrixAddress(matrix) && matrix > 0x1D8 ? matrix - 0x1D8 : 0;
		}

		// The enemy nearest Max (the one most likely hitting him): Minecraft blocks and knocks back
		// from its stand-in. Melee range, or further (gunfire).
		std::uint32_t Attacker(const Vec3& a_max, bool& a_melee)
		{
			std::vector<MadMax::Character> list;
			a_melee = false;
			if (!MadMax::ListCharacters(list)) {
				return 0;
			}
			float          best = 40.0f * 40.0f;
			std::uintptr_t obj = 0;
			for (const auto& c : list) {
				const float dx = c.feet.x - a_max.x, dy = c.feet.y - a_max.y, dz = c.feet.z - a_max.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (!c.dead && d2 > 0.04f && d2 < best) {
					best = d2;
					obj = c.object;
				}
			}
			a_melee = obj && best < 3.5f * 3.5f;
			return obj ? Combat::ActorId(obj) : 0;
		}

		std::uint64_t __fastcall HookDamage(void* a_character, void* a_hit, void* a_p3, void* a_p4)
		{
			const auto me = reinterpret_cast<std::uintptr_t>(a_character);
			float      before = 0.0f, max = 0.0f;
			const bool isMax = enabled && me && me == PlayerCharacter();
			if (isMax) {
				MadMax::ReadHealth(me, before, max);
			}
			const auto result = origDamage(a_character, a_hit, a_p3, a_p4);
			if (!isMax) {
				return result;
			}
			auto& st = State();
			if (st.madMaxControls || !st.mcInWorld) {
				return result;  // Mad Max controls: Max's health as the game has it, nothing for Minecraft
			}
			float after = 0.0f;
			MadMax::ReadHealth(me, after, max);
			float taken = before - after;
			if (!(taken > 0.0f)) {
				const auto bits = static_cast<std::uint32_t>(result);
				std::memcpy(&taken, &bits, sizeof(taken));  // what the game says it applied
			}
			if (!(taken > 0.0f) || !(max > 0.0f) || !std::isfinite(taken)) {
				return result;
			}
			const float mc = taken / max * 20.0f * scale;
			Vec3        feet{};
			bool        melee = false;
			const auto  attacker = MadMax::GetPlayerFeet(feet) ? Attacker(feet, melee) : 0u;
			{
				// Minecraft divides the value by 100 and by its MADMAX_TO_MC_DAMAGE (5).
				Link::Get().PushInput(proto::kInHurt, melee ? proto::kHurtMelee : attacker ? proto::kHurtProjectile : proto::kHurtOther,
					static_cast<std::int32_t>(std::lround(mc * 5.0f * 100.0f)), static_cast<std::int32_t>(attacker), 0);
			}
			logger::info("player hurt: Mad Max took {:.1f} of {:.0f} health -> {:.2f} Minecraft damage ({} from {:08X})", taken, max, mc,
				melee ? "melee" : "ranged/other", attacker);
			return result;
		}
	}

	void Install()
	{
		enabled = IniBool("Combat", "bMaxHitsHurtMinecraft", true);
		scale = static_cast<float>(IniDouble("Combat", "fPlayerDamageScale", 1.0));
		deathLinked = IniBool("Combat", "bMinecraftDeathKillsMax", true);
		const auto text = IniString("Hooks", "CharacterDamage", "");
		const auto plus = text.find('+');
		if (!enabled || plus == std::string::npos) {
			logger::info("player hurt: off");
			return;
		}
		std::uintptr_t rva = 0;
		try {
			rva = std::stoull(text.substr(plus + 1), nullptr, 16);
		} catch (...) {
			return;
		}
		void*      target = reinterpret_cast<void*>(MadMax::ModuleBase() + rva);
		const bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&HookDamage), reinterpret_cast<void**>(&origDamage)) == MH_OK &&
		                MH_EnableHook(target) == MH_OK;
		logger::info("player hurt: Max's hits go to Minecraft (x{}){}", scale, ok ? "" : " - HOOK FAILED");
	}

	void KillMax()
	{
		if (!deathLinked || State().madMaxControls) {
			return;
		}
		if (const auto me = PlayerCharacter()) {
			const bool ok = MadMax::SetHealth(me, 0.0f);
			logger::info("player hurt: Minecraft's player died; Max too{}", ok ? "" : " (FAILED)");
		}
	}
}
