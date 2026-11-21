// ---------------------------------------------------------------------------
// hook_test.cpp -- 在没有 csgo 的情况下验证 skeet_tr.dll 的挂接逻辑
//
// 做法：本地把 0x43310000 这段地址 VirtualAlloc 出来（和真实镜像同址同大小），
//       伪造三样东西：
//         0x4331E000  写 0x55（DLL 的“镜像就绪”判据）
//         0x4346F46C  虚表[0] = 任意镜像内地址（让 DLL 的就绪检查通过）
//         0x4346F4A8  被挂的槽，先指向下面这个 fake thunk
//       然后 LoadLibraryW("skeet_tr.dll")，等它把槽换掉，再按真实 ABI 调用一次：
//         ecx = this, 栈上 6 个参数，第 5 个是 const wchar_t* text，callee 弹 0x18
//       fake thunk 用 __fastcall 模拟（ecx/edx 走寄存器，6 个栈参数 -> ret 0x18）。
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdio>
#include <cwchar>

static const unsigned kImageBase = 0x43310000u;
static const unsigned kImageSize = 0x2FC000u;
static const unsigned kProbeVa   = 0x4331E000u;
static const unsigned kVtable    = 0x4346F46Cu;
static const unsigned kTextSlot  = kVtable + 0x3Cu;

static const wchar_t* g_seen_text   = nullptr;
static int            g_seen_width  = -1;
static int            g_seen_length = -1;
static int            g_calls       = 0;

// 模拟 loader：把翻译表路径写进命名共享内存
struct tr_share_t
{
	unsigned magic;
	unsigned map_length;
	unsigned log_length;
	wchar_t  map_path[260];
	wchar_t  log_path[260];
};

static void publish_map_path(const wchar_t* path, const wchar_t* log_path)
{
	static HANDLE mapping = nullptr;
	mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		sizeof(tr_share_t), L"Local\\skeet_tr_cfg");
	if (!mapping)
	{
		std::printf("[warn] CreateFileMapping failed: %lu\n", GetLastError());
		return;
	}

	void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(tr_share_t));
	if (!view)
	{
		std::printf("[warn] MapViewOfFile failed: %lu\n", GetLastError());
		return;
	}

	tr_share_t* share = static_cast<tr_share_t*>(view);
	share->magic = 0x52544B53u;
	share->map_length = static_cast<unsigned>(wcslen(path));
	share->log_length = static_cast<unsigned>(wcslen(log_path));
	wcsncpy_s(share->map_path, path, _TRUNCATE);
	wcsncpy_s(share->log_path, log_path, _TRUNCATE);
	UnmapViewOfFile(view);

	std::printf("[ok] map path published: %ls\n", path);
	std::printf("[ok] log path published: %ls\n", log_path);
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* record = info->ExceptionRecord;
	std::printf("[exc] code=0x%08lX address=%p\n",
		static_cast<unsigned long>(record->ExceptionCode), record->ExceptionAddress);
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
	{
		std::printf("[exc] %s 0x%p\n",
			record->ExceptionInformation[0] ? "write" : "read",
			reinterpret_cast<void*>(record->ExceptionInformation[1]));
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// 模拟真实的 0x4339D0B8：ecx=this，栈上 x,y,color,width,text,length 共 6 个 dword
static void __fastcall fake_original(void* /*this_*/, void* /*edx*/,
	int /*x*/, int /*y*/, unsigned /*color*/, int width,
	const wchar_t* text, int length)
{
	++g_calls;
	g_seen_text = text;
	g_seen_width = width;
	g_seen_length = length;
}

typedef void (__fastcall* text_slot_t)(void*, void*, int, int, unsigned, int, const wchar_t*, int);

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	AddVectoredExceptionHandler(1, on_exception);

	// 1) 占住和真实镜像一样的地址
	void* image = VirtualAlloc(reinterpret_cast<void*>(kImageBase), kImageSize,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!image)
	{
		std::printf("[fail] VirtualAlloc(0x%08X) -> %lu\n", kImageBase, GetLastError());
		return 1;
	}
	std::printf("[ok] image reserved at 0x%08X (%u bytes)\n", kImageBase, kImageSize);

	// 2) 伪造“镜像已映射”的特征
	*reinterpret_cast<volatile unsigned char*>(kProbeVa) = 0x55;
	*reinterpret_cast<unsigned*>(kVtable) = kProbeVa;              // 只要落在镜像范围内即可
	*reinterpret_cast<void**>(kTextSlot) = reinterpret_cast<void*>(&fake_original);

	// 3) 造一份翻译表并“发布路径”，验证 DLL 走共享内存能读到
	{
		wchar_t dir[MAX_PATH] = {};
		GetCurrentDirectoryW(MAX_PATH, dir);
		wchar_t map_path[MAX_PATH] = {};
		swprintf_s(map_path, L"%s\\hook_tr.txt", dir);
		wchar_t log_path[MAX_PATH] = {};
		swprintf_s(log_path, L"%s\\hook_tr.log", dir);

		const char text[] = "Off=\xE5\x85\xB3\xE9\x97\xAD\r\n";   // Off=关闭, UTF-8
		HANDLE file = CreateFileW(map_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(file, text, sizeof(text) - 1, &written, nullptr);
			CloseHandle(file);
		}

		publish_map_path(map_path, log_path);
	}

	// 4) 加载被测 DLL（DLL 会自己起线程、等 0x55、替换槽）
	HMODULE tr = LoadLibraryW(L"skeet_tr.dll");
	if (!tr)
	{
		std::printf("[fail] LoadLibraryW(skeet_tr.dll) -> %lu\n", GetLastError());
		return 1;
	}
	std::printf("[ok] skeet_tr.dll loaded at %p\n", tr);

	// 4) 等它挂上（槽不再等于 fake_original）
	void* hooked = nullptr;
	for (int i = 0; i < 100; ++i)
	{
		hooked = *reinterpret_cast<void**>(kTextSlot);
		if (hooked != reinterpret_cast<void*>(&fake_original))
			break;
		Sleep(50);
	}

	if (hooked == reinterpret_cast<void*>(&fake_original))
	{
		std::printf("[fail] slot was not patched (still %p)\n", hooked);
		return 1;
	}
	std::printf("[ok] slot patched: %p -> %p\n",
		reinterpret_cast<void*>(&fake_original), hooked);

	// 5) 按真实 ABI 调用：先调 "Off"（命中翻译表），再调一个不存在的串
	//    第 6 个参数故意传 1（不是 3），用来验证 stub 会把 length 改写成译文长度；
	//    width 传 100，用来验证命中后会被清零（否则渲染器会按 x+width 裁剪）。
	text_slot_t call_slot = *reinterpret_cast<text_slot_t*>(kTextSlot);

	call_slot(reinterpret_cast<void*>(0x11111111), nullptr, 10, 20, 0xFF00FF, 100, L"Off", 1);
	const wchar_t* after_known = g_seen_text;
	const int width_known = g_seen_width;
	const int length_known = g_seen_length;

	call_slot(reinterpret_cast<void*>(0x11111111), nullptr, 10, 20, 0xFF00FF, 100, L"not-in-table", 12);
	const wchar_t* after_unknown = g_seen_text;
	const int width_unknown = g_seen_width;
	const int length_unknown = g_seen_length;

	std::printf("[info] calls=%d\n", g_calls);
	std::printf("[info] 'Off' -> %ls  width=%d length=%d (expect OFF / 0 / 3)\n",
		after_known ? after_known : L"(null)", width_known, length_known);
	std::printf("[info] 'not-in-table' -> %ls  width=%d length=%d (expect unchanged 100 / 12)\n",
		after_unknown ? after_unknown : L"(null)", width_unknown, length_unknown);

	const bool hit = after_known && std::wcscmp(after_known, L"Off") != 0 &&
		width_known == 0 && length_known == static_cast<int>(std::wcslen(after_known));
	const bool pass = after_unknown && std::wcscmp(after_unknown, L"not-in-table") == 0 &&
		width_unknown == 100 && length_unknown == 12;

	std::printf("%s\n", hit ? "[PASS] translation + width/length patch" : "[FAIL] translation patch");
	std::printf("%s\n", pass ? "[PASS] pass-through untouched" : "[FAIL] pass-through broken");

	return (hit && pass) ? 0 : 1;
}
