#include "Game.h"

#include <MinHook.h>

// Mad Max reads its keyboard and mouse through DirectInput 8, which we are (dinput8.dll). Wrapping
// the devices' GetDeviceState/GetDeviceData gives us exactly what the game sees: we forward it to
// Minecraft and, while Minecraft drives the player, hand the game a blank device instead. This is
// the Mad Max equivalent of SkyCraft's InputSink + PlayerControls hook (MIT, chasmlol).
namespace madcraft
{
	namespace
	{
		// DirectInput scan code -> SDL scancode / USB HID usage (what Minecraft uses). From SkyCraft.
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);
			t[0x0B] = 39;
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);
			t[0x45] = 83, t[0x46] = 71;
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
			return t;
		}();

		constexpr std::uint32_t kDikO = 0x18;   // Minecraft pause / options menu (Esc stays Mad Max's)
		constexpr std::uint32_t kDikF8 = 0x42;  // toggle: Minecraft controls <-> Mad Max controls

		// Keys Mad Max keeps while Minecraft drives (MadCraft.ini [Input] sGameKeys, DIK hex codes).
		std::array<bool, 256> gameKeys = [] {
			std::array<bool, 256> keys{};
			keys[0x01] = true;  // Esc: Mad Max pause menu
			return keys;
		}();

		enum class Kind
		{
			kOther,
			kKeyboard,
			kMouse
		};

		std::mutex                                    devicesLock;
		std::vector<std::pair<void*, Kind>>           devices;
		std::vector<void*>                            bufferedSeen;  // devices the game reads with GetDeviceData
		std::array<std::uint8_t, 256>                 lastKeys{};
		std::array<std::uint8_t, 8>                   lastButtons{};
		std::atomic<float>                            lookDx{ 0.0f }, lookDy{ 0.0f };
		std::atomic<bool>                             f8Down{ false };

		Kind KindOf(void* a_device)
		{
			std::lock_guard g{ devicesLock };
			for (auto& [d, k] : devices) {
				if (d == a_device) {
					return k;
				}
			}
			return Kind::kOther;
		}

		bool ReadsBuffered(void* a_device)
		{
			std::lock_guard g{ devicesLock };
			return std::ranges::find(bufferedSeen, a_device) != bufferedSeen.end();
		}

		// Minecraft gets the input (and Mad Max doesn't) right now.
		bool RouteToMinecraft()
		{
			auto& st = State();
			if (st.gameMenuOpen) {
				return false;
			}
			return st.mcScreenOpen || (st.minecraftOwnsPlayer && !st.madMaxControls);
		}

		void OnKey(std::uint32_t a_dik, bool a_down)
		{
			auto& st = State();
			auto& link = Link::Get();
			if (a_dik == kDikF8) {
				if (a_down && !f8Down) {
					st.madMaxControls = !st.madMaxControls;
					Input::ReleaseAll();
					logger::info("controls: {}", st.madMaxControls ? "Mad Max" : "Minecraft");
				}
				f8Down = a_down;
				return;
			}
			if (!RouteToMinecraft()) {
				return;
			}
			if (!st.mcScreenOpen) {
				if (a_dik == 0x01 && a_down) {
					// Esc opens Mad Max's pause menu, which needs the mouse: Mad Max takes the
					// controls until F8 hands them back.
					st.madMaxControls = true;
					Input::ReleaseAll();
					logger::info("controls: Mad Max (Esc)");
					return;
				}
				if (gameKeys[a_dik & 0xFF]) {
					return;
				}
				if (a_dik == kDikO) {
					if (a_down) {
						Input::ReleaseAll();
						link.PushInput(proto::kInOpenMenu, 0);
					}
					return;
				}
			}
			if (const auto sdl = kDikToSdl[a_dik & 0xFF]) {
				link.PushInput(proto::kInKey, sdl, a_down ? 1 : 0);
			}
		}

		void OnMouse(LONG a_dx, LONG a_dy, LONG a_wheel, const BYTE* a_buttons, int a_count, bool a_forwardButtons)
		{
			auto& st = State();
			auto& link = Link::Get();
			if (!RouteToMinecraft()) {
				std::memcpy(lastButtons.data(), a_buttons, std::min(a_count, 8));
				return;
			}
			if (st.mcScreenOpen) {
				if (a_dx || a_dy) {
					const int x = std::clamp(st.cursorX.load() + int(a_dx), 0, st.viewportW.load() - 1);
					const int y = std::clamp(st.cursorY.load() + int(a_dy), 0, st.viewportH.load() - 1);
					st.cursorX = x;
					st.cursorY = y;
					link.PushInput(proto::kInCursor, 0, x, y);
				}
			} else {
				lookDx = lookDx + float(a_dx);
				lookDy = lookDy + float(a_dy);
			}
			if (a_wheel) {
				link.PushInput(proto::kInScroll, 0, a_wheel > 0 ? 120 : -120);
			}
			if (a_forwardButtons) {
				// DirectInput button 0 left, 1 right, 2 middle -> SDL 1 left, 3 right, 2 middle.
				static constexpr std::uint16_t kSdl[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
				for (int i = 0; i < std::min(a_count, 8); ++i) {
					const bool down = (a_buttons[i] & 0x80) != 0;
					if (down != ((lastButtons[i] & 0x80) != 0) && kSdl[i]) {
						link.PushInput(proto::kInMouseButton, kSdl[i], down ? 1 : 0);
					}
					lastButtons[i] = a_buttons[i];
				}
			}
		}

		// ---- IDirectInputDevice8 hooks (vtable 9: GetDeviceState, 10: GetDeviceData) ----------
		using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
		using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
		GetStateFn origGetStateW = nullptr, origGetStateA = nullptr;
		GetDataFn  origGetDataW = nullptr, origGetDataA = nullptr;

		HRESULT FilterState(void* a_this, DWORD a_size, LPVOID a_data, HRESULT a_hr)
		{
			if (FAILED(a_hr) || !a_data) {
				return a_hr;
			}
			const auto kind = KindOf(a_this);
			const bool buffered = ReadsBuffered(a_this);
			if (kind == Kind::kKeyboard && a_size >= 256) {
				auto* keys = static_cast<std::uint8_t*>(a_data);
				if (!buffered) {
					for (std::uint32_t i = 0; i < 256; ++i) {
						const bool down = (keys[i] & 0x80) != 0;
						if (down != ((lastKeys[i] & 0x80) != 0)) {
							OnKey(i, down);
						}
					}
				}
				std::memcpy(lastKeys.data(), keys, 256);
				if (RouteToMinecraft()) {
					for (std::uint32_t i = 0; i < 256; ++i) {
						if (!gameKeys[i] || State().mcScreenOpen) {
							keys[i] = 0;
						}
					}
				}
			} else if (kind == Kind::kMouse && a_size >= sizeof(DIMOUSESTATE)) {
				auto*     m = static_cast<DIMOUSESTATE*>(a_data);
				const int nButtons = a_size >= sizeof(DIMOUSESTATE2) ? 8 : 4;
				if (!buffered) {
					OnMouse(m->lX, m->lY, m->lZ, m->rgbButtons, nButtons, true);
				}
				if (RouteToMinecraft()) {
					std::memset(a_data, 0, a_size);
				}
			}
			return a_hr;
		}

		HRESULT FilterData(void* a_this, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags, HRESULT a_hr)
		{
			if (FAILED(a_hr) || !a_data || !a_inOut || (a_flags & DIGDD_PEEK)) {
				return a_hr;
			}
			const auto kind = KindOf(a_this);
			if (kind == Kind::kOther) {
				return a_hr;
			}
			{
				std::lock_guard g{ devicesLock };
				if (std::ranges::find(bufferedSeen, a_this) == bufferedSeen.end()) {
					bufferedSeen.push_back(a_this);
				}
			}
			const bool route = RouteToMinecraft();
			DWORD      kept = 0;
			for (DWORD i = 0; i < *a_inOut; ++i) {
				const auto& e = a_data[i];
				bool        keep = !route;
				if (kind == Kind::kKeyboard) {
					OnKey(e.dwOfs, (e.dwData & 0x80) != 0);
					keep = keep || (gameKeys[e.dwOfs & 0xFF] && !State().mcScreenOpen);
				} else {
					const LONG v = static_cast<LONG>(e.dwData);
					if (e.dwOfs == DIMOFS_X) {
						OnMouse(v, 0, 0, lastButtons.data(), 0, false);
					} else if (e.dwOfs == DIMOFS_Y) {
						OnMouse(0, v, 0, lastButtons.data(), 0, false);
					} else if (e.dwOfs == DIMOFS_Z) {
						OnMouse(0, 0, v, lastButtons.data(), 0, false);
					} else if (e.dwOfs >= DIMOFS_BUTTON0 && e.dwOfs <= DIMOFS_BUTTON7) {
						BYTE buttons[8];
						std::memcpy(buttons, lastButtons.data(), 8);
						buttons[e.dwOfs - DIMOFS_BUTTON0] = static_cast<BYTE>(e.dwData & 0x80);
						OnMouse(0, 0, 0, buttons, 8, true);
					}
				}
				if (keep) {
					a_data[kept++] = e;
				}
			}
			*a_inOut = kept;
			return a_hr;
		}

		HRESULT STDMETHODCALLTYPE GetStateW(void* a_this, DWORD a_size, LPVOID a_data) { return FilterState(a_this, a_size, a_data, origGetStateW(a_this, a_size, a_data)); }
		HRESULT STDMETHODCALLTYPE GetStateA(void* a_this, DWORD a_size, LPVOID a_data) { return FilterState(a_this, a_size, a_data, origGetStateA(a_this, a_size, a_data)); }
		HRESULT STDMETHODCALLTYPE GetDataW(void* a_this, DWORD a_size, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags)
		{
			return FilterData(a_this, a_data, a_inOut, a_flags, origGetDataW(a_this, a_size, a_data, a_inOut, a_flags));
		}
		HRESULT STDMETHODCALLTYPE GetDataA(void* a_this, DWORD a_size, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags)
		{
			return FilterData(a_this, a_data, a_inOut, a_flags, origGetDataA(a_this, a_size, a_data, a_inOut, a_flags));
		}

		template <class F>
		void Hook(void* a_device, int a_slot, F a_detour, F* a_original)
		{
			if (*a_original) {
				return;
			}
			auto** vtable = *reinterpret_cast<void***>(a_device);
			if (MH_CreateHook(vtable[a_slot], reinterpret_cast<void*>(a_detour), reinterpret_cast<void**>(a_original)) == MH_OK) {
				MH_EnableHook(vtable[a_slot]);
			}
		}

		void Register(REFGUID a_guid, void* a_device)
		{
			const Kind kind = IsEqualGUID(a_guid, GUID_SysKeyboard) ? Kind::kKeyboard : IsEqualGUID(a_guid, GUID_SysMouse) ? Kind::kMouse : Kind::kOther;
			if (kind == Kind::kOther) {
				return;
			}
			std::lock_guard g{ devicesLock };
			devices.emplace_back(a_device, kind);
			logger::info("input: wrapped DirectInput {}", kind == Kind::kKeyboard ? "keyboard" : "mouse");
		}

		void LoadGameKeys()
		{
			static bool loaded = false;
			if (loaded) {
				return;
			}
			loaded = true;
			const auto list = IniString("Input", "sGameKeys", "");
			if (list.empty()) {
				return;
			}
			gameKeys.fill(false);
			std::size_t start = 0;
			for (std::size_t i = 0; i <= list.size(); ++i) {
				if (i == list.size() || list[i] == ',') {
					try {
						gameKeys[std::stoul(list.substr(start, i - start), nullptr, 16) & 0xFF] = true;
					} catch (...) {
					}
					start = i + 1;
				}
			}
		}
	}

	namespace Input
	{
		void WrapDevice(REFGUID a_guid, IDirectInputDevice8W* a_device)
		{
			LoadGameKeys();
			Register(a_guid, a_device);
			Hook(a_device, 9, &GetStateW, &origGetStateW);
			Hook(a_device, 10, &GetDataW, &origGetDataW);
		}

		void WrapDeviceA(REFGUID a_guid, IDirectInputDevice8A* a_device)
		{
			LoadGameKeys();
			Register(a_guid, a_device);
			Hook(a_device, 9, &GetStateA, &origGetStateA);
			Hook(a_device, 10, &GetDataA, &origGetDataA);
		}

		void ConsumeLook(float& a_dx, float& a_dy)
		{
			a_dx = lookDx.exchange(0.0f);
			a_dy = lookDy.exchange(0.0f);
		}

		void ReleaseAll()
		{
			Link::Get().PushInput(proto::kInReleaseAll, 0);
		}
	}
}
