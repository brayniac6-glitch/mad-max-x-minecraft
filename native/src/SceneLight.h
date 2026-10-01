#pragma once

#include "MadMax.h"

// Minecraft's light in Mad Max's world, and Mad Max's darkness on Minecraft's player.
//  - Lights: every light-emitting Minecraft block (torches, lanterns, glowstone, fire, lava; sent as
//    kRenLights) and a light the player holds light Mad Max's own surfaces: a screen pass over Mad
//    Max's finished frame that rebuilds each pixel's position from the scene depth and adds the
//    light the way Minecraft's falls off (level - distance), flickering for flames.
//  - Shade: how lit Mad Max's frame is around the player, measured from the frame itself (a small
//    mip read back to the CPU), sent to Minecraft (MadState::shade) to darken Steve, the hand and
//    held items in Mad Max's shadows, interiors and nights.
// Render thread only.
namespace madcraft::SceneLight
{
	void OnLights(const std::uint8_t* a_data, std::uint32_t a_bytes);
	void RemoveSection(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz);
	void Clear();

	struct FrameInfo
	{
		ID3D11DepthStencilView*   depth;       // Mad Max's scene depth (live or snapshot)
		float                     clearDepth;  // its clear value (the sky)
		bool                      flipDepth;   // buffer depth = 1 - the matrix's
		float                     viewProj[4][4];  // camera-relative Mad Max world -> clip, row vectors
		Vec3                      camPos;
		ID3D11ShaderResourceView* scene;      // copy of Mad Max's frame (mip 0 = this frame), or null
		UINT                      width, height;
	};
	// Adds Minecraft's lights to Mad Max's frame (render target a_rtv). Changes pipeline state; the
	// caller saves and restores it.
	void Apply(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_rtv, const FrameInfo& a_frame);

	// Reads back a small mip of the scene copy (a frame or two late) and updates State().shade.
	void MeasureShade(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_scene, UINT a_width, UINT a_height, DXGI_FORMAT a_format);
	// The same measurement as a 0..1 light factor (before Minecraft's light curve), for Minecraft's
	// blocks and mobs drawn into the frame.
	float SceneFactor();
}
