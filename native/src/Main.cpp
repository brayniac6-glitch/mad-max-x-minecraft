#include "CameraDriver.h"
#include "Game.h"

#include <MinHook.h>

// MadCraft is built as dinput8.dll and dropped next to MadMax.exe. Windows loads it instead of the
// system DirectInput; every export forwards to the real one in System32. DirectInput8Create is the
// one Mad Max calls: we hand back the real interface with its CreateDevice wrapped, so we see the
// keyboard and mouse the game reads (Input.cpp).
namespace
{
	HMODULE realDinput = nullptr;

	template <class F>
	F Real(const char* a_name)
	{
		if (!realDinput) {
			wchar_t sys[MAX_PATH]{};
			::GetSystemDirectoryW(sys, MAX_PATH);
			realDinput = ::LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
		}
		return realDinput ? reinterpret_cast<F>(::GetProcAddress(realDinput, a_name)) : nullptr;
	}

	// IDirectInput8::CreateDevice is vtable slot 3 (after IUnknown's 3). Same slot for A and W.
	using CreateDeviceW = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
	using CreateDeviceA = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
	CreateDeviceW origCreateDeviceW = nullptr;
	CreateDeviceA origCreateDeviceA = nullptr;

	HRESULT STDMETHODCALLTYPE HookCreateDeviceW(IDirectInput8W* a_this, REFGUID a_guid, LPDIRECTINPUTDEVICE8W* a_out, LPUNKNOWN a_outer)
	{
		const auto hr = origCreateDeviceW(a_this, a_guid, a_out, a_outer);
		if (SUCCEEDED(hr) && a_out && *a_out) {
			madcraft::Input::WrapDevice(a_guid, *a_out);
		}
		return hr;
	}

	HRESULT STDMETHODCALLTYPE HookCreateDeviceA(IDirectInput8A* a_this, REFGUID a_guid, LPDIRECTINPUTDEVICE8A* a_out, LPUNKNOWN a_outer)
	{
		const auto hr = origCreateDeviceA(a_this, a_guid, a_out, a_outer);
		if (SUCCEEDED(hr) && a_out && *a_out) {
			madcraft::Input::WrapDeviceA(a_guid, *a_out);
		}
		return hr;
	}

	void HookVtableSlot(void* a_object, int a_slot, void* a_detour, void** a_original)
	{
		auto** vtable = *reinterpret_cast<void***>(a_object);
		if (*a_original) {
			return;  // one vtable per interface implementation: already hooked
		}
		if (MH_CreateHook(vtable[a_slot], a_detour, a_original) == MH_OK) {
			MH_EnableHook(vtable[a_slot]);
		}
	}

	// Runs off the loader lock: waits for the game's window and D3D11 device, then hooks Present.
	void StartupThread()
	{
		madcraft::CameraDriver::Install();
		for (int i = 0; i < 600 && !madcraft::Overlay::Install(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE a_inst, DWORD a_version, REFIID a_iid, LPVOID* a_out, LPUNKNOWN a_outer)
{
	static const auto real = Real<decltype(&DirectInput8Create)>("DirectInput8Create");
	if (!real) {
		return E_FAIL;
	}
	const auto hr = real(a_inst, a_version, a_iid, a_out, a_outer);
	if (SUCCEEDED(hr) && a_out && *a_out) {
		if (IsEqualIID(a_iid, IID_IDirectInput8W)) {
			HookVtableSlot(*a_out, 3, reinterpret_cast<void*>(&HookCreateDeviceW), reinterpret_cast<void**>(&origCreateDeviceW));
		} else if (IsEqualIID(a_iid, IID_IDirectInput8A)) {
			HookVtableSlot(*a_out, 3, reinterpret_cast<void*>(&HookCreateDeviceA), reinterpret_cast<void**>(&origCreateDeviceA));
		}
	}
	return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow()
{
	static const auto real = Real<HRESULT(WINAPI*)()>("DllCanUnloadNow");
	return real ? real() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID a_clsid, REFIID a_iid, LPVOID* a_out)
{
	static const auto real = Real<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
	return real ? real(a_clsid, a_iid, a_out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer()
{
	static const auto real = Real<HRESULT(WINAPI*)()>("DllRegisterServer");
	return real ? real() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer()
{
	static const auto real = Real<HRESULT(WINAPI*)()>("DllUnregisterServer");
	return real ? real() : E_FAIL;
}

BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_module);
		// Only inside Mad Max: other programs in the folder (the GOG uninstaller) get plain DirectInput.
		wchar_t exe[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, exe, MAX_PATH);
		if (_wcsicmp(std::filesystem::path(exe).filename().c_str(), L"MadMax.exe") != 0) {
			return TRUE;
		}
		madcraft::log::Open(madcraft::ModDir() / L"MadCraft.log");
		logger::info("MadCraft 0.1.0 loading");
		if (MH_Initialize() != MH_OK) {
			logger::error("MinHook failed to initialize; MadCraft disabled");
			return TRUE;
		}
		madcraft::MadMax::Init();
		if (!madcraft::Link::Get().Create()) {
			logger::error("MadCraft disabled: could not create shared memory");
			return TRUE;
		}
		// As early as possible: Minecraft takes about as long to start as Mad Max does to reach its menu.
		madcraft::Launcher::StartMinecraft();
		std::thread(StartupThread).detach();
	}
	return TRUE;
}
