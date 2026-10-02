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
		constexpr std::uint32_t kDikF5 = 0x3F;  // in a car: first person <-> Mad Max's chase camera
		bool                    f5Down = false;

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

		// The car key ([Input] sCarKey, default F): in Minecraft mode it presses Mad Max's own
		// get-in / get-out / interact key ([Input] sMadMaxCarKey, default E) for a moment, so cars
		// work without leaving Minecraft mode (Minecraft's E is its inventory).
		std::uint32_t              carKey = 0x21;
		std::uint32_t              madMaxCarKey = 0x12;
		std::uint64_t              carHoldMs = 300;
		std::atomic<std::uint64_t> injectUntilMs{ 0 };
		bool                       carDown = false;
		bool                       injectedDown = false;  // buffered: the press we added is down

		// Pressed for at least carHoldMs, and for as long as the car key is held (Mad Max's get-in key
		// may want holding).
		bool                       carInjectHeld = false;  // this press is being passed to Mad Max
		bool Injecting() { return ::GetTickCount64() < injectUntilMs || carInjectHeld; }

		// In the first-person car camera: PgUp/PgDn raise/lower the eyes, Home/End move them forward/
		// back; saved to the ini's [Vehicle].
		bool AdjustCarEyes(std::uint32_t a_dik)
		{
			auto&       st = State();
			const char* key = nullptr;
			float       value = 0.0f;
			switch (a_dik) {
			case 0xC9:  // PgUp
			case 0xD1:  // PgDn
				value = st.carEyeY + (a_dik == 0xC9 ? 0.1f : -0.1f);
				st.carEyeY = value;
				key = "fFirstPersonEyeY";
				break;
			case 0xC7:  // Home
			case 0xCF:  // End
				value = st.carEyeForward + (a_dik == 0xC7 ? 0.2f : -0.2f);
				st.carEyeForward = value;
				key = "fFirstPersonEyeForward";
				break;
			default:
				return false;
			}
			const auto text = std::format("{:.2f}", value);
			::WritePrivateProfileStringA("Vehicle", key, text.c_str(), (ModDir() / "MadCraft.ini").string().c_str());
			logger::info("vehicle: first-person eyes {} = {}", key, text);
			return true;
		}

		// What Minecraft has been told is held (diagnostics), and how many presses it got.
		std::array<std::atomic<bool>, 256> sentKeys{};
		std::array<std::atomic<bool>, 8>   sentButtons{};
		std::atomic<int>                   presses{ 0 };

		void Send(proto::InputType a_type, std::uint16_t a_code, bool a_down)
		{
			if (a_type == proto::kInKey && a_code < sentKeys.size()) {
				sentKeys[a_code] = a_down;
			} else if (a_type == proto::kInMouseButton && a_code < sentButtons.size()) {
				sentButtons[a_code] = a_down;
			}
			presses += a_down ? 1 : 0;
			Link::Get().PushInput(a_type, a_code, a_down ? 1 : 0);
		}

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
					st.autoControls = false;  // a manual choice
					st.escAtMs = 0;
					Input::ReleaseAll();
					logger::info("controls: {}", st.madMaxControls ? "Mad Max" : "Minecraft");
				}
				f8Down = a_down;
				return;
			}
			if (st.carCam && (a_dik == 0xC9 || a_dik == 0xD1 || a_dik == 0xC7 || a_dik == 0xCF)) {
				if (a_down) {
					AdjustCarEyes(a_dik);
				}
				return;
			}
			// F5 in a car (Minecraft mode): first person in the seat <-> Mad Max's chase camera.
			if (a_dik == kDikF5 && st.driving && !st.madMaxControls) {
				if (a_down && !f5Down) {
					st.carFirstPerson = !st.carFirstPerson;
				}
				f5Down = a_down;
				return;
			}
			if (carKey && a_dik == carKey) {
				// Tracked whoever has the controls: the hand-off keeps going while F is held.
				if (st.interactHeld && !a_down) {
					st.interactReleasedMs = ::GetTickCount64();
				}
				st.interactHeld = a_down;
			}
			if (carKey && a_dik == carKey && a_down && !carDown) {
				logger::info("vehicle: car key pressed (controls {}, Minecraft has the player {}, in a car {}, screen open {}, menu {})",
					st.madMaxControls ? "Mad Max" : "Minecraft", st.minecraftOwnsPlayer.load(), st.driving.load(), st.mcScreenOpen.load(), st.gameMenuOpen.load());
			}
			if (carKey && a_dik == carKey && !st.madMaxControls) {
				if (a_down && !carDown && !st.mcScreenOpen && !st.gameMenuOpen && st.minecraftOwnsPlayer && !st.driving) {
					// On foot: Mad Max only lets Max into a car under its own control, so it gets the
					// controls (and this very press) until he's in, then Minecraft mode comes back.
					st.carHandoffUntilMs = ::GetTickCount64() + 20000;  // ends as soon as the action does
					st.madMaxControls = true;
					Input::ReleaseAll();
					carDown = a_down;
					logger::info("interact: F -> Mad Max does it (car, ladder, door...), Minecraft mode again once it's done");
					return;
				}
				if (a_down && !carDown && !st.mcScreenOpen && !st.gameMenuOpen && (st.minecraftOwnsPlayer || st.driving)) {
					const auto now = ::GetTickCount64();
					injectUntilMs = now + carHoldMs;
					st.interactUntilMs = now + 2500;
					carInjectHeld = true;
					logger::info("vehicle: car key -> Mad Max's key {:02X} ({})", madMaxCarKey, st.driving ? "getting out" : "getting in / interacting");
				}
				carDown = a_down;
				if (!a_down) {
					carInjectHeld = false;
				}
				return;  // not Minecraft's (swap hands) and not Mad Max's own
			}
			if (!RouteToMinecraft()) {
				return;
			}
			if (!st.mcScreenOpen) {
				if (a_dik == 0x01 && a_down) {
					// Esc opens Mad Max's pause menu, which needs the mouse: Mad Max takes the
					// controls, and Minecraft mode comes back when the game unpauses.
					st.madMaxControls = true;
					st.autoControls = true;
					st.escAtMs = ::GetTickCount64();
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
				Send(proto::kInKey, sdl, a_down);
			}
		}

		void OnMouse(LONG a_dx, LONG a_dy, LONG a_wheel, const BYTE* a_buttons, int a_count, bool a_forwardButtons)
		{
			auto& st = State();
			auto& link = Link::Get();
			if (st.carCam) {
				// First person in a car: the mouse looks around the cab (buttons stay Mad Max's).
				lookDx = lookDx + float(a_dx);
				lookDy = lookDy + float(a_dy);
				std::memcpy(lastButtons.data(), a_buttons, std::min(a_count, 8));
				return;
			}
			if (!RouteToMinecraft()) {
				std::memcpy(lastButtons.data(), a_buttons, std::min(a_count, 8));
				return;
			}
			if (st.mcScreenOpen) {
				if (a_dx || a_dy) {
					// The cursor is in Minecraft's overlay pixels (it may render at a fraction of Mad
					// Max's resolution): mouse movement is scaled to match, keeping the fractions.
					const int   ow = st.overlayW > 0 ? st.overlayW.load() : st.viewportW.load();
					const int   oh = st.overlayH > 0 ? st.overlayH.load() : st.viewportH.load();
					const float kx = float(ow) / float(std::max(st.viewportW.load(), 1));
					const float ky = float(oh) / float(std::max(st.viewportH.load(), 1));
					static float fx = 0.0f, fy = 0.0f;
					fx += float(a_dx) * kx;
					fy += float(a_dy) * ky;
					const int mx = int(fx), my = int(fy);
					fx -= float(mx);
					fy -= float(my);
					const int x = std::clamp(st.cursorX.load() + mx, 0, ow - 1);
					const int y = std::clamp(st.cursorY.load() + my, 0, oh - 1);
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
						Send(proto::kInMouseButton, kSdl[i], down);
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
				Game::GameThreadTick();  // the game polls its keyboard on its game thread, once a frame
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
				if (!State().madMaxControls) {
					if (carKey && carKey != madMaxCarKey) {
						keys[carKey] = 0;
					}
					if (State().driving) {
						keys[kDikF5] = 0;
					}
					if (Injecting()) {
						keys[madMaxCarKey] = 0x80;
					}
				}
			} else if (kind == Kind::kMouse && a_size >= sizeof(DIMOUSESTATE)) {
				auto*     m = static_cast<DIMOUSESTATE*>(a_data);
				const int nButtons = a_size >= sizeof(DIMOUSESTATE2) ? 8 : 4;
				if (!buffered) {
					OnMouse(m->lX, m->lY, m->lZ, m->rgbButtons, nButtons, true);
				}
				if (State().carCam) {
					m->lX = 0;  // the cab look has it, not Mad Max's chase camera
					m->lY = 0;
				}
				if (RouteToMinecraft()) {
					// Mad Max's camera sets the look: it keeps the mouse movement, Minecraft keeps
					// the buttons and wheel.
					const LONG keepX = m->lX, keepY = m->lY;
					std::memset(a_data, 0, a_size);
					if (State().cameraLook && !State().mcScreenOpen) {
						m->lX = keepX;
						m->lY = keepY;
					}
				}
			}
			return a_hr;
		}

		HRESULT FilterData(void* a_this, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags, HRESULT a_hr, DWORD a_capacity)
		{
			if (FAILED(a_hr) || !a_data || !a_inOut || (a_flags & DIGDD_PEEK)) {
				return a_hr;
			}
			const auto kind = KindOf(a_this);
			if (kind == Kind::kOther) {
				return a_hr;
			}
			if (kind == Kind::kKeyboard) {
				Game::GameThreadTick();
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
					if (carKey && e.dwOfs == carKey && carKey != madMaxCarKey && !State().madMaxControls) {
						keep = false;
					}
					if (carKey && e.dwOfs == carKey && carKey == madMaxCarKey && State().carHandoffUntilMs != 0) {
						keep = true;  // the press that started the hand-off (routing was decided before it)
					}
					if (e.dwOfs == kDikF5 && State().driving && !State().madMaxControls) {
						keep = false;
					}
				} else {
					const LONG v = static_cast<LONG>(e.dwData);
					if ((e.dwOfs == DIMOFS_X || e.dwOfs == DIMOFS_Y) && State().cameraLook && !State().mcScreenOpen) {
						keep = true;  // Mad Max's camera sets the look (see FilterState)
					}
					if ((e.dwOfs == DIMOFS_X || e.dwOfs == DIMOFS_Y) && State().carCam) {
						keep = false;  // the cab look (see FilterState)
					}
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
			// The car key's press of Mad Max's key, as events for a game reading the buffer.
			if (kind == Kind::kKeyboard && !State().madMaxControls) {
				const bool want = Injecting();
				if (want != injectedDown && kept < a_capacity) {
					DIDEVICEOBJECTDATA ev{};
					ev.dwOfs = madMaxCarKey;
					ev.dwData = want ? 0x80 : 0x00;
					ev.dwTimeStamp = ::GetTickCount();
					ev.dwSequence = kept ? a_data[kept - 1].dwSequence + 1 : 0;
					a_data[kept++] = ev;
					injectedDown = want;
				}
			}
			*a_inOut = kept;
			return a_hr;
		}

		HRESULT STDMETHODCALLTYPE GetStateW(void* a_this, DWORD a_size, LPVOID a_data) { return FilterState(a_this, a_size, a_data, origGetStateW(a_this, a_size, a_data)); }
		HRESULT STDMETHODCALLTYPE GetStateA(void* a_this, DWORD a_size, LPVOID a_data) { return FilterState(a_this, a_size, a_data, origGetStateA(a_this, a_size, a_data)); }
		HRESULT STDMETHODCALLTYPE GetDataW(void* a_this, DWORD a_size, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags)
		{
			const DWORD capacity = a_inOut ? *a_inOut : 0;
			return FilterData(a_this, a_data, a_inOut, a_flags, origGetDataW(a_this, a_size, a_data, a_inOut, a_flags), a_size == sizeof(DIDEVICEOBJECTDATA) ? capacity : 0);
		}
		HRESULT STDMETHODCALLTYPE GetDataA(void* a_this, DWORD a_size, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags)
		{
			const DWORD capacity = a_inOut ? *a_inOut : 0;
			return FilterData(a_this, a_data, a_inOut, a_flags, origGetDataA(a_this, a_size, a_data, a_inOut, a_flags), a_size == sizeof(DIDEVICEOBJECTDATA) ? capacity : 0);
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
			auto hexKey = [](const char* a_key, std::uint32_t a_default) {
				try {
					const auto v = IniString("Input", a_key, "");
					return v.empty() ? a_default : static_cast<std::uint32_t>(std::stoul(v, nullptr, 16) & 0xFF);
				} catch (...) {
					return a_default;
				}
			};
			carKey = hexKey("sCarKey", 0x21);
			madMaxCarKey = hexKey("sMadMaxCarKey", 0x21);
			carHoldMs = static_cast<std::uint64_t>(std::max(50.0, IniDouble("Input", "iCarKeyHoldMs", 300)));
			logger::info("input: car key {:02X} presses Mad Max's {:02X} for {} ms", carKey, madMaxCarKey, carHoldMs);
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
			for (auto& k : sentKeys) {
				k = false;
			}
			for (auto& b : sentButtons) {
				b = false;
			}
			Link::Get().PushInput(proto::kInReleaseAll, 0);
		}

		std::string DescribeHeld()
		{
			std::string s;
			for (std::size_t i = 0; i < sentKeys.size(); ++i) {
				if (sentKeys[i]) {
					s += std::format(" key{}", i);
				}
			}
			for (std::size_t i = 0; i < sentButtons.size(); ++i) {
				if (sentButtons[i]) {
					s += std::format(" mouse{}", i);
				}
			}
			s += std::format(" ({} presses)", presses.exchange(0));
			return s;
		}
	}
}
