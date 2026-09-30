#pragma once

#include "MadMax.h"

// Mad Max's real ground and walls for Minecraft's physics, from its own physics raycasts (Phase 1;
// replaces the flat Phase 0 floor when [Hooks] Raycast* are configured). Game thread only.
namespace madcraft::Collision
{
	bool Available();
	// Minecraft (re)connected: drop and resend everything (any thread).
	void RequestReset();
	std::uint32_t Epoch();
	// Once per game frame with the player's feet (Minecraft coordinates): scans and streams the
	// ground around them within a small time budget.
	void Update(const McVec& a_feet);
}
