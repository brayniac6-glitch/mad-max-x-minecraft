// Ported from SkyCraft (MIT, (c) 2026 chasmlol): skse/src/Launcher.cpp
#include "Game.h"

#include <ExDisp.h>
#include <ShlDisp.h>
#include <ShlObj.h>
#include <servprov.h>
#include <wrl/client.h>

#include <fstream>

// Minecraft starts with Mad Max. The MadCraft Fabric mod then waits on its title screen until Mad
// Max's world is up, hides its own window and opens the MadCraft world by itself, and quits again
// when Mad Max closes. What to start comes from <game>/madcraft/MadCraft.ini; by default Prism
// Launcher's "MadCraft" instance from the bundled MadCraft-Minecraft.zip (Prism signs in to the
// player's Microsoft account; the official launcher can't be started into a profile from outside).
namespace madcraft::Launcher
{
	namespace
	{
		enum class Status
		{
			kOff,
			kRunning,
			kStarting,
			kSignIn,
			kNoLauncher,
			kFailed,
		};
		std::atomic<Status> status{ Status::kOff };

		std::wstring Widen(const std::string& a_utf8)
		{
			if (a_utf8.empty()) {
				return {};
			}
			const int    n = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
			std::wstring out(static_cast<std::size_t>(n), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), out.data(), n);
			return out;
		}

		std::wstring ExpandEnv(const std::wstring& a_path)
		{
			wchar_t     buf[MAX_PATH * 2];
			const DWORD n = ::ExpandEnvironmentStringsW(a_path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
			return n > 0 && n <= std::size(buf) ? std::wstring(buf) : a_path;
		}

		// A Minecraft with the MadCraft mod holds this mutex while it runs (MadLink.announceRunning).
		bool MinecraftRunning()
		{
			HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MadCraft_v1_minecraft");
			if (mutex) {
				::CloseHandle(mutex);
				return true;
			}
			return false;
		}

		// Runs a program the way double-clicking it would: started by the desktop's Explorer, not by
		// Mad Max, so Minecraft isn't a child of the game (GOG Galaxy's overlay and playtime tracking
		// stay on Mad Max alone).
		bool OpenFromDesktop(const std::wstring& a_file, const std::wstring& a_args, const std::wstring& a_dir, int a_show)
		{
			using Microsoft::WRL::ComPtr;
			ComPtr<IShellWindows> windows;
			if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) {
				return false;
			}
			VARIANT location{};
			location.vt = VT_I4;
			location.lVal = CSIDL_DESKTOP;
			VARIANT           empty{};
			long              hwnd = 0;
			ComPtr<IDispatch> desktop;
			if (FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)) || !desktop) {
				return false;
			}
			ComPtr<IServiceProvider>     services;
			ComPtr<IShellBrowser>        browser;
			ComPtr<IShellView>           view;
			ComPtr<IDispatch>            background;
			ComPtr<IShellFolderViewDual> folderView;
			ComPtr<IDispatch>            application;
			ComPtr<IShellDispatch2>      shell;
			if (FAILED(desktop.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
				FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
				FAILED(background.As(&folderView)) || FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell))) {
				return false;
			}
			BSTR    file = ::SysAllocString(a_file.c_str());
			VARIANT args{}, dir{}, operation{}, show{};
			args.vt = dir.vt = operation.vt = VT_BSTR;
			args.bstrVal = ::SysAllocString(a_args.c_str());
			dir.bstrVal = ::SysAllocString(a_dir.c_str());
			operation.bstrVal = ::SysAllocString(L"open");
			show.vt = VT_I4;
			show.lVal = a_show;
			const HRESULT hr = shell->ShellExecute(file, args, dir, operation, show);
			::SysFreeString(file);
			::VariantClear(&args);
			::VariantClear(&dir);
			::VariantClear(&operation);
			return SUCCEEDED(hr);
		}

		std::filesystem::path FindPrism()
		{
			for (const wchar_t* candidate : { L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe", L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe" }) {
				std::filesystem::path p = ExpandEnv(candidate);
				if (std::filesystem::exists(p)) {
					return p;
				}
			}
			return {};
		}

		std::filesystem::path Bundle() { return ModDir() / L"MadCraft-Minecraft.zip"; }
		std::filesystem::path InstallDir() { return ExpandEnv(L"%LOCALAPPDATA%\\MadCraft"); }

		// Unpacks the bundle to %LOCALAPPDATA%\MadCraft the first time, and again whenever this
		// MadCraft brings a different one. Prism's own data there (the signed-in account, downloaded
		// Minecraft and Java, the MadCraft world) is kept; the instance and its MadCraft, Fabric API
		// and e4mc jars are replaced, so both halves always match.
		std::filesystem::path EnsureBundle()
		{
			const auto      bundle = Bundle();
			const auto      dir = InstallDir();
			const auto      prism = dir / "Prism" / "prismlauncher.exe";
			std::error_code ec;
			const auto      stamp = std::format("{} {}", std::filesystem::file_size(bundle, ec),
					 std::filesystem::last_write_time(bundle, ec).time_since_epoch().count());
			std::string     installed;
			if (std::ifstream in{ dir / "bundle.stamp" }; in) {
				std::getline(in, installed);
			}
			if (installed == stamp && std::filesystem::exists(prism)) {
				return prism;
			}
			logger::info("Minecraft: unpacking MadCraft's Minecraft to {}", dir.string());
			std::filesystem::create_directories(dir, ec);
			for (const auto& entry : std::filesystem::directory_iterator(dir / "Prism" / "instances" / "MadCraft" / ".minecraft" / "mods", ec)) {
				const auto name = entry.path().filename().string();
				if (name.starts_with("madcraft-") || name.starts_with("fabric-api-") || name.starts_with("e4mc-")) {
					std::filesystem::remove(entry.path(), ec);
				}
			}
			std::wstring        command = L"\"" + ExpandEnv(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf \"" + bundle.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			DWORD               code = 1;
			if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
				::WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
				::GetExitCodeProcess(pi.hProcess, &code);
				::CloseHandle(pi.hThread);
				::CloseHandle(pi.hProcess);
			}
			if (code != 0 || !std::filesystem::exists(prism)) {
				logger::warn("Minecraft: unpacking failed (tar exit code {})", code);
				return {};
			}
			if (!std::filesystem::exists(dir / "Prism" / "prismlauncher.cfg")) {
				std::filesystem::copy_file(dir / "defaults" / "prismlauncher.cfg", dir / "Prism" / "prismlauncher.cfg", ec);
			}
			std::ofstream(dir / "bundle.stamp") << stamp;
			return prism;
		}

		bool Start(const std::filesystem::path& a_program, const std::wstring& a_args, const std::string& a_shown)
		{
			const auto         ext = a_program.extension().wstring();
			const bool         script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
			const std::wstring dir = a_program.parent_path().wstring();
			const HRESULT      com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool         viaDesktop = OpenFromDesktop(a_program.wstring(), a_args, dir, script ? SW_HIDE : SW_SHOWNORMAL);
			if (SUCCEEDED(com)) {
				::CoUninitialize();
			}
			if (viaDesktop) {
				logger::info("Minecraft: started {}", a_shown);
				return true;
			}
			std::wstring        command = script ? L"cmd.exe /c \"\"" + a_program.wstring() + L"\" " + a_args + L"\"" : L"\"" + a_program.wstring() + L"\" " + a_args;
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi)) {
				logger::warn("Minecraft: couldn't start {} (error {})", a_shown, ::GetLastError());
				return false;
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			logger::info("Minecraft: started {} (directly)", a_shown);
			return true;
		}
	}

	void StartMinecraft()
	{
		if (!IniBool("Minecraft", "bStartWithMadMax", true)) {
			logger::info("Minecraft: not started with Mad Max (bStartWithMadMax = 0)");
			return;
		}
		if (MinecraftRunning()) {
			logger::info("Minecraft: already running");
			status = Status::kRunning;
			return;
		}
		const std::filesystem::path chosen = ExpandEnv(Widen(IniString("Minecraft", "sLauncher", "")));
		const std::string           argsShown = IniString("Minecraft", "sArguments", "--launch MadCraft");
		const std::wstring          args = Widen(argsShown);
		const bool                  bundled = chosen.empty() && std::filesystem::exists(Bundle());
		const std::filesystem::path installed = chosen.empty() && !bundled ? FindPrism() : std::filesystem::path{};
		if (chosen.empty() && !bundled && installed.empty()) {
			logger::warn("Minecraft: not started: no MadCraft-Minecraft.zip in the madcraft folder and no Prism Launcher where its installer puts it; set sLauncher in MadCraft.ini");
			status = Status::kNoLauncher;
			return;
		}
		if (!chosen.empty() && !std::filesystem::exists(chosen)) {
			logger::warn("Minecraft: not started: {} doesn't exist (sLauncher in MadCraft.ini)", chosen.string());
			status = Status::kNoLauncher;
			return;
		}
		status = Status::kStarting;
		// Off the loader-lock thread: unpacking and talking to Explorer take a moment.
		std::thread([chosen, installed, bundled, args, argsShown] {
			std::filesystem::path program = !chosen.empty() ? chosen : installed;
			if (bundled) {
				program = EnsureBundle();
				if (program.empty()) {
					status = Status::kFailed;
					return;
				}
				if (!std::filesystem::exists(program.parent_path() / "accounts.json")) {
					status = Status::kSignIn;  // first time: Prism asks for the Microsoft account
				}
			}
			if (!Start(program, args, program.string() + " " + argsShown)) {
				status = Status::kFailed;
			}
		}).detach();
	}
}
