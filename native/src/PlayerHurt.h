#pragma once

#include "MadMax.h"

// Mad Max hitting Max hurts the Minecraft player too (sheet/hooks.tsv H11). The character damage
// entry ([Hooks] CharacterDamage, CCharacter vtable slot 23) is hooked; what it took off Max, as a
// share of his max health, goes to Minecraft as kInHurt, where armour, shields and knockback apply.
// So the damage scales with the enemy and weapon the way Mad Max's own does. Only in Minecraft mode.
namespace madcraft::PlayerHurt
{
	void Install();
	// Minecraft's player died (kEvPlayerDied): Max dies too. Game thread.
	void KillMax();
}
