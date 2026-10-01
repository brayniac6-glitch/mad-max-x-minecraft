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
