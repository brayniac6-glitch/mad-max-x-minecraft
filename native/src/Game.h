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

		float yaw{ 0.0f };  // MC degrees, integrated from raw mouse
		float pitch{ 0.0f };
		bool  lookInitialized{ false };
		float sensitivity{ 0.5f };

		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };

		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };

		HWND window{ nullptr };
	};

	Runtime& State();

	namespace Game
	{
		// Once per rendered frame (from Present): link bookkeeping, puppeting, state out to MC.
		void Tick();
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
	}

	namespace Overlay
	{
		// Hooks IDXGISwapChain::Present (found from a throwaway swap chain) with MinHook.
		bool Install();
	}
}
