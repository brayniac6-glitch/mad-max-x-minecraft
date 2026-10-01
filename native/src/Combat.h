#pragma once

#include "MadMax.h"

// Minecraft weapons against Mad Max's NPCs (H09/H10). Nearby characters are published to the actor
// table, where the Fabric mod mirrors them as invisible hittable stand-ins; Minecraft's own combat
// (weapon damage, crits, Sharpness, Strength, attack cooldown, bows) produces the damage, which comes
// back as kEvHitActor and is applied through the character's own damage entry. Game thread only.
namespace madcraft::Combat
{
	void Init();
	void Update(const Vec3& a_playerFeet);
}
