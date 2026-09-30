#include "MadMax.h"

namespace madcraft
{
	namespace
	{
		struct Chain
		{
			std::string                name;
			std::uintptr_t             base{ 0 };  // module + offset, resolved at Init
			std::vector<std::uintptr_t> offsets;   // applied after each dereference
			bool                       valid{ false };
			bool                       warned{ false };
		};

		Chain playerMatrix;  // -> float[16] row-major world matrix (Apex: translation in row 3)
		Chain vehicleFlag;   // -> non-zero byte while Max is in a vehicle

		double scale = 1.0;  // Mad Max units per Minecraft block
		double signX = 1.0, signZ = 1.0;
		float  yawOffsetDeg = 0.0f;
		int    translationIndex = 12;  // float index of the translation in the matrix

		std::string Trim(std::string a_s)
		{
			const auto b = a_s.find_first_not_of(" \t");
			const auto e = a_s.find_last_not_of(" \t");
			return b == std::string::npos ? std::string{} : a_s.substr(b, e - b + 1);
		}

		// "MadMax.exe+1A2B3C0, 18, 40" (hex, optional 0x).
		Chain ParseChain(const char* a_key)
		{
			Chain c;
			c.name = a_key;
			const auto text = IniString("Hooks", a_key, "");
			if (text.empty()) {
				return c;
			}
			std::vector<std::string> parts;
			std::size_t              start = 0;
			for (std::size_t i = 0; i <= text.size(); ++i) {
				if (i == text.size() || text[i] == ',') {
					parts.push_back(Trim(text.substr(start, i - start)));
					start = i + 1;
				}
			}
			const auto& head = parts.front();
			const auto  plus = head.find('+');
			HMODULE     module = ::GetModuleHandleA(plus == std::string::npos ? nullptr : head.substr(0, plus).c_str());
			if (!module) {
				logger::warn("hook {}: module in '{}' not loaded", a_key, head);
				return c;
			}
			try {
				c.base = reinterpret_cast<std::uintptr_t>(module) + std::stoull(plus == std::string::npos ? head : head.substr(plus + 1), nullptr, 16);
				for (std::size_t i = 1; i < parts.size(); ++i) {
					c.offsets.push_back(std::stoull(parts[i], nullptr, 16));
				}
			} catch (...) {
				logger::warn("hook {}: can't parse '{}'", a_key, text);
				return c;
			}
			c.valid = true;
			logger::info("hook {} = {}", a_key, text);
			return c;
		}

		// Follows the chain to the final address. Every step is checked, so a stale chain just fails.
		bool Resolve(Chain& a_chain, std::uintptr_t& a_out)
		{
			if (!a_chain.valid) {
				if (!a_chain.warned) {
					a_chain.warned = true;
					logger::info("hook {} not configured: that feature stays off", a_chain.name);
				}
				return false;
			}
			std::uintptr_t addr = a_chain.base;
			for (const auto off : a_chain.offsets) {
				std::uintptr_t next = 0;
				if (!SafeRead(addr, &next, sizeof(next)) || next == 0) {
					return false;
				}
				addr = next + off;
			}
			a_out = addr;
			return true;
		}

		bool ReadMatrix(float (&a_m)[16], std::uintptr_t& a_addr)
		{
			return Resolve(playerMatrix, a_addr) && SafeRead(a_addr, a_m, sizeof(a_m));
		}
	}

	bool SafeRead(std::uintptr_t a_addr, void* a_out, std::size_t a_bytes)
	{
		SIZE_T done = 0;
		return a_addr > 0x10000 && ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_addr), a_out, a_bytes, &done) && done == a_bytes;
	}

	bool SafeWrite(std::uintptr_t a_addr, const void* a_in, std::size_t a_bytes)
	{
		SIZE_T done = 0;
		return a_addr > 0x10000 && ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPVOID>(a_addr), a_in, a_bytes, &done) && done == a_bytes;
	}

	std::filesystem::path ModDir()
	{
		wchar_t exe[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, exe, MAX_PATH);
		return std::filesystem::path(exe).parent_path() / L"madcraft";
	}

	std::string IniString(const char* a_section, const char* a_key, const char* a_default)
	{
		static const auto ini = (ModDir() / L"MadCraft.ini").string();
		char              buf[1024]{};
		::GetPrivateProfileStringA(a_section, a_key, a_default, buf, sizeof(buf), ini.c_str());
		std::string s = buf;
		if (const auto semi = s.find(';'); semi != std::string::npos) {
			s = s.substr(0, semi);  // trailing comment
		}
		return Trim(s);
	}

	double IniDouble(const char* a_section, const char* a_key, double a_default)
	{
		const auto s = IniString(a_section, a_key, "");
		try {
			return s.empty() ? a_default : std::stod(s);
		} catch (...) {
			return a_default;
		}
	}

	bool IniBool(const char* a_section, const char* a_key, bool a_default)
	{
		const auto s = IniString(a_section, a_key, a_default ? "1" : "0");
		return s == "1" || s == "true" || s == "True";
	}

	namespace MadMax
	{
		void Init()
		{
			playerMatrix = ParseChain("PlayerMatrix");
			vehicleFlag = ParseChain("InVehicle");
			scale = IniDouble("World", "fUnitsPerBlock", proto::kUnitsPerBlock);
			signX = IniBool("World", "bFlipX", false) ? -1.0 : 1.0;
			signZ = IniBool("World", "bFlipZ", false) ? -1.0 : 1.0;
			yawOffsetDeg = static_cast<float>(IniDouble("World", "fYawOffsetDeg", 0.0));
			translationIndex = static_cast<int>(IniDouble("Hooks", "iMatrixTranslationIndex", 12));
			if (translationIndex < 0 || translationIndex > 13) {
				translationIndex = 12;
			}
			logger::info("world: {} units/block, flipX {}, flipZ {}, yaw offset {}", scale, signX < 0, signZ < 0, yawOffsetDeg);
		}

		bool PlayerAvailable()
		{
			float          m[16];
			std::uintptr_t addr = 0;
			return ReadMatrix(m, addr) && std::isfinite(m[translationIndex]);
		}

		bool GetPlayerFeet(Vec3& a_out)
		{
			float          m[16];
			std::uintptr_t addr = 0;
			if (!ReadMatrix(m, addr)) {
				return false;
			}
			a_out = { m[translationIndex], m[translationIndex + 1], m[translationIndex + 2] };
			return std::isfinite(a_out.x) && std::isfinite(a_out.y) && std::isfinite(a_out.z);
		}

		bool SetPlayerFeet(const Vec3& a_pos)
		{
			float          m[16];
			std::uintptr_t addr = 0;
			if (!ReadMatrix(m, addr)) {
				return false;
			}
			const float t[3]{ a_pos.x, a_pos.y, a_pos.z };
			return SafeWrite(addr + sizeof(float) * translationIndex, t, sizeof(t));
		}

		// Row-major, rows are the basis vectors: row 2 is forward. Heading = atan2(fwd.x, fwd.z).
		bool GetPlayerHeading(float& a_out)
		{
			float          m[16];
			std::uintptr_t addr = 0;
			if (!ReadMatrix(m, addr)) {
				return false;
			}
			a_out = std::atan2(m[8], m[10]);
			return std::isfinite(a_out);
		}

		bool SetPlayerHeading(float a_rad)
		{
			float          m[16];
			std::uintptr_t addr = 0;
			if (!ReadMatrix(m, addr)) {
				return false;
			}
			// Rotation about +Y only (the character stays upright); rows 0..2, translation untouched.
			const float c = std::cos(a_rad), s = std::sin(a_rad);
			const float rot[12]{ c, 0, -s, m[3], 0, 1, 0, m[7], s, 0, c, m[11] };
			return SafeWrite(addr, rot, sizeof(rot));
		}

		bool InVehicle()
		{
			std::uintptr_t addr = 0;
			std::uint8_t   flag = 0;
			return Resolve(vehicleFlag, addr) && SafeRead(addr, &flag, 1) && flag != 0;
		}

		McVec ToMc(const Vec3& a_p)
		{
			return { signX * a_p.x / scale, a_p.y / scale, signZ * a_p.z / scale };
		}

		Vec3 FromMc(double a_x, double a_y, double a_z)
		{
			return { static_cast<float>(signX * a_x * scale), static_cast<float>(a_y * scale), static_cast<float>(signZ * a_z * scale) };
		}

		// Through the forward vector, so any axis flips stay right. Mad Max: heading h faces
		// (sin h, cos h) in XZ. Minecraft: yaw y (degrees) faces (-sin y, cos y).
		float HeadingToMcYaw(float a_rad)
		{
			const double fx = signX * std::sin(a_rad), fz = signZ * std::cos(a_rad);
			return static_cast<float>(std::atan2(-fx, fz) * 57.29577951308232) + yawOffsetDeg;
		}

		float McYawToHeading(float a_deg)
		{
			const double y = (a_deg - yawOffsetDeg) * 0.017453292519943295;
			const double gx = signX * -std::sin(y), gz = signZ * std::cos(y);
			return static_cast<float>(std::atan2(gx, gz));
		}
	}
}
