#pragma once

#include "Link.h"

// Everything that touches Mad Max's own memory goes through here. SkyCraft had CommonLibSSE for
// this; Mad Max has no such library, so each hook point is found in Ghidra (sheet/hooks.tsv) and
// entered into MadCraft.ini [Hooks] as a Cheat-Engine-style pointer chain:
//     PlayerMatrix = MadMax.exe+1A2B3C0, 18, 40
// meaning: read the pointer at module+0x1A2B3C0, add 0x18, read the pointer there, add 0x40.
// Anything not configured makes the matching feature switch itself off (logged once), so the
// plugin still loads and shows Minecraft's overlay before the reverse engineering is done.
namespace madcraft
{
	struct Vec3
	{
		float x, y, z;
	};

	struct McVec
	{
		double x, y, z;
	};

	namespace MadMax
	{
		// Reads [Hooks] and [World] from MadCraft.ini. Call once at startup.
		void Init();

		// A game is loaded and the player's transform resolves.
		bool PlayerAvailable();
		// Player's feet in Mad Max world space (Apex: metres, Y up).
		bool GetPlayerFeet(Vec3& a_out);
		// Heading: radians about the up axis, as stored in the player's world matrix.
		bool GetPlayerHeading(float& a_out);
		// Moves and turns Max. Through the game's own SetTransform when [Hooks] PlayerSetTransform is
		// configured (so Havok's character controller moves too), else by writing the matrix.
		bool SetPlayerPose(const Vec3& a_feet, float a_heading);
		// Max is in a vehicle (the Magnum Opus drives; Minecraft rides along).
		bool InVehicle();

		// Mad Max's render camera ([Hooks] CameraMatrix): position and the direction it looks, in Mad
		// Max world space. Which matrix row is "forward" is found at runtime (see Game.cpp).
		bool GetCameraMatrix(float (&a_m)[16]);
		// The same camera's world -> clip matrix (row vectors: clip = p * M), for drawing into Mad
		// Max's frame. It sits 0x180 after the world matrix ([141715F90]+0x5E0 +0x1D4).
		bool GetCameraViewProj(float (&a_m)[16]);
		// Mad Max's clock in hours ([Hooks] TimeOfDay: what its GetTimeOfDay script function returns).
		bool GetTimeOfDay(float& a_hours);
		// The render camera object itself (for the camera driver), 0 if not resolvable.
		std::uintptr_t RenderCameraObject();

		// Mad Max's physics raycast against the static world only (terrain, buildings, rocks), in Mad
		// Max world space. Game thread only. Returns false on a miss or when [Hooks] Raycast* aren't set.
		bool RaycastAvailable();
		bool RaycastStatic(const Vec3& a_from, const Vec3& a_to, Vec3& a_hit);

		// Mad Max world <-> Minecraft blocks. Scale and axis flips come from [World] in the ini
		// until they are verified in game; Apex and Minecraft are both Y-up.
		McVec ToMc(const Vec3& a_p);
		Vec3  FromMc(double a_x, double a_y, double a_z);
		float HeadingToMcYaw(float a_rad);
		float McYawToHeading(float a_deg);
	}

	// Pointer-chain helpers, safe against bad pointers (a wrong chain returns false, never crashes).
	bool SafeRead(std::uintptr_t a_addr, void* a_out, std::size_t a_bytes);
	bool SafeWrite(std::uintptr_t a_addr, const void* a_in, std::size_t a_bytes);

	// MadCraft.ini (next to the plugin in <game>/madcraft/).
	std::filesystem::path ModDir();
	std::string           IniString(const char* a_section, const char* a_key, const char* a_default);
	double                IniDouble(const char* a_section, const char* a_key, double a_default);
	bool                  IniBool(const char* a_section, const char* a_key, bool a_default);
}
