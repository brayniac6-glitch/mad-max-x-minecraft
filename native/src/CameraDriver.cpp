#include "CameraDriver.h"

#include "Addresses.h"

#include <MinHook.h>

// SkyCraft's CameraDriver (MIT, chasmlol) for Mad Max: while Minecraft drives the player, Mad Max's
// render camera looks out of Minecraft's eyes (first person), or from Minecraft's F5 position.
//
// Mad Max's render camera ([141715F90]+0x5E0) updates once per rendered frame in
// FUN_1408926F0(camera, ?, t): it interpolates its world matrix (+0x54) between a "from" (+0x94) and
// a "to" (+0xD4) transform by t, then derives the view, projection, view-projection and frustum. Writing
// our camera into both ends just before that call makes Mad Max render from it, with its own view,
// culling and shadows following (sheet/hooks.tsv H06).
namespace madcraft::CameraDriver
{
	namespace
	{
		using UpdateFn = void(__fastcall*)(void*, void*, float);
		UpdateFn origUpdate = nullptr;

		std::mutex          lock;
		bool                active = false;
		float               matrix[16]{};
		std::atomic<std::uintptr_t> mainCamera{ 0 };

		// SetAttached: rebuilt in the hook from Max's transform at that moment.
		bool  attached = false;
		float lookYaw = 0.0f, lookPitch = 0.0f, eyeY = 0.0f, eyeForward = 0.0f, handed = 1.0f;

		void BuildAttached()
		{
			Vec3  feet{};
			float heading = 0.0f;
			if (!MadMax::GetPlayerFeet(feet) || !MadMax::GetPlayerHeading(heading)) {
				return;  // keep the last matrix
			}
			const float carYaw = MadMax::HeadingToMcYaw(heading);
			const float cy = carYaw * 0.017453292f;
			const Vec3  fw = MadMax::FromMc(-std::sin(cy), 0.0, std::cos(cy));
			const Vec3  base = MadMax::FromMc(0.0, 0.0, 0.0);
			const Vec3  eye{ feet.x + (fw.x - base.x) * eyeForward, feet.y + eyeY, feet.z + (fw.z - base.z) * eyeForward };
			LookMatrix(carYaw + lookYaw, lookPitch, eye, handed, matrix);
		}

		void __fastcall HookUpdate(void* a_camera, void* a_p2, float a_t)
		{
			{
				std::lock_guard g{ lock };
				if (active && reinterpret_cast<std::uintptr_t>(a_camera) == mainCamera.load()) {
					if (attached) {
						BuildAttached();
					}
					auto* base = static_cast<std::uint8_t*>(a_camera);
					std::memcpy(base + 0x94, matrix, sizeof(matrix));
					std::memcpy(base + 0xD4, matrix, sizeof(matrix));
				}
			}
			origUpdate(a_camera, a_p2, a_t);
		}
	}

	bool Install()
	{
		const auto text = IniString("Hooks", "RenderCameraUpdate", "");
		if (text.empty()) {
			logger::info("camera driver: [Hooks] RenderCameraUpdate not set; Mad Max keeps its camera");
			return false;
		}
		void* target = reinterpret_cast<void*>(Addresses::FromText(text));
		if (!target) {
			logger::warn("camera driver: RenderCameraUpdate '{}' not found in this build", text);
			return false;
		}
		const bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&origUpdate)) == MH_OK &&
		                MH_EnableHook(target) == MH_OK;
		logger::info("camera driver: render camera update {}", ok ? "hooked" : "hook failed");
		return ok;
	}

	void Set(std::uintptr_t a_camera, const float (&a_world)[16])
	{
		std::lock_guard g{ lock };
		mainCamera = a_camera;
		std::memcpy(matrix, a_world, sizeof(matrix));
		active = true;
		attached = false;
	}

	void SetAttached(std::uintptr_t a_camera, float a_lookYaw, float a_pitch, float a_eyeY, float a_eyeForward, float a_handed)
	{
		std::lock_guard g{ lock };
		mainCamera = a_camera;
		lookYaw = a_lookYaw, lookPitch = a_pitch, eyeY = a_eyeY, eyeForward = a_eyeForward, handed = a_handed;
		if (!active || !attached) {
			BuildAttached();
		}
		active = true;
		attached = true;
	}

	void Release()
	{
		std::lock_guard g{ lock };
		active = false;
		attached = false;
	}

	void LookMatrix(float a_yawDeg, float a_pitchDeg, const Vec3& a_eye, float a_handed, float (&a_out)[16])
	{
		const float yaw = a_yawDeg * 0.017453292f;
		const float pitch = a_pitchDeg * 0.017453292f;
		// Minecraft's look direction (x, y, z), into Mad Max's axes.
		const Vec3  dirW = MadMax::FromMc(-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch));
		const Vec3  base = MadMax::FromMc(0.0, 0.0, 0.0);
		const float d[3] = { dirW.x - base.x, dirW.y - base.y, dirW.z - base.z };
		const float dl = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1e-6f);
		const float r2[3] = { -d[0] / dl, -d[1] / dl, -d[2] / dl };  // Mad Max's cameras look along -row 2
		float       r1[3] = { -r2[1] * r2[0], 1.0f - r2[1] * r2[1], -r2[1] * r2[2] };  // world up, made orthogonal
		const float ul = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
		for (float& v : r1) {
			v /= std::max(ul, 1e-4f);
		}
		const float r0[3] = { a_handed * (r1[1] * r2[2] - r1[2] * r2[1]), a_handed * (r1[2] * r2[0] - r1[0] * r2[2]), a_handed * (r1[0] * r2[1] - r1[1] * r2[0]) };
		const float m[16] = { r0[0], r0[1], r0[2], 0.0f, r1[0], r1[1], r1[2], 0.0f, r2[0], r2[1], r2[2], 0.0f, a_eye.x, a_eye.y, a_eye.z, 1.0f };
		std::memcpy(a_out, m, sizeof(m));
	}
}
