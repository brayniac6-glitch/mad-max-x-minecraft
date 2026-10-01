#include "CameraDriver.h"

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

		void __fastcall HookUpdate(void* a_camera, void* a_p2, float a_t)
		{
			{
				std::lock_guard g{ lock };
				if (active && reinterpret_cast<std::uintptr_t>(a_camera) == mainCamera.load()) {
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
		const auto plus = text.find('+');
		HMODULE    module = ::GetModuleHandleA(plus == std::string::npos ? nullptr : text.substr(0, plus).c_str());
		if (!module) {
			return false;
		}
		std::uintptr_t rva = 0;
		try {
			rva = std::stoull(plus == std::string::npos ? text : text.substr(plus + 1), nullptr, 16);
		} catch (...) {
			logger::warn("camera driver: can't parse RenderCameraUpdate '{}'", text);
			return false;
		}
		void* target = reinterpret_cast<std::uint8_t*>(module) + rva;
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
	}

	void Release()
	{
		std::lock_guard g{ lock };
		active = false;
	}
}
