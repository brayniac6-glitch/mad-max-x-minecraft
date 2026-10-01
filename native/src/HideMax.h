#pragma once

#include "MadMax.h"

// Max's model hidden while Minecraft drives him (see HideMax.cpp).
namespace madcraft::HideMax
{
	// Hooks the character draws ([Hooks] CharacterDrawMain / CharacterDrawOther). Call once.
	void Install();
	// Every frame: hide Max or not, and where he is (Mad Max world). Any thread.
	void Update(bool a_hide, const Vec3& a_feet);
}
