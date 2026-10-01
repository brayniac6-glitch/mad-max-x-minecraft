#include "MadMax.h"

namespace madcraft
{
	namespace
	{
		// Two forms (hex numbers):
		//   MadMax.exe+1A2B3C0, 18, 40         pointer chain from a static address
		//   vtable:MadMax.exe+11F2A48, F0, 8    the live object whose vtable is that address (found by
		//                                      scanning the heap), + F0, then dereference + 8 ...
		struct Chain
		{
			std::string                 name;
			std::uintptr_t              base{ 0 };  // module + offset, resolved at Init
			std::vector<std::uintptr_t> offsets;   // applied after each dereference
			bool                        byVtable{ false };
			std::atomic<std::uintptr_t> object{ 0 };  // byVtable: the object found by the last scan
			std::atomic<bool>           scanning{ false };
			std::uint64_t               lastScanMs{ 0 };
			bool                        valid{ false };
			bool                        warned{ false };
		};

		// One qword compare per 8 bytes of committed read-write private memory. Returns every hit
		// (capped): the object's first qword is its vtable pointer. SEH-guarded: the game can free a
		// region while we read it.
		std::size_t ScanRegion(const std::uintptr_t* a_begin, std::size_t a_count, std::uintptr_t a_value, std::uintptr_t* a_hits, std::size_t a_maxHits)
		{
			std::size_t n = 0;
			__try {
				for (std::size_t i = 0; i < a_count && n < a_maxHits; ++i) {
					if (a_begin[i] == a_value) {
						a_hits[n++] = reinterpret_cast<std::uintptr_t>(a_begin + i);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		std::vector<std::uintptr_t> FindObjects(std::uintptr_t a_vtable)
		{
			std::vector<std::uintptr_t> hits(64);
			std::size_t                 found = 0;
			MEMORY_BASIC_INFORMATION    mbi{};
			// Our own stack holds a_vtable (the argument), so it would find itself.
			ULONG_PTR stackLo = 0, stackHi = 0;
			::GetCurrentThreadStackLimits(&stackLo, &stackHi);
			for (std::uintptr_t addr = 0x10000; found < hits.size() && ::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)); addr = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
				const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
				if (base + mbi.RegionSize > stackLo && base < stackHi) {
					continue;
				}
				if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
					found += ScanRegion(static_cast<const std::uintptr_t*>(mbi.BaseAddress), mbi.RegionSize / sizeof(std::uintptr_t), a_vtable, hits.data() + found, hits.size() - found);
				}
			}
			hits.resize(found);
			return hits;
		}

		Chain playerMatrix;       // -> float[16] row-major world matrix (Apex: translation in row 3)
		Chain vehicleFlag;        // -> non-zero byte while Max is in a vehicle
		Chain setTransformIface;  // -> the object whose vtable holds SetTransform(this, const float m[16])
		Chain cameraMatrix;       // -> the render camera's 4x4 world matrix
		Chain timeOfDay;          // -> float: Mad Max's clock
		Chain physicsSystem;      // -> pointer to the CPhysicsSystem (dereferenced once)
		Chain raycastFn;          // function address (no dereference)
		Chain staticFilterCtor;   // function address: CStaticOnlyRaycastFilter(this, mode, 0, 0, 0)

		// FUN_140849E50: (system, tag, ray{origin[3], dir[3]}, minDist, maxDist, result, collector,
		// 0, 0, 0, 0). The collector is a CStaticOnlyRaycastFilter (0x98 bytes, mode 3 as the game's
		// own callers use). Result: +0x08 normal, +0x14 hit fraction between min and max (1 = none).
		using RaycastFn = std::uint8_t(__fastcall*)(void*, const char*, const float*, float, float, void*, void*, void*, char, int, int*);
		using FilterCtorFn = void*(__fastcall*)(void*, int, int, int, void*);

		// The collector: by default CIgnoreCharactersAndVehiclesRaycastFilter, built as the game's own
		// "DebugSpawner" ray builds it (base collector FUN_140808600(buf, 0, 0, 0), then its vftable):
		// everything but people and cars, so ships, wrecks, fences and props are solid too, but never
		// Max himself. Without [Hooks] RaycastFilterBase/Vtable: the static-only filter (terrain, rocks).
		using FilterBaseFn = void*(__fastcall*)(void*, int, int, int);
		FilterBaseFn   filterBase = nullptr;
		std::uintptr_t filterVtable = 0;

		bool CallRaycast(RaycastFn a_fn, FilterCtorFn a_ctor, void* a_sys, const float* a_ray, float a_max, float& a_fraction)
		{
			alignas(16) std::uint8_t filter[0x100]{};
			alignas(16) std::uint8_t result[0x80]{};
			__try {
				if (filterBase && filterVtable) {
					filterBase(filter, 0, 0, 0);
					*reinterpret_cast<std::uintptr_t*>(filter) = filterVtable;
				} else {
					a_ctor(filter, 3, 0, 0, nullptr);
				}
				const auto hit = a_fn(a_sys, "MadCraft", a_ray, 0.0f, a_max, result, filter, nullptr, 0, 0, nullptr);
				a_fraction = *reinterpret_cast<float*>(result + 0x14);
				return (hit & 1) != 0 && a_fraction < 1.0f;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		int   setTransformSlot = -1;

		// The game's SetTransform, SEH-guarded: a wrong slot in the ini must not take the game down.
		bool CallSetTransform(std::uintptr_t a_fn, std::uintptr_t a_this, const float* a_m)
		{
			using Fn = void(__fastcall*)(void*, const float*);
			__try {
				reinterpret_cast<Fn>(a_fn)(reinterpret_cast<void*>(a_this), a_m);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

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
		void ParseChain(Chain& c, const char* a_key)
		{
			c.name = a_key;
			auto text = IniString("Hooks", a_key, "");
			if (text.empty()) {
				return;
			}
			if (text.starts_with("vtable:")) {
				c.byVtable = true;
				text = Trim(text.substr(7));
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
				return;
			}
			try {
				c.base = reinterpret_cast<std::uintptr_t>(module) + std::stoull(plus == std::string::npos ? head : head.substr(plus + 1), nullptr, 16);
				for (std::size_t i = 1; i < parts.size(); ++i) {
					c.offsets.push_back(std::stoull(parts[i], nullptr, 16));
				}
			} catch (...) {
				logger::warn("hook {}: can't parse '{}'", a_key, text);
				return;
			}
			if (c.byVtable && c.offsets.empty()) {
				c.offsets.push_back(0);
			}
			c.valid = true;
			logger::info("hook {} = {}{}", a_key, c.byVtable ? "vtable:" : "", text);
		}

		std::uint64_t NowMs() { return ::GetTickCount64(); }

		// The object a vtable chain starts from: the cached one while it still has that vtable,
		// else a background rescan (at most every 2 s; the heap scan takes a moment).
		bool ChainObject(Chain& a_chain, std::uintptr_t& a_out)
		{
			std::uintptr_t obj = a_chain.object.load();
			std::uintptr_t vt = 0;
			if (obj && SafeRead(obj, &vt, sizeof(vt)) && vt == a_chain.base) {
				a_out = obj;
				return true;
			}
			a_chain.object = 0;
			if (!a_chain.scanning && NowMs() - a_chain.lastScanMs > 2000) {
				a_chain.scanning = true;
				a_chain.lastScanMs = NowMs();
				std::thread([&a_chain] {
					const auto hits = FindObjects(a_chain.base);
					// Several live instances (other characters share the class): the configured
					// offsets must lead somewhere readable; the first that does wins. Ambiguity is
					// logged so the ini can be tightened (e.g. to the player-specific subclass).
					for (const auto hit : hits) {
						std::uintptr_t addr = hit + a_chain.offsets[0], next = 0;
						bool           ok = true;
						for (std::size_t i = 1; i < a_chain.offsets.size() && ok; ++i) {
							ok = SafeRead(addr, &next, sizeof(next)) && next != 0;
							addr = next + a_chain.offsets[i];
						}
						if (ok) {
							a_chain.object = hit;
							break;
						}
					}
					logger::info("hook {}: {} live object(s) with vtable {:X}{}", a_chain.name, hits.size(), a_chain.base, hits.size() > 1 ? " (using the first that resolves)" : "");
					a_chain.scanning = false;
				}).detach();
			}
			return false;
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
			std::size_t    first = 0;
			if (a_chain.byVtable) {
				std::uintptr_t obj = 0;
				if (!ChainObject(a_chain, obj)) {
					return false;
				}
				addr = obj + a_chain.offsets[0];
				first = 1;
			}
			for (std::size_t i = first; i < a_chain.offsets.size(); ++i) {
				std::uintptr_t next = 0;
				if (!SafeRead(addr, &next, sizeof(next)) || next == 0) {
					return false;
				}
				addr = next + a_chain.offsets[i];
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
			ParseChain(playerMatrix, "PlayerMatrix");
			ParseChain(vehicleFlag, "InVehicle");
			ParseChain(setTransformIface, "PlayerSetTransform");
			ParseChain(cameraMatrix, "CameraMatrix");
			ParseChain(timeOfDay, "TimeOfDay");
			ParseChain(physicsSystem, "PhysicsSystem");
			ParseChain(raycastFn, "RaycastFunction");
			ParseChain(staticFilterCtor, "RaycastStaticFilter");
			{
				Chain base, vt;
				ParseChain(base, "RaycastFilterBase");
				ParseChain(vt, "RaycastFilterVtable");
				if (base.valid && vt.valid) {
					filterBase = reinterpret_cast<FilterBaseFn>(base.base);
					filterVtable = vt.base;
					logger::info("collision: rays hit everything but characters and vehicles (ships, wrecks, props)");
				}
			}
			setTransformSlot = static_cast<int>(IniDouble("Hooks", "iSetTransformSlot", -1));
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

		bool SetPlayerPose(const Vec3& a_feet, float a_heading)
		{
			float          m[16];
			std::uintptr_t addr = 0;
			if (!ReadMatrix(m, addr)) {
				return false;
			}
			// Upright rotation about +Y (rows are basis vectors; row 2 forward), then the translation.
			const float c = std::cos(a_heading), s = std::sin(a_heading);
			const float rot[12]{ c, 0, -s, m[3], 0, 1, 0, m[7], s, 0, c, m[11] };
			std::memcpy(m, rot, sizeof(rot));
			m[translationIndex] = a_feet.x;
			m[translationIndex + 1] = a_feet.y;
			m[translationIndex + 2] = a_feet.z;

			std::uintptr_t iface = 0;
			if (setTransformSlot >= 0 && Resolve(setTransformIface, iface)) {
				std::uintptr_t vtbl = 0, fn = 0;
				if (SafeRead(iface, &vtbl, sizeof(vtbl)) && SafeRead(vtbl + sizeof(void*) * setTransformSlot, &fn, sizeof(fn)) && CallSetTransform(fn, iface, m)) {
					return true;
				}
			}
			return SafeWrite(addr, m, sizeof(m));
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

		bool InVehicle()
		{
			// A pointer (Max's attachment parent: the vehicle seat), non-null while attached.
			std::uintptr_t addr = 0;
			std::uintptr_t parent = 0;
			return Resolve(vehicleFlag, addr) && SafeRead(addr, &parent, sizeof(parent)) && parent != 0;
		}

		bool GetCameraMatrix(float (&a_m)[16])
		{
			std::uintptr_t addr = 0;
			if (!Resolve(cameraMatrix, addr) || !SafeRead(addr, a_m, sizeof(a_m))) {
				return false;
			}
			// Sanity: the rotation rows must be unit vectors, else the offset is wrong.
			for (int r = 0; r < 3; ++r) {
				const float len = std::sqrt(a_m[r * 4] * a_m[r * 4] + a_m[r * 4 + 1] * a_m[r * 4 + 1] + a_m[r * 4 + 2] * a_m[r * 4 + 2]);
				if (!(std::fabs(len - 1.0f) < 0.05f)) {
					return false;
				}
			}
			return true;
		}

		bool GetCameraViewProj(float (&a_m)[16])
		{
			std::uintptr_t addr = 0;
			if (!Resolve(cameraMatrix, addr) || !SafeRead(addr + 0x180, a_m, sizeof(a_m))) {
				return false;
			}
			for (const float v : a_m) {
				if (!std::isfinite(v)) {
					return false;
				}
			}
			return a_m[15] != 0.0f || a_m[11] != 0.0f;  // a projection has w from z
		}

		bool GetTimeOfDay(float& a_hours)
		{
			std::uintptr_t addr = 0;
			float          t = 0.0f;
			if (!Resolve(timeOfDay, addr) || !SafeRead(addr, &t, sizeof(t)) || !std::isfinite(t)) {
				return false;
			}
			// Hours 0..24 expected; a 0..1 day fraction is scaled up (logged once to confirm which).
			static bool logged = false;
			if (!std::exchange(logged, true)) {
				logger::info("time of day reads {:.3f}", t);
			}
			a_hours = t <= 1.0001f ? t * 24.0f : std::fmod(t, 24.0f);
			return true;
		}

		std::uintptr_t RenderCameraObject()
		{
			std::uintptr_t addr = 0;
			return Resolve(cameraMatrix, addr) ? addr - 0x54 : 0;  // the chain ends at its world matrix (+0x54)
		}

		bool RaycastAvailable()
		{
			return physicsSystem.valid && raycastFn.valid && staticFilterCtor.valid;
		}

		bool RaycastStatic(const Vec3& a_from, const Vec3& a_to, Vec3& a_hit)
		{
			if (!RaycastAvailable()) {
				return false;
			}
			std::uintptr_t sysSlot = 0, sys = 0;
			if (!Resolve(physicsSystem, sysSlot) || !SafeRead(sysSlot, &sys, sizeof(sys)) || !sys) {
				return false;
			}
			const float dx = a_to.x - a_from.x, dy = a_to.y - a_from.y, dz = a_to.z - a_from.z;
			const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (len < 1e-4f) {
				return false;
			}
			const float ray[6]{ a_from.x, a_from.y, a_from.z, dx / len, dy / len, dz / len };
			float       fraction = 1.0f;
			if (!CallRaycast(reinterpret_cast<RaycastFn>(raycastFn.base), reinterpret_cast<FilterCtorFn>(staticFilterCtor.base),
					reinterpret_cast<void*>(sys), ray, len, fraction)) {
				return false;
			}
			a_hit = { a_from.x + dx * fraction, a_from.y + dy * fraction, a_from.z + dz * fraction };
			return true;
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
