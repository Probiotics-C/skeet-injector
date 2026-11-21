#pragma once
//
// loader.h -- debug.exe loader module (manual map / no file on disk)
//
//  1. Read the embedded skeet.dll (RCDATA) into memory as raw bytes.
//  2. Wait for csgo.exe.
//  3. Manual map the PE image into the target process:
//       - reserve the cheat image base 0x43310000 (0x2FC000, RWX)
//       - allocate + build the image (headers, sections, relocations)
//       - resolve imports with a tiny remote call stub (LoadLibraryA/GetProcAddress)
//       - set up TLS index
//       - call the DLL entry point (DllMainCRTStartup) with DLL_PROCESS_ATTACH
//
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace loader
{
	// Find csgo.exe pid; 0 when not running.
	DWORD find_csgo_pid();

	// Read embedded DLL as raw bytes; empty on failure.
	std::vector<std::uint8_t> embedded_dll();

	// Read the embedded translation DLL (RCDATA 103); empty when the build
	// did not embed one (loader then simply skips the text hook).
	std::vector<std::uint8_t> embedded_tr_dll();

	// Read the embedded translation table (RCDATA 104); empty when absent.
	std::vector<std::uint8_t> embedded_tr_map();

	// Launch CS:GO through Steam with -insecure. Returns empty string on success.
	std::string launch_csgo();

	// Locate csgo.exe: registry -> Steam library folders (libraryfolders.vdf)
	// -> running process -> common fallbacks. Empty when not found.
	std::wstring find_csgo_exe();

	// Early reservation of the fixed cheat image base (0x43310000).
	// Call as soon as csgo.exe exists, before the engine can take that address.
	// Empty string on success.
	std::string reserve_cheat_base(DWORD pid);

	// Block until the target's main game window (class Valve001) is visible.
	// Returns false when the process exits or the timeout elapses.
	bool wait_for_game_ready(DWORD pid, int timeout_ms);

	// Free the cheat base reserved by reserve_cheat_base() (aborted attempt).
	void release_cheat_base(DWORD pid);

	// Manual-map the image into the target. Nothing is written to disk.
	// Returns empty string on success, otherwise an error description.
	std::string inject_manual_map(DWORD pid, const std::vector<std::uint8_t>& image);

	// Write the image to a temporary file, LoadLibraryW() it inside the
	// target (proper TLS / SEH / resource setup), then delete the file.
	// The module stays mapped after the file is removed.
	// `tag` only distinguishes the temp file names of the modules injected
	// in one run (main DLL vs translation DLL), so they cannot collide.
	std::string inject_dll(DWORD pid, const std::vector<std::uint8_t>& image, const wchar_t* tag);

	// Is the module loaded in the target process?
	bool module_present(DWORD pid, const char* module_name);

	// Read back the reserved cheat image (0x43310000, 0x2FC000) and count
	// non-zero bytes; used to verify skeet_t::map() actually ran.
	bool cheat_image_stats(DWORD pid, std::uint64_t* nonzero_bytes);
}
