#pragma once

// Where MadCraft's MadMax.exe addresses are in the running build. MadCraft.ini (and sheet/hooks.tsv)
// give them as the GOG build's (MadMax.exe+<rva>); any other build (Steam) has them elsewhere, so
// every one is found again by its signature (Signatures.inc, tools/make_signatures.py): function
// prologues, the instructions that reference a global, and RTTI for vtables.
namespace madcraft::Addresses
{
	// The GOG build the ini's addresses are from.
	bool IsKnownBuild();
	// Finds every signature in this process's MadMax.exe. On the known build it only checks them
	// (and logs any that disagree); elsewhere the found addresses are used. Waits (up to a minute) for
	// code still encrypted by a DRM wrapper. False if any is missing on an unknown build.
	bool Resolve();
	// A GOG rva -> this build's rva (the same on the GOG build); 0 if it wasn't found.
	std::uintptr_t Translate(std::uintptr_t a_gogRva);
	// "MadMax.exe+1715FB8" (hex, the GOG address) -> its address in this process; 0 if unknown.
	// Other modules ("d3d11.dll+...") are taken as they are.
	std::uintptr_t FromText(const std::string& a_text);
	// What's missing on this build (for the log / message).
	std::string Missing();
}
