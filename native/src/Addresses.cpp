#include "Addresses.h"

#include "MadMax.h"

namespace madcraft::Addresses
{
	namespace
	{
		enum class Kind
		{
			function,
			global,
			vtable
		};

		struct Signature
		{
			Kind          kind;
			std::uintptr_t gogRva;
			const char*   rtti;      // vtable: the class's RTTI type name
			std::uint32_t vtOffset;  // vtable: the subobject offset its locator records
			const char*   pattern;   // hex bytes, ?? = any
			int           disp;      // global: where the RIP-relative displacement sits in the pattern
			int           insnLen;   // global: length of the instruction holding it
		};

		constexpr Signature kSignatures[] = {
#include "Signatures.inc"
		};

		constexpr DWORD kKnownTimestamp = 0x565D5965;  // GOG MadMax.exe (PE TimeDateStamp)
		constexpr DWORD kKnownImageSize = 0x1AAE000;

		std::unordered_map<std::uintptr_t, std::uintptr_t> found;  // GOG rva -> this build's rva
		std::vector<std::string>                           missing;
		int                                                missingRequired = 0;
		bool                                               resolved = false;

		struct Section
		{
			const std::uint8_t* begin = nullptr;
			std::size_t         size = 0;
		};

		std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)); }

		Section FindSection(const char* a_name)
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(Base());
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
				if (std::strncmp(reinterpret_cast<const char*>(sec->Name), a_name, 8) == 0) {
					return { base + sec->VirtualAddress, std::max<std::size_t>(sec->Misc.VirtualSize, 0) };
				}
			}
			return {};
		}

		void Parse(const char* a_text, std::vector<std::uint8_t>& a_bytes, std::vector<bool>& a_mask)
		{
			for (const char* p = a_text; *p;) {
				while (*p == ' ') {
					++p;
				}
				if (!*p) {
					break;
				}
				if (p[0] == '?') {
					a_bytes.push_back(0);
					a_mask.push_back(false);
				} else {
					a_bytes.push_back(static_cast<std::uint8_t>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
					a_mask.push_back(true);
				}
				p += 2;
			}
		}

		// All matches of a masked pattern (stops at 2: only a unique one counts). SEH: a region of
		// .text that's still being decrypted could be unreadable.
		int Scan(const Section& a_sec, const std::vector<std::uint8_t>& a_bytes, const std::vector<bool>& a_mask, const std::uint8_t*& a_match)
		{
			int hits = 0;
			__try {
				const std::size_t n = a_bytes.size();
				for (std::size_t i = 0; i + n <= a_sec.size && hits < 2; ++i) {
					if (a_sec.begin[i] != a_bytes[0]) {
						continue;
					}
					std::size_t k = 1;
					while (k < n && (!a_mask[k] || a_sec.begin[i + k] == a_bytes[k])) {
						++k;
					}
					if (k == n) {
						if (hits++ == 0) {
							a_match = a_sec.begin + i;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
			return hits;
		}

		// A vtable by RTTI: type descriptor (".?AVName@@" in .data) -> complete object locator in
		// .rdata (signature 1, this offset, pointing at itself) -> the vtable right after a pointer to it.
		// Raw scans, SEH-guarded (no C++ objects in them).
		std::uintptr_t FindBytes(const Section& a_sec, const char* a_needle, std::size_t a_len)
		{
			__try {
				for (std::size_t i = 0; i + a_len <= a_sec.size; ++i) {
					if (a_sec.begin[i] == static_cast<std::uint8_t>(a_needle[0]) && std::memcmp(a_sec.begin + i, a_needle, a_len) == 0) {
						return reinterpret_cast<std::uintptr_t>(a_sec.begin + i);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return 0;
		}

		std::uintptr_t FindLocator(const Section& a_rdata, std::uintptr_t a_base, std::uint32_t a_td, std::uint32_t a_offset)
		{
			__try {
				for (std::size_t i = 0; i + 24 <= a_rdata.size; i += 4) {
					const auto* d = reinterpret_cast<const std::uint32_t*>(a_rdata.begin + i);
					const auto  self = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_rdata.begin + i) - a_base);
					if (d[0] == 1 && d[1] == a_offset && d[3] == a_td && d[5] == self) {
						return a_base + self;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return 0;
		}

		std::uintptr_t FindPointerTo(const Section& a_rdata, std::uintptr_t a_value)
		{
			__try {
				for (std::size_t i = 0; i + 16 <= a_rdata.size; i += 8) {
					if (*reinterpret_cast<const std::uint64_t*>(a_rdata.begin + i) == a_value) {
						return reinterpret_cast<std::uintptr_t>(a_rdata.begin + i);
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return 0;
		}

		std::uintptr_t FindVtable(const char* a_name, std::uint32_t a_offset)
		{
			const auto        base = Base();
			const Section     data = FindSection(".data"), rdata = FindSection(".rdata");
			const std::string needle = std::string(a_name) + std::string(1, char(0));
			const auto        name = FindBytes(data, needle.data(), needle.size());
			if (!name) {
				return 0;
			}
			const auto col = FindLocator(rdata, base, static_cast<std::uint32_t>(name - 0x10 - base), a_offset);
			const auto slot = col ? FindPointerTo(rdata, col) : 0;
			return slot ? slot + 8 - base : 0;
		}

		std::uintptr_t FindOne(const Signature& a_sig, const Section& a_text, bool& a_retry)
		{
			if (a_sig.kind == Kind::vtable) {
				return FindVtable(a_sig.rtti, a_sig.vtOffset);
			}
			std::vector<std::uint8_t> bytes;
			std::vector<bool>         mask;
			Parse(a_sig.pattern, bytes, mask);
			const std::uint8_t* match = nullptr;
			const int           hits = Scan(a_text, bytes, mask, match);
			if (hits < 0) {
				a_retry = true;
				return 0;
			}
			if (hits != 1) {
				return 0;
			}
			if (a_sig.kind == Kind::function) {
				return reinterpret_cast<std::uintptr_t>(match) - Base();
			}
			std::int32_t rel = 0;
			std::memcpy(&rel, match + a_sig.disp, sizeof(rel));
			return reinterpret_cast<std::uintptr_t>(match) + a_sig.insnLen + rel - Base();
		}
	}

	bool IsKnownBuild()
	{
		// [Hooks] bForceSignatures = 1: treat this build as unknown and run on the signatures alone
		// (testing what a Steam copy gets, on the GOG one).
		static const bool force = IniBool("Hooks", "bForceSignatures", false);
		if (force) {
			return false;
		}
		const auto* base = reinterpret_cast<const std::uint8_t*>(Base());
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
		return nt->FileHeader.TimeDateStamp == kKnownTimestamp && nt->OptionalHeader.SizeOfImage == kKnownImageSize;
	}

	bool Resolve()
	{
		const bool known = IsKnownBuild();
		const auto text = FindSection(".text");
		// [Combat] sProbeVtables are diagnostics only: not finding them doesn't stop MadCraft.
		std::unordered_set<std::uintptr_t> probes;
		{
			const auto list = IniString("Combat", "sProbeVtables", "");
			for (std::size_t at = list.find('+'); at != std::string::npos; at = list.find('+', at + 1)) {
				try {
					probes.insert(std::stoull(list.substr(at + 1), nullptr, 16));
				} catch (...) {
				}
			}
		}
		// A DRM wrapper (Steam's) decrypts the code after launch: until then nothing matches. Retry.
		for (int attempt = 0; attempt < 120; ++attempt) {
			found.clear();
			missing.clear();
			missingRequired = 0;
			bool retry = false;
			for (const auto& sig : kSignatures) {
				const auto rva = FindOne(sig, text, retry);
				if (rva) {
					found[sig.gogRva] = rva;
				} else {
					const bool probe = probes.contains(sig.gogRva);
					missing.push_back(std::format("{:X}{}{}{}", sig.gogRva, sig.rtti[0] ? " " : "", sig.rtti, probe ? " (diagnostic)" : ""));
					missingRequired += probe ? 0 : 1;
				}
			}
			const bool anyCode = std::ranges::any_of(kSignatures, [](const Signature& s) { return s.kind != Kind::vtable && found.contains(s.gogRva); });
			if (anyCode || known || attempt == 119) {
				break;
			}
			if (attempt == 0) {
				logger::info("addresses: game code not readable yet (DRM still unpacking?); waiting");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		resolved = true;
		if (known) {
			// GOG: the ini's addresses are right as they are; the signatures are checked against them.
			int agree = 0;
			for (const auto& [gog, rva] : found) {
				if (gog == rva) {
					++agree;
				} else {
					logger::warn("addresses: signature for {:X} found {:X} (check tools/make_signatures.py)", gog, rva);
				}
			}
			logger::info("addresses: GOG build; {}/{} signatures agree{}", agree, std::size(kSignatures), missing.empty() ? "" : std::format(", missing: {}", Missing()));
			found.clear();
			return true;
		}
		logger::info("addresses: {} of {} found by signature{}", found.size(), std::size(kSignatures), missing.empty() ? "" : std::format("; missing: {}", Missing()));
		for (const auto& [gog, rva] : found) {
			logger::info("addresses:   {:X} -> {:X}", gog, rva);
		}
		return missingRequired == 0;
	}

	std::uintptr_t Translate(std::uintptr_t a_gogRva)
	{
		if (!resolved || IsKnownBuild()) {
			return a_gogRva;
		}
		const auto it = found.find(a_gogRva);
		return it == found.end() ? 0 : it->second;
	}

	std::uintptr_t FromText(const std::string& a_text)
	{
		const auto plus = a_text.find('+');
		std::string module = plus == std::string::npos ? std::string() : a_text.substr(0, plus);
		module.erase(0, module.find_first_not_of(" \t"));
		std::uintptr_t rva = 0;
		try {
			rva = std::stoull(plus == std::string::npos ? a_text : a_text.substr(plus + 1), nullptr, 16);
		} catch (...) {
			return 0;
		}
		if (module.empty() || _stricmp(module.c_str(), "MadMax.exe") == 0) {
			const auto r = Translate(rva);
			return r ? Base() + r : 0;
		}
		const HMODULE h = ::GetModuleHandleA(module.c_str());
		return h ? reinterpret_cast<std::uintptr_t>(h) + rva : 0;
	}

	std::string Missing()
	{
		std::string s;
		for (const auto& m : missing) {
			s += (s.empty() ? "" : ", ") + m;
		}
		return s;
	}
}
