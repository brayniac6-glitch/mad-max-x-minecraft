#pragma once

#include "MadMax.h"

namespace madcraft
{
	// Shared runtime state between the per-frame update, the input wrapper and the Present hook.
	// Mirrors SkyCraft's Runtime (MIT, chasmlol) minus the Skyrim types.
	struct Runtime
	{
		// Minecraft is connected, in its world, and has acknowledged our last teleport: its player
		// position drives Max.
		std::atomic<bool> puppeting{ false };
		// The same, or waiting for Minecraft to arrive after a teleport: Mad Max's own movement
		// input is withheld either way.
		std::atomic<bool> minecraftOwnsPlayer{ false };
		// Mad Max has the controls: every key goes to the game. Starts on (menus need the mouse) and
		// comes back on Esc and after loads; F8 hands the player to Minecraft.
		std::atomic<bool> madMaxControls{ true };
		// Mad Max has the controls because of a pause, a cutscene, a load or Esc: Minecraft mode comes
		// back on its own when that's over (F8 makes it a manual choice again). escAtMs: when Esc was
		// pressed (its menu should pause the game shortly after).
		std::atomic<bool>          autoControls{ false };
		std::atomic<std::uint64_t> escAtMs{ 0 };
		std::atomic<bool> mcScreenOpen{ false };
		std::atomic<bool> gameMenuOpen{ false };
		std::atomic<bool> mcInWorld{ false };
		// Mad Max's camera sets the look: the mouse keeps turning Mad Max's camera while Minecraft
		// drives, instead of going to Minecraft.
		std::atomic<bool> cameraLook{ false };

		float yaw{ 0.0f };  // MC degrees, integrated from raw mouse
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };
		// Minecraft's overlay size (it may render below Mad Max's resolution); the cursor lives in these.
		std::atomic<int> overlayW{ 0 };
		std::atomic<int> overlayH{ 0 };

		// Where Steve's body is drawn (Minecraft coords): Max's feet this frame. Render thread.
		double bodyX{ 0.0 }, bodyY{ 0.0 }, bodyZ{ 0.0 };
		bool   bodyValid{ false };

		// How lit Mad Max's frame is around the player, as a Minecraft light level 0..1 (SceneLight),
		// and the light the player holds in Minecraft (McState::heldLight).
		std::atomic<float>         shade{ 1.0f };
		std::atomic<std::uint32_t> heldLight{ 0 };

		// Max is in a car (Mad Max drives it with its own controls and camera; Steve sits in the seat),
		// and until when Max is left to Mad Max after the car key (getting in plays its own animation).
		std::atomic<bool>          driving{ false };
		std::atomic<std::uint64_t> interactUntilMs{ 0 };
		// The car key on foot in Minecraft mode hands Max to Mad Max until then (it only lets him get
		// in a car under its own control); back to Minecraft mode once in the car, or at the deadline.
		std::atomic<std::uint64_t> carHandoffUntilMs{ 0 };
		// The interact key (F) is held during the hand-off / when it was let go.
		std::atomic<bool>          interactHeld{ false };
		std::atomic<std::uint64_t> interactReleasedMs{ 0 };
		// Driving camera in Minecraft mode (F5 in a car): Steve's eyes in the seat, or Mad Max's
		// own chase camera. carCam: the first-person one is on now (the mouse looks around the cab).
		std::atomic<bool>          carFirstPerson{ false };
		std::atomic<bool>          carCam{ false };
		// The first-person car eyes (metres above / forward of Max's seat), adjustable in the car with
		// PgUp/PgDn and Home/End, saved to the ini.
		std::atomic<float>         carEyeY{ 1.4f };
		std::atomic<float>         carEyeForward{ 0.4f };

		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };

		HWND window{ nullptr };
	};

	Runtime& State();

	namespace Game
	{
		// Once per rendered frame (from Present): link bookkeeping, puppeting, state out to MC.
		void Tick();
		// On Mad Max's game thread (its keyboard poll, before gameplay runs): moves Max to the pose
		// Tick() published. Game functions are only ever called from here.
		void GameThreadTick();
	}

	namespace Launcher
	{
		void StartMinecraft();
	}

	namespace Input
	{
		// Wraps the DirectInput devices Mad Max creates (keyboard, mouse).
		void WrapDevice(REFGUID a_guid, IDirectInputDevice8W* a_device);
		void WrapDeviceA(REFGUID a_guid, IDirectInputDevice8A* a_device);
		void ConsumeLook(float& a_dx, float& a_dy);
		void ReleaseAll();
		// Diagnostics: what Minecraft has been told is held (SDL codes), and presses since last call.
		std::string DescribeHeld();
	}

	namespace Overlay
	{
		// Hooks IDXGISwapChain::Present (found from a throwaway swap chain) with MinHook.
		bool Install();
	}
}
