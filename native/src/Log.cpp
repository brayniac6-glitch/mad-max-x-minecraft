#include "Log.h"

#include <chrono>
#include <cstdio>

namespace madcraft::log
{
	namespace
	{
		std::mutex lock;
		FILE*      file = nullptr;
	}

	void Open(const std::filesystem::path& a_path)
	{
		std::lock_guard g{ lock };
		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		file = _wfsopen(a_path.c_str(), L"w", _SH_DENYWR);  // others may read it while the game runs
	}

	void Write(const char* a_level, const std::string& a_line)
	{
		std::lock_guard g{ lock };
		if (!file) {
			return;
		}
		SYSTEMTIME t;
		::GetLocalTime(&t);
		std::fprintf(file, "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, a_level, a_line.c_str());
		std::fflush(file);
	}
}
