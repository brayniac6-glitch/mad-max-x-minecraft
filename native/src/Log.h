#pragma once

// Minimal logger with the same call shape SkyCraft uses (logger::info("x {}", y)), writing to
// <game>/madcraft/MadCraft.log. No spdlog: this DLL has no plugin framework to share one with.
namespace madcraft::log
{
	void Open(const std::filesystem::path& a_path);
	void Write(const char* a_level, const std::string& a_line);
}

namespace logger
{
	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		madcraft::log::Write("info", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		madcraft::log::Write("warning", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		madcraft::log::Write("error", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
}
