#include "loader.h"
#include "resource.h"

#include <TlHelp32.h>
#include <winreg.h>
#include <cstring>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")

namespace loader
{

namespace
{

// Generic remote call stub (x86, works for __stdcall and __cdecl targets):
//   void __stdcall stub(call_packet_t* p)
//   { p->function(p->arg1, p->arg2, p->arg3); }
//
//   mov eax, [esp+4]        ; packet
//   push ebp                ; save ebp
//   mov ebp, esp            ; ebp = frame base
//   mov ecx, [eax]          ; function
//   push dword ptr [eax+0Ch]; arg3
//   push dword ptr [eax+8]  ; arg2
//   push dword ptr [eax+4]  ; arg1
//   call ecx
//   mov esp, ebp            ; restore esp (callee popped only its own args)
//   pop ebp
//   ret 4
//
// The callee may pop 0..3 arguments (stdcall) or none (cdecl); restoring ESP
// from EBP makes the stub correct for every case instead of assuming 3.
const unsigned char kCallStub[] = {
	0x8B, 0x44, 0x24, 0x04,   // mov eax,[esp+4]
	0x55,                     // push ebp
	0x89, 0xE5,               // mov ebp,esp
	0x8B, 0x08,               // mov ecx,[eax]
	0xFF, 0x70, 0x0C,         // push [eax+0Ch]
	0xFF, 0x70, 0x08,         // push [eax+8]
	0xFF, 0x70, 0x04,         // push [eax+4]
	0xFF, 0xD1,               // call ecx
	0x89, 0xEC,               // mov esp,ebp
	0x5D,                     // pop ebp
	0xC2, 0x04, 0x00          // ret 4
};

struct call_packet_t
{
	std::uint32_t function;
	std::uint32_t arg1;
	std::uint32_t arg2;
	std::uint32_t arg3;
};

struct remote_ctx_t
{
	HANDLE process = nullptr;
	std::uint32_t stub = 0;
	std::uint32_t packet = 0;
	std::uint32_t scratch = 0;
};

bool remote_call(remote_ctx_t& ctx, std::uint32_t function,
	std::uint32_t arg1, std::uint32_t arg2, std::uint32_t arg3,
	std::uint32_t* result)
{
	if (result)
		*result = 0;

	call_packet_t packet = { function, arg1, arg2, arg3 };
	if (!WriteProcessMemory(ctx.process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.packet)), &packet, sizeof(packet), nullptr))
		return false;

	HANDLE thread = CreateRemoteThread(ctx.process, nullptr, 0,
		reinterpret_cast<LPTHREAD_START_ROUTINE>(static_cast<std::uintptr_t>(ctx.stub)),
		reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.packet)), 0, nullptr);
	if (!thread)
		return false;

	WaitForSingleObject(thread, 30000);

	DWORD exit_code = 0;
	GetExitCodeThread(thread, &exit_code);
	CloseHandle(thread);

	if (result)
		*result = static_cast<std::uint32_t>(exit_code);
	return true;
}

// Copy a NUL terminated string into the remote scratch buffer and return its address.
std::uint32_t push_scratch(remote_ctx_t& ctx, const char* text)
{
	const std::size_t length = std::strlen(text) + 1;
	if (length > 0x800)
		return 0;
	if (!WriteProcessMemory(ctx.process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.scratch)), text, length, nullptr))
		return 0;
	return ctx.scratch;
}

std::uint32_t remote_module_base(DWORD pid, const char* module_name)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);

	std::uint32_t base = 0;
	if (Module32FirstW(snapshot, &entry))
	{
		do
		{
			char name[MAX_MODULE_NAME32 + 1] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, MAX_MODULE_NAME32, nullptr, nullptr);
			if (_stricmp(name, module_name) == 0)
			{
				base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr));
				break;
			}
		} while (Module32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	return base;
}

// 刚启动的进程可能还没把模块表建好(Toolhelp 拿不到), 等待模块就绪
bool wait_for_process_modules(DWORD pid, int timeout_ms)
{
	const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
	do
	{
		if (remote_module_base(pid, "kernel32.dll") != 0)
			return true;
		Sleep(150);
	} while (GetTickCount64() < deadline);
	return false;
}

// Resolve an export address the way it appears inside the target process.
// Works across forwarders (e.g. api-ms-win-crt-* -> ucrtbase) by locating the
// module that actually owns the address and adding the same offset to the
// target's base of that module.
std::uint32_t remote_export(DWORD pid, const char* dll, const char* function)
{
	HMODULE local = LoadLibraryA(dll);
	if (!local)
		return 0;

	FARPROC address = GetProcAddress(local, function);
	if (!address)
		return 0;

	HMODULE owner = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(address), &owner) || !owner)
		return 0;

	char owner_path[MAX_PATH] = {};
	if (!GetModuleFileNameA(owner, owner_path, MAX_PATH))
		return 0;

	const char* owner_name = std::strrchr(owner_path, '\\');
	owner_name = owner_name ? owner_name + 1 : owner_path;

	std::uint32_t remote_owner = remote_module_base(pid, owner_name);
	for (int attempt = 0; attempt < 10 && !remote_owner; ++attempt)
	{
		Sleep(100);
		remote_owner = remote_module_base(pid, owner_name);
	}
	if (!remote_owner)
		return 0;

	const std::uint32_t local_offset = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address))
		- static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(owner));
	return remote_owner + local_offset;
}

void apply_relocations(std::vector<std::uint8_t>& image, std::uint32_t delta)
{
	if (!delta)
		return;

	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	if (!dir.VirtualAddress || !dir.Size)
		return;

	std::size_t offset = 0;
	while (offset + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size)
	{
		auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(image.data() + dir.VirtualAddress + offset);
		if (!block->SizeOfBlock)
			break;

		const std::size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
		auto* entries = reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(block) + sizeof(IMAGE_BASE_RELOCATION));

		for (std::size_t i = 0; i < count; ++i)
		{
			const std::uint16_t type = static_cast<std::uint16_t>(entries[i] >> 12);
			const std::uint16_t page_offset = static_cast<std::uint16_t>(entries[i] & 0x0FFF);
			if (type != IMAGE_REL_BASED_HIGHLOW)
				continue;

			auto* patch = reinterpret_cast<std::uint32_t*>(image.data() + block->VirtualAddress + page_offset);
			*patch += delta;
		}

		offset += block->SizeOfBlock;
	}
}

} // namespace

// 删除上次注入遗留的临时 DLL(游戏退出后文件才会解锁)
void cleanup_stale_temp_dlls()
{
	wchar_t temp_directory[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, temp_directory))
		return;

	std::wstring pattern = std::wstring(temp_directory) + L"gs_*.dll";
	WIN32_FIND_DATAW data = {};
	HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			const std::wstring file = std::wstring(temp_directory) + data.cFileName;
			DeleteFileW(file.c_str());
		}
	} while (FindNextFileW(find, &data));

	FindClose(find);
}

DWORD find_csgo_pid()
{
	DWORD pid = 0;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32W process = {};
	process.dwSize = sizeof(process);

	if (Process32FirstW(snapshot, &process))
	{
		do
		{
			if (_wcsicmp(process.szExeFile, L"csgo.exe") == 0)
			{
				pid = process.th32ProcessID;
				break;
			}
		} while (Process32NextW(snapshot, &process));
	}

	CloseHandle(snapshot);
	return pid;
}


bool module_present(DWORD pid, const char* module_name)
{
	return remote_module_base(pid, module_name) != 0;
}

bool cheat_image_stats(DWORD pid, std::uint64_t* nonzero_bytes)
{
	if (nonzero_bytes)
		*nonzero_bytes = 0;

	HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
	if (!process)
		return false;

	const std::size_t size = 0x2FC000;
	std::vector<std::uint8_t> buffer(size, 0);
	SIZE_T read = 0;
	const bool ok = ReadProcessMemory(process,
		reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(0x43310000)),
		buffer.data(), size, &read) != FALSE;
	CloseHandle(process);

	if (read)
	{
		std::uint64_t nonzero = 0;
		for (std::size_t i = 0; i < read; ++i)
			if (buffer[i])
				++nonzero;
		if (nonzero_bytes)
			*nonzero_bytes = nonzero;
	}

	return ok;
}


namespace
{
std::vector<std::uint8_t> load_rcdata(int id)
{
	std::vector<std::uint8_t> data;

	HMODULE self = GetModuleHandleW(nullptr);
	HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (!resource)
		return data;

	HGLOBAL loaded = LoadResource(self, resource);
	if (!loaded)
		return data;

	const void* pointer = LockResource(loaded);
	const DWORD size = SizeofResource(self, resource);
	if (!pointer || !size)
		return data;

	const auto* bytes = static_cast<const std::uint8_t*>(pointer);
	data.assign(bytes, bytes + size);
	return data;
}
} // namespace


std::vector<std::uint8_t> embedded_dll()
{
	return load_rcdata(IDR_SKEETDLL);
}


std::vector<std::uint8_t> embedded_tr_dll()
{
	return load_rcdata(IDR_SKEETTR);
}


std::vector<std::uint8_t> embedded_tr_map()
{
	return load_rcdata(IDR_TRMAP);
}

static std::wstring steam_exe_path()
{
	wchar_t value[MAX_PATH * 2] = {};
	DWORD size = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe",
		RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0])
		return value;

	const wchar_t* candidates[] = {
		L"C:\\Program Files (x86)\\Steam\\steam.exe",
		L"C:\\Program Files\\Steam\\steam.exe",
		L"D:\\Steam\\steam.exe",
		L"E:\\Steam\\steam.exe",
		L"F:\\Steam\\steam.exe"
	};
	for (const wchar_t* candidate : candidates)
		if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)
			return candidate;

	return std::wstring();
}

namespace
{
bool file_exists_w(const std::wstring& path)
{
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring steam_install_path()
{
	wchar_t value[MAX_PATH * 2] = {};
	DWORD size = sizeof(value);

	if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
		RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0])
		return value;

	size = sizeof(value);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath",
		RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0])
		return value;

	size = sizeof(value);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath",
		RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0])
		return value;

	return std::wstring();
}

// Parse steamapps\libraryfolders.vdf and append every library root.
// Old format:  "1"    "D:\\SteamLibrary"
// New format:  "path" "D:\\SteamLibrary"
void add_library_paths_from_vdf(const std::wstring& vdf_path, std::vector<std::wstring>& libraries)
{
	HANDLE file = CreateFileW(vdf_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER file_size = {};
	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 || file_size.QuadPart > (1 << 20))
	{
		CloseHandle(file);
		return;
	}

	std::string data(static_cast<std::size_t>(file_size.QuadPart), '\0');
	DWORD read = 0;
	ReadFile(file, &data[0], static_cast<DWORD>(data.size()), &read, nullptr);
	CloseHandle(file);
	data.resize(read);

	std::size_t position = 0;
	while (position < data.size())
	{
		std::size_t line_end = data.find('\n', position);
		if (line_end == std::string::npos)
			line_end = data.size();
		const std::string line = data.substr(position, line_end - position);
		position = line_end + 1;

		const std::size_t q1 = line.find('"');
		if (q1 == std::string::npos) continue;
		const std::size_t q2 = line.find('"', q1 + 1);
		if (q2 == std::string::npos) continue;
		const std::string key = line.substr(q1 + 1, q2 - q1 - 1);

		const std::size_t q3 = line.find('"', q2 + 1);
		if (q3 == std::string::npos) continue;
		const std::size_t q4 = line.find('"', q3 + 1);
		if (q4 == std::string::npos) continue;
		const std::string value = line.substr(q3 + 1, q4 - q3 - 1);

		const bool is_path_key = (key == "path");
		const bool is_numeric_key = !key.empty() && key.find_first_not_of("0123456789") == std::string::npos;
		if (!is_path_key && !is_numeric_key)
			continue;

		std::string unescaped;
		unescaped.reserve(value.size());
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
			{
				unescaped += '\\';
				++i;
			}
			else
			{
				unescaped += value[i];
			}
		}

		wchar_t wide[MAX_PATH * 2] = {};
		if (!unescaped.empty() &&
			MultiByteToWideChar(CP_UTF8, 0, unescaped.c_str(), -1, wide, _countof(wide)) > 0)
			libraries.push_back(wide);
	}
}
} // namespace

std::wstring find_csgo_exe()
{
	std::vector<std::wstring> libraries;

	const std::wstring steam = steam_install_path();
	if (!steam.empty())
	{
		libraries.push_back(steam);
		add_library_paths_from_vdf(steam + L"\\steamapps\\libraryfolders.vdf", libraries);
	}

	for (const std::wstring& library : libraries)
	{
		const std::wstring candidate = library + L"\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe";
		if (file_exists_w(candidate))
			return candidate;
	}

	// Already running: take the real process path.
	const DWORD pid = find_csgo_pid();
	if (pid)
	{
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process)
		{
			wchar_t path[MAX_PATH * 2] = {};
			DWORD length = _countof(path);
			const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) && length != 0;
			CloseHandle(process);
			if (ok)
				return path;
		}
	}

	// Last resort: common install locations.
	const wchar_t* fallbacks[] = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe",
		L"C:\\Program Files\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe",
		L"D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe",
		L"E:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe",
		L"F:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\csgo.exe"
	};
	for (const wchar_t* fallback : fallbacks)
		if (GetFileAttributesW(fallback) != INVALID_FILE_ATTRIBUTES)
			return fallback;

	return std::wstring();
}

std::string launch_csgo()
{
	// 直接启动本地的 csgo.exe, 而不是走 Steam(730 现在会被识别为 CS2)
	const std::wstring exe = find_csgo_exe();
	if (exe.empty())
		return "csgo.exe not found";

	const std::size_t slash = exe.find_last_of(L'\\');
	const std::wstring working_directory = slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);

	std::wstring command = L"\"" + exe + L"\" -insecure";
	std::vector<wchar_t> buffer(command.begin(), command.end());
	buffer.push_back(0);

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
		working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process))
		return "failed to start csgo.exe";

	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return std::string();
}

bool wait_for_game_ready(DWORD pid, int timeout_ms)
{
	const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
	while (GetTickCount64() < deadline)
	{
		// 进程已退出则不再等待
		HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (process)
		{
			const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
			CloseHandle(process);
			if (exited)
				return false;
		}

		HWND window = FindWindowW(L"Valve001", nullptr);
		if (window)
		{
			DWORD window_pid = 0;
			GetWindowThreadProcessId(window, &window_pid);
			if (window_pid == pid && IsWindowVisible(window))
				return true;
		}

		Sleep(300);
	}

	return false;
}

std::string reserve_cheat_base(DWORD pid)
{
	HANDLE process = nullptr;
	for (int attempt = 0; attempt < 40 && !process; ++attempt)
	{
		process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
		if (!process)
			Sleep(100);
	}
	if (!process)
		return "OpenProcess failed";

	std::string result;
	LPVOID reserved = VirtualAllocEx(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)), 0x2FC000,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!reserved)
	{
		bool already_injected = false;
		MEMORY_BASIC_INFORMATION info = {};
		if (VirtualQueryEx(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)), &info, sizeof(info)))
			already_injected = (info.State == MEM_COMMIT && info.Protect == PAGE_EXECUTE_READWRITE && info.RegionSize >= 0x2FC000);

		result = already_injected
			? "CSGO already injected - close the game and start again"
			: "0x43310000 is in use - close CSGO and start again";
	}

	CloseHandle(process);
	return result;
}


void release_cheat_base(DWORD pid)
{
	HANDLE process = OpenProcess(PROCESS_VM_OPERATION, FALSE, pid);
	if (!process)
		return;
	VirtualFreeEx(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)), 0, MEM_RELEASE);
	CloseHandle(process);
}


std::string inject_manual_map(DWORD pid, const std::vector<std::uint8_t>& file)
{
	if (file.size() < sizeof(IMAGE_DOS_HEADER))
		return "invalid dll image (too small)";

	auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return "invalid dll image (no MZ)";
	if (dos->e_lfanew <= 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > file.size())
		return "invalid dll image (no PE header)";

	auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return "invalid dll image (bad signature)";
	if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
		return "dll is not 32-bit";

	const std::uint32_t size_of_image = nt->OptionalHeader.SizeOfImage;
	const std::uint32_t size_of_headers = nt->OptionalHeader.SizeOfHeaders;
	const std::uint32_t preferred_base = nt->OptionalHeader.ImageBase;
	const std::uint32_t entry_rva = nt->OptionalHeader.AddressOfEntryPoint;
	if (!size_of_image || size_of_headers > size_of_image)
		return "invalid dll image (bad sizes)";

	HANDLE process = nullptr;
	for (int attempt = 0; attempt < 40 && !process; ++attempt)
	{
		process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
		if (!process)
			Sleep(100);
	}
	if (!process)
		return "OpenProcess failed";

	// 目标可能刚启动, 等模块表就绪后再解析函数地址
	if (!wait_for_process_modules(pid, 20000))
	{
		CloseHandle(process);
		return "target modules not ready (game still starting?)";
	}

	// 1) reserve the fixed cheat image base used by skeet_t::map()
	LPVOID cheat = VirtualAllocEx(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)), 0x2FC000,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!cheat)
		{
			// 可能已由 reserve_cheat_base() 提前预留 -> 视为成功
			bool reserved = false;
			MEMORY_BASIC_INFORMATION info = {};
			if (VirtualQueryEx(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)), &info, sizeof(info)))
				reserved = (info.State == MEM_COMMIT && info.Protect == PAGE_EXECUTE_READWRITE &&
					info.RegionSize >= 0x2FC000 &&
					info.AllocationBase == reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x43310000)));
	
			if (!reserved)
			{
				const DWORD last = GetLastError();
				CloseHandle(process);
				if (last == ERROR_INVALID_ADDRESS || last == ERROR_ACCESS_DENIED)
					return "0x43310000 is in use - close CSGO and start again";
				return "VirtualAllocEx (cheat base) failed";
			}
		}

	// 2) working area: stub + call packet + scratch (all RWX, remote)
	LPVOID work = VirtualAllocEx(process, nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!work)
	{
		CloseHandle(process);
		return "VirtualAllocEx (stub) failed";
	}

	remote_ctx_t ctx;
	ctx.process = process;
	ctx.stub = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(work));
	ctx.packet = ctx.stub + 0x100;
	ctx.scratch = ctx.stub + 0x200;

	if (!WriteProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.stub)), kCallStub, sizeof(kCallStub), nullptr))
	{
		CloseHandle(process);
		return "WriteProcessMemory (stub) failed";
	}

	const std::uint32_t load_library_a = remote_export(pid, "kernel32.dll", "LoadLibraryA");
	const std::uint32_t get_proc_address = remote_export(pid, "kernel32.dll", "GetProcAddress");
	const std::uint32_t tls_alloc = remote_export(pid, "kernel32.dll", "TlsAlloc");
	if (!load_library_a)
	{
		CloseHandle(process);
		return "failed to resolve LoadLibraryA in target";
	}
	if (!get_proc_address)
	{
		CloseHandle(process);
		return "failed to resolve GetProcAddress in target";
	}

	// 3) allocate the image (preferred base first)
	LPVOID image_remote = VirtualAllocEx(process,
		reinterpret_cast<void*>(static_cast<std::uintptr_t>(preferred_base)),
		size_of_image, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!image_remote)
		image_remote = VirtualAllocEx(process, nullptr, size_of_image,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!image_remote)
	{
		CloseHandle(process);
		return "VirtualAllocEx (image) failed";
	}

	const std::uint32_t remote_base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(image_remote));

	// 4) build the image locally
	std::vector<std::uint8_t> image(size_of_image, 0);
	std::memcpy(image.data(), file.data(), size_of_headers);

	const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		const IMAGE_SECTION_HEADER& section = sections[i];
		if (!section.SizeOfRawData || !section.PointerToRawData)
			continue;
		if (static_cast<std::size_t>(section.PointerToRawData) + section.SizeOfRawData > file.size())
			continue;

		std::size_t copy = section.SizeOfRawData;
		if (section.VirtualAddress + copy > size_of_image)
			copy = size_of_image - section.VirtualAddress;
		if (!copy)
			continue;

		std::memcpy(image.data() + section.VirtualAddress, file.data() + section.PointerToRawData, copy);
	}

	if (remote_base != preferred_base)
		apply_relocations(image, remote_base - preferred_base);

	// 5) resolve imports inside the target
	const IMAGE_DATA_DIRECTORY& import_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	std::string failure;
	std::size_t resolved = 0;

	if (import_dir.VirtualAddress && import_dir.Size)
	{
		auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data() + import_dir.VirtualAddress);
		while (descriptor->Name)
		{
			const char* dll_name = reinterpret_cast<const char*>(image.data() + descriptor->Name);

			std::uint32_t module = 0;
			const std::uint32_t name_pointer = push_scratch(ctx, dll_name);
			if (!name_pointer || !remote_call(ctx, load_library_a, name_pointer, 0, 0, &module) || !module || module >= 0xC0000000u)
			{
				failure = std::string("LoadLibraryA failed: ") + dll_name;
				break;
			}

			auto* thunk = reinterpret_cast<std::uint32_t*>(
				image.data() + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
			auto* iat = reinterpret_cast<std::uint32_t*>(image.data() + descriptor->FirstThunk);

			for (std::size_t i = 0; thunk[i]; ++i)
			{
				const std::uint32_t entry = thunk[i];
				std::uint32_t address = 0;

				if (entry & 0x80000000u)
				{
					remote_call(ctx, get_proc_address, module, entry & 0xFFFFu, 0, &address);
				}
				else
				{
					const char* function = reinterpret_cast<const char*>(image.data() + (entry & 0x7FFFFFFFu) + 2);
					const std::uint32_t function_pointer = push_scratch(ctx, function);
					if (function_pointer)
						remote_call(ctx, get_proc_address, module, function_pointer, 0, &address);
				}

				if (!address)
				{
					failure = std::string("GetProcAddress failed in ") + dll_name;
					break;
				}

				iat[i] = address;
				++resolved;
			}

			if (!failure.empty())
				break;

			++descriptor;
		}
	}

	if (!failure.empty())
	{
		CloseHandle(process);
		return failure;
	}

	// 6) upload the finished image
	if (!WriteProcessMemory(process, image_remote, image.data(), size_of_image, nullptr))
	{
		CloseHandle(process);
		return "WriteProcessMemory (image) failed";
	}

	// 7) TLS index (directory exists but has no callbacks)
	const IMAGE_DATA_DIRECTORY& tls_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
	if (tls_dir.VirtualAddress && tls_dir.Size && tls_alloc)
	{
		auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY32*>(image.data() + tls_dir.VirtualAddress);
		if (tls->AddressOfIndex)
		{
			std::uint32_t index = 0;
			if (remote_call(ctx, tls_alloc, 0, 0, 0, &index) && index != 0xFFFFFFFFu)
			{
				const std::uint32_t index_rva = tls->AddressOfIndex - preferred_base;
				WriteProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(remote_base + index_rva)),
					&index, sizeof(index), nullptr);
			}
		}
	}

	// 8) call the entry point: DllMainCRTStartup(hinstDLL, DLL_PROCESS_ATTACH, 0)
	std::uint32_t entry_result = 0;
	if (!remote_call(ctx, remote_base + entry_rva, remote_base, 1, 0, &entry_result))
	{
		CloseHandle(process);
		return "entry point call failed";
	}
	if (!entry_result)
	{
		CloseHandle(process);
		return "DllMain returned FALSE";
	}

	CloseHandle(process);
	return std::string();
}


std::string inject_dll(DWORD pid, const std::vector<std::uint8_t>& image, const wchar_t* tag)
{
	if (image.empty())
		return "empty dll image";

cleanup_stale_temp_dlls();

	wchar_t temp_directory[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, temp_directory))
		return "GetTempPathW failed";

	wchar_t temp_path[MAX_PATH * 2] = {};
	swprintf_s(temp_path, _countof(temp_path), L"%sgs_%s_%lu_%lu.dll", temp_directory,
		(tag && *tag) ? tag : L"dll", (unsigned long)pid, (unsigned long)GetTickCount());

	HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return "failed to create temp dll";

	DWORD written = 0;
	const bool wrote = WriteFile(file, image.data(), (DWORD)image.size(), &written, nullptr) != FALSE;
	CloseHandle(file);
	if (!wrote || written != image.size())
	{
		DeleteFileW(temp_path);
		return "failed to write temp dll";
	}

	HANDLE process = nullptr;
	for (int attempt = 0; attempt < 40 && !process; ++attempt)
	{
		process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
		if (!process)
			Sleep(100);
	}
	if (!process)
	{
		DeleteFileW(temp_path);
		return "OpenProcess failed";
	}

	if (!wait_for_process_modules(pid, 20000))
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "target modules not ready (game still starting?)";
	}

	LPVOID work = VirtualAllocEx(process, nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!work)
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "VirtualAllocEx (stub) failed";
	}

	remote_ctx_t ctx;
	ctx.process = process;
	ctx.stub = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(work));
	ctx.packet = ctx.stub + 0x100;
	ctx.scratch = ctx.stub + 0x200;

	if (!WriteProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.stub)), kCallStub, sizeof(kCallStub), nullptr))
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "WriteProcessMemory (stub) failed";
	}

	const std::uint32_t load_library_w = remote_export(pid, "kernel32.dll", "LoadLibraryW");
	if (!load_library_w)
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "failed to resolve LoadLibraryW in target";
	}

	const std::size_t path_bytes = (std::wcslen(temp_path) + 1) * sizeof(wchar_t);
	if (path_bytes > 0x800)
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "dll path too long";
	}
	if (!WriteProcessMemory(process, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ctx.scratch)), temp_path, path_bytes, nullptr))
	{
		CloseHandle(process);
		DeleteFileW(temp_path);
		return "WriteProcessMemory (path) failed";
	}

	std::uint32_t module = 0;
	const bool called = remote_call(ctx, load_library_w, ctx.scratch, 0, 0, &module);
	CloseHandle(process);

	// 被映射的 DLL 文件在游戏运行期间删不掉, 留给下次启动的 cleanup_stale_temp_dlls()
	DeleteFileW(temp_path);

	if (!called)
		return "CreateRemoteThread (LoadLibraryW) failed";
	if (!module || module >= 0xC0000000u)
		return "LoadLibraryW failed in target";

	return std::string();
}

} // namespace loader
