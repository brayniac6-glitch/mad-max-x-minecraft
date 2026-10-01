#pragma once

#include "MadMax.h"

// Minecraft's eyes as Mad Max's render camera (see CameraDriver.cpp).
namespace madcraft::CameraDriver
{
	// Hooks the render camera's per-frame update ([Hooks] RenderCameraUpdate). Call once.
	bool Install();
	// This frame's camera: a_camera is the render camera object, a_world its world matrix in Mad
	// Max's layout (row-major, translation in row 3). Any thread.
	void Set(std::uintptr_t a_camera, const float (&a_world)[16]);
	// Mad Max's own camera again (driving, menus, Mad Max controls).
	void Release();
}
