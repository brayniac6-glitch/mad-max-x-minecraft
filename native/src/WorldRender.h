#pragma once

#include "MadMax.h"

namespace madcraft::WorldRender
{
	// Present hook, before the HUD overlay: drains Minecraft's meshes and draws its blocks, Steve and
	// entities into Mad Max's frame with Mad Max's camera and scene depth.
	void Draw(ID3D11Device* a_device, ID3D11DeviceContext* a_context, IDXGISwapChain* a_swapChain);
}
