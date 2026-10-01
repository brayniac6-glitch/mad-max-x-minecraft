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
	// First person in a car: the eyes ride with the player (Max in the seat), placed from Max's
	// transform at the moment Mad Max updates its camera, so they move exactly with the car this frame.
	// a_lookYaw is relative to the car (Minecraft degrees), a_pitch absolute; a_eyeY metres up,
	// a_eyeForward metres along the car.
	void SetAttached(std::uintptr_t a_camera, float a_lookYaw, float a_pitch, float a_eyeY, float a_eyeForward, float a_handed);
	// Mad Max's own camera again (driving, menus, Mad Max controls).
	void Release();

	// A Mad Max camera world matrix looking along Minecraft's yaw/pitch (degrees) from a_eye.
	// a_handed: +1/-1, Mad Max's camera row convention (row0 = handed * (row1 x row2)).
	void LookMatrix(float a_yawDeg, float a_pitchDeg, const Vec3& a_eye, float a_handed, float (&a_out)[16]);
}
