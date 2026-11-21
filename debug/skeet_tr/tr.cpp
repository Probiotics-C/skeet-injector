// ---------------------------------------------------------------------------
// skeet_tr.cpp -- gamesense 菜单文本翻译模块（测试版，无 CRT 依赖）
//
// 挂接点（静态反汇编 + 运行时 dump 双重确认）：
//
//   renderer 虚表 0x4346F46C，+0x3C = type 10 文字 thunk 0x4339D0B8
//
//   0x4339D0B8 的栈布局（__thiscall，ecx = renderer）：
//       [esp+04] = x         [esp+08] = y       [esp+0C] = color
//       [esp+10] = width     [esp+14] = text    [esp+18] = length(16 位)
//       ret 0x18
//   thunk 内部：ecx = [ecx+4]（core），把 &text 作为第 5 个参数 call 0x4334173D。
//   实现体拿的是 wchar_t**，所以只能替换虚表槽并跳回“槽里原来的值”。
//
// 字体（运行时实测）：
//   core      = *(renderer+4)
//   core+0x104  float scale
//   core+0x108  字体集 A：6 个槽，每槽一个字体对象*（没有链、没有 fallback）
//   core+0x15C  字体集 B（scale != 1.0 时用）
//   字体对象：+0 vtable(0x434722BC)  +4 height  +8 flags  +0x0C face(wchar_t[32])
//   缺字形时渲染器直接跳过该字符 —— 所以换串只能用它已有的字符集。
//
// 本文件不链接 CRT：只用 kernel32，静态缓冲，无异常/无 STL/无堆分配。
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

// ---------------------------------------------------------------- 目标地址
static const unsigned kImageBase = 0x43310000u;
static const unsigned kImageEnd  = 0x43310000u + 0x2FC000u;
static const unsigned kProbeVa   = 0x4331E000u;   // .text 首字节，map() 之后是 0x55
static const unsigned kVtable    = 0x4346F46Cu;   // renderer 虚表
static const unsigned kTextSlot  = kVtable + 0x3Cu;

// loader 通过命名共享内存传来的路径（翻译表 / 日志都放在 debug.exe 同目录）
static wchar_t g_shared_map[MAX_PATH] = {};
static unsigned char g_embedded_map[32768] = {};
static unsigned g_embedded_size = 0;
static wchar_t g_shared_log[MAX_PATH] = {};

// ---------------------------------------------------------------- 翻译表
static const int kMaxPairs = 1024;
static const int kFromMax  = 128;
static const int kToMax    = 192;

struct tr_pair
{
	wchar_t from[kFromMax];
	wchar_t to[kToMax];
};

static tr_pair g_pairs[kMaxPairs];
static int g_pair_count = 0;

static unsigned tr_len(const wchar_t* s)
{
	unsigned n = 0;
	while (s && s[n])
		++n;
	return n;
}

static bool tr_same(const wchar_t* a, const wchar_t* b)
{
	if (!a || !b)
		return false;
	while (*a && *a == *b)
	{
		++a;
		++b;
	}
	return *a == *b;
}

static void tr_copy_wide(wchar_t* dst, int capacity, const wchar_t* src)
{
	int i = 0;
	if (!dst || capacity <= 0)
		return;
	for (; src && src[i] && i + 1 < capacity; ++i)
		dst[i] = src[i];
	dst[i] = 0;
}

static void add_entry(const wchar_t* from, const wchar_t* to)
{
	if (!from || !to || !*from || !*to || g_pair_count >= kMaxPairs)
		return;

	for (int i = 0; i < g_pair_count; ++i)
	{
		if (tr_same(g_pairs[i].from, from))
		{
			tr_copy_wide(g_pairs[i].to, kToMax, to);
			return;
		}
	}

	tr_copy_wide(g_pairs[g_pair_count].from, kFromMax, from);
	tr_copy_wide(g_pairs[g_pair_count].to, kToMax, to);
	++g_pair_count;
}

// 内置探针：现在放中文。配合 font_select 劫持验证 CJK 字形。
static void seed_builtin()
{
	g_pair_count = 0;
	add_entry(L"Off", L"关闭");
}

static void tr_maybe_reload();

// 隔离实验开关：为 false 时 hook 完全不替换文本，只做观测。
// 现在改回 true —— 配合 font_select 劫持，让中文走 slot6(Malgun Gothic)。
static bool g_translate_enabled = true;    // 恢复翻译
static bool g_texture_enabled = true;      // 恢复纹理
static bool g_warm_enabled = true;         // 恢复烤字
static bool g_cjk_ready = false;           // 已一次性预烤完
static bool g_prewarming = false;          // 正在预烤（跳过纹理刷新）

// 翻译表哈希索引：避免每段文本都线性扫 500 条
static const int kHashSize = 2048;
static int g_hash_head[kHashSize];
static int g_hash_next[kMaxPairs];

static unsigned hash_wchars(const wchar_t* s, int len)
{
	unsigned h = 2166136261u;
	for (int i = 0; i < len; ++i)
	{
		h ^= static_cast<unsigned>(static_cast<unsigned short>(s[i]));
		h *= 16777619u;
	}
	return h;
}

static void build_translation_index()
{
	for (int i = 0; i < kHashSize; ++i)
		g_hash_head[i] = -1;
	for (int i = 0; i < g_pair_count; ++i)
	{
		const int len = tr_len(g_pairs[i].from);
		const unsigned h = hash_wchars(g_pairs[i].from, len) & (kHashSize - 1);
		g_hash_next[i] = g_hash_head[h];
		g_hash_head[h] = i;
	}
}

static const wchar_t* find_translation_n(const wchar_t* s, int len)
{
	if (!s || len <= 0 || len >= kFromMax)
		return 0;

	const unsigned h = hash_wchars(s, len) & (kHashSize - 1);
	for (int i = g_hash_head[h]; i >= 0; i = g_hash_next[i])
	{
		const wchar_t* a = g_pairs[i].from;
		int k = 0;
		while (k < len && a[k] && a[k] == s[k])
			++k;
		if (k == len && !a[k])
			return g_pairs[i].to;
	}
	return 0;
}

static bool is_list_sep(wchar_t c)
{
	return c == L',' || c == L'，' || c == L'、' || c == L';' || c == L'|';
}

// 多选下拉框会显示运行时拼接的串（如 "AWP, SSG 08"）。
// 整串查不到时，按列表分隔符拆开，逐个选项翻译后按原分隔符拼回去。
static wchar_t g_composite[1024];

// 供 stub 调用：命中返回译文指针，未命中返回 0
extern "C" const wchar_t* __cdecl tr_lookup(const wchar_t* text)
{
	if (!text)
		return 0;

	tr_maybe_reload();

	if (!g_translate_enabled)
		return 0;

	const unsigned length = tr_len(text);
	const wchar_t* exact = find_translation_n(text, static_cast<int>(length));
	if (exact)
		return exact;

	bool has_sep = false;
	for (int i = 0; text[i]; ++i)
		if (is_list_sep(text[i]))
		{
			has_sep = true;
			break;
		}
	if (!has_sep)
		return 0;

	const int cap = static_cast<int>(sizeof(g_composite) / sizeof(g_composite[0])) - 1;
	int out = 0;
	int i = 0;
	bool translated = false;

	while (text[i] && out < cap)
	{
		const int seg_start = i;
		while (text[i] && !is_list_sep(text[i]))
			++i;
		const int seg_end = i;

		int ls = seg_start;
		while (ls < seg_end && (text[ls] == L' ' || text[ls] == L'\t'))
			++ls;
		int le = seg_end;
		while (le > ls && (text[le - 1] == L' ' || text[le - 1] == L'\t'))
			--le;

		for (int k = seg_start; k < ls && out < cap; ++k)
			g_composite[out++] = text[k];

		if (le > ls)
		{
			const wchar_t* tr = find_translation_n(text + ls, le - ls);
			if (tr)
			{
				translated = true;
				for (int k = 0; tr[k] && out < cap; ++k)
					g_composite[out++] = tr[k];
			}
			else
			{
				for (int k = ls; k < le && out < cap; ++k)
					g_composite[out++] = text[k];
			}
		}

		for (int k = le; k < seg_end && out < cap; ++k)
			g_composite[out++] = text[k];

		if (text[i] && out < cap)
			g_composite[out++] = text[i++];
	}

	g_composite[out] = 0;
	return translated ? g_composite : 0;
}

// 供 stub 调用：译文长度（type 10 第 6 个参数是字符数，必须一起改）
extern "C" unsigned __cdecl tr_length(const wchar_t* text)
{
	return tr_len(text);
}

static void* g_tr_orig = 0;
static bool g_add_cjk = false;
static bool g_cjk_added = false;

extern "C" void __cdecl tr_draw_log(unsigned x, unsigned y, unsigned width, unsigned length,
	unsigned new_length, const wchar_t* text, unsigned renderer, unsigned flags);
extern "C" void __cdecl tr_note(unsigned renderer, unsigned flags, const wchar_t* text);
extern "C" void __cdecl tr_check_cjk(const wchar_t* text);

// 裸函数：改 type 10 的三个参数后跳回原 thunk
//   pushad 保存顺序 EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI -> 内存里
//     +0x00 EDI  +0x04 ESI  +0x08 EBP  +0x0C ESP  +0x10 EBX  +0x14 EDX  +0x18 ECX  +0x1C EAX
//   pushad 之后整体 +0x20：+0x24 x  +0x28 y  +0x2C color  +0x30 width  +0x34 text  +0x38 length
extern "C" __declspec(naked) void tr_text_hook()
{
	__asm
	{
		pushad
		mov  ebx, esp

		// cdecl：参数从右往左压 -> 先 text，再 flags，最后 renderer
		push dword ptr [ebx + 0x34]         // text
		push dword ptr [ebx + 0x14]         // edx = style flags
		push dword ptr [ebx + 0x18]         // ecx = renderer
		call tr_note
		add  esp, 12

		mov  ebp, [ebx + 0x34]              // 原始 text
		push dword ptr [ebx + 0x34]
		call tr_lookup
		add  esp, 4
		test eax, eax
		jz   tr_no_match

		mov  [ebx + 0x34], eax              // text = 译文
		push eax
		call tr_check_cjk                   // 译文含非 ASCII 时给 font_select 打标记
		add  esp, 4

		push dword ptr [ebx + 0x34]
		call tr_length
		add  esp, 4
		mov  esi, [ebx + 0x38]              // 原始 length
		mov  edi, [ebx + 0x30]              // 原始 width
		mov  [ebx + 0x38], eax              // length = 译文长度
		// width 保持原值：之前置 0 可能被当成“裁剪到 0”，导致顶点不生成

		push dword ptr [ebx + 0x14]         // edx = style flags
		push dword ptr [ebx + 0x18]         // ecx = renderer
		push ebp                            // 原文
		push eax                            // 译文长度
		push esi                            // 原始 length
		push edi                            // 原始 width
		push dword ptr [ebx + 0x28]
		push dword ptr [ebx + 0x24]
		call tr_draw_log
		add  esp, 32

	tr_no_match:
		popad
		jmp  dword ptr [g_tr_orig]
	}
}

// ---------------------------------------------------------------- 日志
// 关闭日志：不再创建 skeet_tr.log / 崩溃日志 / minidump。
static const bool kEnableLog = false;

static wchar_t g_log_path[MAX_PATH] = {};
static HANDLE g_log_file = INVALID_HANDLE_VALUE;

static char* log_append(char* p, const char* text)
{
	if (!text)
		return p;
	while (*text)
		*p++ = *text++;
	return p;
}

static char* log_append_hex(char* p, unsigned value)
{
	static const char* digits = "0123456789ABCDEF";
	*p++ = '0';
	*p++ = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		*p++ = digits[(value >> shift) & 0xF];
	return p;
}

static char* log_append_dec(char* p, unsigned value)
{
	char temp[12];
	int n = 0;
	if (!value)
		temp[n++] = '0';
	while (value)
	{
		temp[n++] = static_cast<char>('0' + (value % 10));
		value /= 10;
	}
	while (n)
		*p++ = temp[--n];
	return p;
}

static void log_line(char* begin, char* end)
{
	*end++ = '\r';
	*end++ = '\n';

	if (g_log_file == INVALID_HANDLE_VALUE && g_log_path[0])
	{
		g_log_file = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
		if (g_log_file != INVALID_HANDLE_VALUE)
			SetFilePointer(g_log_file, 0, 0, FILE_END);
	}

	if (g_log_file == INVALID_HANDLE_VALUE)
		return;

	DWORD written = 0;
	WriteFile(g_log_file, begin, static_cast<DWORD>(end - begin), &written, 0);
}

static void log_text(const char* text)
{
	char buffer[256];
	char* p = log_append(buffer, text);
	log_line(buffer, p);
}

static void log_pair(const char* label, unsigned a, unsigned b)
{
	char buffer[256];
	char* p = log_append(buffer, label);
	p = log_append_hex(p, a);
	p = log_append(p, " -> ");
	p = log_append_hex(p, b);
	log_line(buffer, p);
}

static void build_temp_log_path()
{
	wchar_t temp[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, temp))
		return;

	int i = 0;
	for (const wchar_t* s = temp; *s && i + 16 < MAX_PATH; ++s)
		g_log_path[i++] = *s;
	const wchar_t* name = L"skeet_tr.log";
	for (const wchar_t* s = name; *s && i + 1 < MAX_PATH; ++s)
		g_log_path[i++] = *s;
	g_log_path[i] = 0;
}

static void open_log()
{
	if (!kEnableLog)
		return;

	g_log_path[0] = 0;

	if (g_shared_log[0])
	{
		int i = 0;
		for (; g_shared_log[i] && i + 1 < MAX_PATH; ++i)
			g_log_path[i] = g_shared_log[i];
		g_log_path[i] = 0;
	}
	else
	{
		build_temp_log_path();
	}

	if (!g_log_path[0])
		return;

	g_log_file = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

	if (g_log_file == INVALID_HANDLE_VALUE && g_shared_log[0])
	{
		build_temp_log_path();
		g_log_file = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	}
}

// ---------------------------------------------------------------- 内存探测
static bool page_readable(const void* address, unsigned size)
{
	MEMORY_BASIC_INFORMATION info = {};
	if (!VirtualQuery(address, &info, sizeof(info)))
		return false;

	if (info.State != MEM_COMMIT)
		return false;

	if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return false;

	const unsigned base = reinterpret_cast<unsigned>(address);
	const unsigned end = base + size;
	const unsigned region_end = reinterpret_cast<unsigned>(info.BaseAddress) +
		static_cast<unsigned>(info.RegionSize);
	return end <= region_end;
}

// ---------------------------------------------------------------- 崩溃捕获
// MSVC/Clang 的 ASAN 不支持 32 位 x86，注入进程里也无法启用 ASAN 运行时。
// 这里用 VEH 捕获崩溃：异常码/地址/寄存器/栈写进日志，并生成 minidump。
typedef BOOL (WINAPI* minidump_write_fn)(HANDLE, DWORD, HANDLE, int, const void*, const void*, const void*);

static minidump_write_fn g_minidump_write = 0;
static wchar_t g_dump_path[MAX_PATH] = {};
static volatile LONG g_crash_busy = 0;

static void build_dump_path()
{
	int i = 0;
	for (; g_log_path[i] && i + 5 < MAX_PATH; ++i)
		g_dump_path[i] = g_log_path[i];
	const wchar_t suffix[] = L".dmp";
	for (int k = 0; suffix[k] && i + 1 < MAX_PATH; ++k)
		g_dump_path[i++] = suffix[k];
	g_dump_path[i] = 0;
}

static void crash_write(const char* text, unsigned length)
{
	if (g_log_file == INVALID_HANDLE_VALUE || !text || !length)
		return;
	DWORD written = 0;
	WriteFile(g_log_file, text, length, &written, 0);
	FlushFileBuffers(g_log_file);
}

static LONG CALLBACK tr_crash_handler(PEXCEPTION_POINTERS info)
{
	const DWORD code = info->ExceptionRecord->ExceptionCode;

	// 忽略调试输出等 0x4001xxxx 信息类异常（不是崩溃）
	if ((code & 0xFFFF0000u) == 0x40010000u)
		return EXCEPTION_CONTINUE_SEARCH;

	// 只记录真正的致命异常
	const bool fatal =
		code == 0xC0000005u ||   // access violation
		code == 0xC000001Du ||   // illegal instruction
		code == 0xC0000094u ||   // int divide by zero
		code == 0xC0000096u ||   // privileged instruction
		code == 0xC00000FDu ||   // stack overflow
		code == 0xC0000409u;     // stack buffer overrun / fast fail
	if (!fatal)
		return EXCEPTION_CONTINUE_SEARCH;

	if (InterlockedExchange(&g_crash_busy, 1) != 0)
		return EXCEPTION_CONTINUE_SEARCH;

	const EXCEPTION_RECORD* er = info->ExceptionRecord;
	const CONTEXT* c = info->ContextRecord;

	char buffer[1400];
	char* p = buffer;

	p = log_append(p, "\r\n=== CRASH ===\r\ncode=");
	p = log_append_hex(p, er->ExceptionCode);
	p = log_append(p, " addr=");
	p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(er->ExceptionAddress)));
	p = log_append(p, "\r\neip=");
	p = log_append_hex(p, c->Eip);
	p = log_append(p, " esp=");
	p = log_append_hex(p, c->Esp);
	p = log_append(p, " ebp=");
	p = log_append_hex(p, c->Ebp);
	p = log_append(p, " eax=");
	p = log_append_hex(p, c->Eax);
	p = log_append(p, " ebx=");
	p = log_append_hex(p, c->Ebx);
	p = log_append(p, " ecx=");
	p = log_append_hex(p, c->Ecx);
	p = log_append(p, " edx=");
	p = log_append_hex(p, c->Edx);
	p = log_append(p, " esi=");
	p = log_append_hex(p, c->Esi);
	p = log_append(p, " edi=");
	p = log_append_hex(p, c->Edi);

	p = log_append(p, "\r\nstack:");
	const unsigned* sp = reinterpret_cast<const unsigned*>(c->Esp);
	for (int i = 0; i < 64 && p < buffer + 1250; ++i)
	{
		if (!page_readable(reinterpret_cast<const void*>(sp + i), 4))
			break;
		const unsigned v = sp[i];
		if ((v >= kImageBase && v < kImageEnd) ||
			(v >= reinterpret_cast<unsigned>(&tr_crash_handler) - 0x200000 &&
			 v < reinterpret_cast<unsigned>(&tr_crash_handler) + 0x200000))
		{
			p = log_append(p, " ");
			p = log_append_hex(p, v);
		}
	}
	p = log_append(p, "\r\n");
	crash_write(buffer, static_cast<unsigned>(p - buffer));

	if (g_minidump_write && g_dump_path[0])
	{
		HANDLE file = CreateFileW(g_dump_path, GENERIC_WRITE, 0, 0,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
		if (file != INVALID_HANDLE_VALUE)
		{
			const BOOL ok = g_minidump_write(GetCurrentProcess(), GetCurrentProcessId(),
				file, 0 /*MiniDumpNormal*/, info, 0, 0);
			const DWORD gle = ok ? 0 : GetLastError();
			CloseHandle(file);

			char line[80];
			char* q = log_append(line, "minidump ok=");
			q = log_append_dec(q, ok ? 1u : 0u);
			q = log_append(q, " gle=");
			q = log_append_hex(q, gle);
			q = log_append(q, "\r\n");
			crash_write(line, static_cast<unsigned>(q - line));
		}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_handler()
{
	if (!kEnableLog)
		return;

	build_dump_path();

	HMODULE dbg = LoadLibraryA("dbghelp.dll");
	if (dbg)
		g_minidump_write = reinterpret_cast<minidump_write_fn>(
			GetProcAddress(dbg, "MiniDumpWriteDump"));

	HMODULE k = GetModuleHandleA("kernel32.dll");
	typedef PVOID (WINAPI* add_veh_fn)(ULONG, PVECTORED_EXCEPTION_HANDLER);
	add_veh_fn add = k
		? reinterpret_cast<add_veh_fn>(GetProcAddress(k, "AddVectoredExceptionHandler")) : 0;
	if (add)
		add(1, tr_crash_handler);
}

// ---------------------------------------------------------------- 字体诊断
// 渲染核心 core = *(renderer+4)；字体集 A @ core+0x108，B @ core+0x15C。
// 每个槽是一个字体对象（没有 fallback 链）：
//   +0 vtable  +4 height  +8 flags  +0x0C face(wchar_t[32])
static unsigned g_renderer = 0;
static bool g_fonts_dumped = false;

static int append_face(char* p, unsigned object)
{
	const wchar_t* face = reinterpret_cast<const wchar_t*>(object + 0x0C);
	int n = 0;
	for (; n < 30 && face[n]; ++n)
		p[n] = (face[n] >= 0x20 && face[n] < 0x7F) ? static_cast<char>(face[n]) : '.';
	return n;
}

static void log_font_set(const char* label, unsigned base)
{
	// 已知 6 个样式槽；+0x48 / +0x54 可能是额外字体（CJK？），一起打出来
	static const unsigned slots[] = { 0x00, 0x0C, 0x18, 0x24, 0x30, 0x3C, 0x48, 0x54 };

	for (int i = 0; i < 8; ++i)
	{
		const unsigned slot = base + slots[i];
		if (!page_readable(reinterpret_cast<const void*>(slot), 4))
			continue;

		const unsigned object = *reinterpret_cast<unsigned*>(slot);

		char buffer[256];
		char* p = log_append(buffer, label);
		p = log_append(p, " slot");
		p = log_append_dec(p, static_cast<unsigned>(i));
		p = log_append(p, " obj=");
		p = log_append_hex(p, object);

		if (object && page_readable(reinterpret_cast<const void*>(object), 0x90))
		{
			p = log_append(p, " vt=");
			p = log_append_hex(p, *reinterpret_cast<unsigned*>(object));
			p = log_append(p, " h=");
			p = log_append_dec(p, static_cast<unsigned>(*reinterpret_cast<int*>(object + 4)));
			p = log_append(p, " f=");
			p = log_append_hex(p, static_cast<unsigned>(*reinterpret_cast<int*>(object + 8)));
			p = log_append(p, " face=\"");
			p += append_face(p, object);
			p = log_append(p, "\"");
		}

		log_line(buffer, p);
	}
}

static unsigned resolve_core(unsigned renderer)
{
	// 解析失败必须返回 0，绝不能回退成 renderer 本身：
	// 非核心的 renderer 会把 g_core 覆盖成错误对象，随后 core+0xA8 的临界区就会崩。
	if (!renderer || !page_readable(reinterpret_cast<const void*>(renderer + 4), 4))
		return 0;

	const unsigned candidate = *reinterpret_cast<unsigned*>(renderer + 4);
	if (candidate < 0x10000 ||
		!page_readable(reinterpret_cast<const void*>(candidate), 0x1C0))
		return 0;

	const unsigned f0 = *reinterpret_cast<unsigned*>(candidate + 0x108);
	if (!f0)
		return candidate;   // 字体还没建：先接受，后续会被更严格地校验

	if (page_readable(reinterpret_cast<const void*>(f0), 4) &&
		*reinterpret_cast<unsigned*>(f0) == 0x434722BCu)
		return candidate;

	return 0;
}

// ---------------------------------------------------------------- font_select 劫持
// 0x4332AF73: int __stdcall font_select(int flags)，ret 4。原逻辑（反汇编）：
//     base = (flags & 0x100) ? core+0x108
//          : (*(float*)(core+0x104) == 1.0f ? core+0x108 : core+0x15C)
//     flags&2 -> base+0x18 ; else flags&1 -> base+0x0C ; else flags&4 -> base+0x30 ; else base
// 菜单里 CJK 字体实际在 slot6 = core+0x108+0x48（Malgun Gothic），但这段代码永远选不到它。
// 我们的改动：当 g_cjk_pending 置位（本次绘制是中文译文）时，返回我们自己的槽，
// 槽里放的就是 slot6 那个字体对象。拉丁绘制完全不受影响。
static unsigned g_core = 0;
static unsigned g_cjk_slot[4] = {};
static volatile int g_cjk_pending = 0;
static volatile unsigned g_cjk_bind_tick = 0;
static bool g_font_select_hooked = false;

extern "C" int __stdcall tr_font_select(int flags)
{
	if (g_cjk_pending && g_core)
	{
		g_cjk_pending = 0;
		static bool logged_cjk_select = false;
		if (!logged_cjk_select)
		{
			logged_cjk_select = true;
			log_text("font_select: CJK slot returned (hijack is live)");
		}
		// 返回真实 slot6 节点。热路径不做 VirtualQuery，直接读（core 已校验）。
		const unsigned node = g_core + 0x108 + 0x48;
		const unsigned font = *reinterpret_cast<unsigned*>(node);
		if (font && *reinterpret_cast<unsigned*>(font) == 0x434722BCu)
		{
			g_cjk_slot[0] = font;
			return static_cast<int>(node);
		}
		return static_cast<int>(reinterpret_cast<unsigned>(&g_cjk_slot[0]));
	}

	unsigned base;
	if (flags & 0x100)
		base = g_core + 0x108;
	else if (*reinterpret_cast<unsigned*>(g_core + 0x104) == 0x3F800000u)
		base = g_core + 0x108;
	else
		base = g_core + 0x15C;

	if (flags & 2)
		return static_cast<int>(base + 0x18);
	if (flags & 1)
		return static_cast<int>(base + 0x0C);
	if (flags & 4)
		return static_cast<int>(base + 0x30);
	return static_cast<int>(base);
}

static void install_font_select_hook()
{
	if (g_font_select_hooked || !g_core)
		return;
	g_font_select_hooked = true;

	unsigned char* target = reinterpret_cast<unsigned char*>(0x4332AF73u);
	DWORD old_protect = 0;
	if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old_protect))
	{
		log_pair("font_select: VirtualProtect failed, gle=", GetLastError(), 0);
		g_font_select_hooked = false;
		return;
	}

	target[0] = 0xE9;
	const int rel = static_cast<int>(reinterpret_cast<unsigned>(&tr_font_select)) -
		static_cast<int>(reinterpret_cast<unsigned>(target) + 5);
	*reinterpret_cast<int*>(target + 1) = rel;

	VirtualProtect(target, 8, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), target, 8);

	log_pair("font_select hooked: relay=", reinterpret_cast<unsigned>(&tr_font_select),
		static_cast<unsigned>(rel));
}

// ---------------------------------------------------------------- 字体纹理
// 每个字体槽节点: { font, texture, next }。
// 绘制前会取成功匹配节点的 [1] 作为纹理；为 0 则整段文字直接跳过。
// 0x4334A887(core, slot): 从字体图集(+0x88/+0x7C/+0x80)创建 D3D 纹理并写回 slot[1]。
typedef int (__fastcall* refresh_texture_fn)(int core, int* slot);

// 引擎自己的字体集重建 (renderer vtable +0x04 @ 0x433C95CE) 在创建纹理前会锁 core+0xA8。
// 我们直接调用 sub_4334A887 时必须拿同一把锁，否则和引擎的 per-frame 重建并发会崩。
typedef void (__stdcall* cs_fn)(void*);

static cs_fn resolve_kernel_proc(const char* name)
{
	HMODULE k = GetModuleHandleA("kernel32.dll");
	return k ? reinterpret_cast<cs_fn>(GetProcAddress(k, name)) : 0;
}

static bool core_is_valid()
{
	if (!g_core || !page_readable(reinterpret_cast<const void*>(g_core), 0x1C0))
		return false;
	const unsigned f0 = *reinterpret_cast<unsigned*>(g_core + 0x108);
	return !f0 ||
		(page_readable(reinterpret_cast<const void*>(f0), 4) &&
		 *reinterpret_cast<unsigned*>(f0) == 0x434722BCu);
}

static void fontset_lock()
{
	if (!core_is_valid() || !page_readable(reinterpret_cast<const void*>(g_core + 0xA8), 4))
		return;
	static cs_fn enter = 0;
	if (!enter)
		enter = resolve_kernel_proc("EnterCriticalSection");
	if (enter)
		enter(reinterpret_cast<void*>(g_core + 0xA8));
}

static void fontset_unlock()
{
	if (!core_is_valid() || !page_readable(reinterpret_cast<const void*>(g_core + 0xA8), 4))
		return;
	static cs_fn leave = 0;
	if (!leave)
		leave = resolve_kernel_proc("LeaveCriticalSection");
	if (leave)
		leave(reinterpret_cast<void*>(g_core + 0xA8));
}

static bool refresh_slot_texture(unsigned slot_node)
{
	if (!g_core || !slot_node)
		return false;
	if (!page_readable(reinterpret_cast<const void*>(slot_node), 12))
		return false;

	const unsigned font = *reinterpret_cast<unsigned*>(slot_node);
	if (!font || !page_readable(reinterpret_cast<const void*>(font), 0xC4))
		return false;
	if (*reinterpret_cast<unsigned*>(font) != 0x434722BCu)
		return false;
	// 图集高度为 0 时没必要建纹理
	if (*reinterpret_cast<int*>(font + 0x80) <= 0)
		return false;

	refresh_texture_fn refresh = reinterpret_cast<refresh_texture_fn>(0x4334A887u);
	const int r = refresh(static_cast<int>(g_core), reinterpret_cast<int*>(slot_node));
	return r >= 0;
}

// 预先扩容 slot6 图集，避免绘制期间 vector realloc 让渲染器拿到悬空指针。
static void pregrow_slot6_atlas()
{
	if (!g_core)
		return;

	const unsigned slot = g_core + 0x108 + 0x48;
	if (!page_readable(reinterpret_cast<const void*>(slot), 12))
		return;
	const unsigned font = *reinterpret_cast<unsigned*>(slot);
	if (!font || !page_readable(reinterpret_cast<const void*>(font), 0xC4))
		return;
	if (*reinterpret_cast<unsigned*>(font) != 0x434722BCu)
		return;

	const int width = *reinterpret_cast<int*>(font + 0x7C);
	if (width <= 0)
		return;

	const int target = 2048;
	if (*reinterpret_cast<int*>(font + 0x80) >= target)
		return;

	typedef void (__thiscall* vec_resize_fn)(void*, unsigned);
	vec_resize_fn resize = reinterpret_cast<vec_resize_fn>(0x43399A02u);
	resize(reinterpret_cast<void*>(font + 0x88),
		static_cast<unsigned>(width) * static_cast<unsigned>(target));
	*reinterpret_cast<int*>(font + 0x80) = target;
}

// 所有字体槽：图集增长或 slot[1] 为空时重建纹理（修“部分拉丁不显示”）
static void ensure_slot_textures()
{
	if (!g_core)
		return;

	static unsigned seen_a[7] = {};
	static unsigned seen_b[7] = {};

	for (int set = 0; set < 2; ++set)
	{
		const unsigned base = g_core + (set == 0 ? 0x108u : 0x15Cu);
		unsigned* seen = (set == 0) ? seen_a : seen_b;

		for (int i = 0; i < 7; ++i)
		{
			const unsigned node = base + static_cast<unsigned>(i) * 0x0Cu;
			if (!page_readable(reinterpret_cast<const void*>(node), 12))
				continue;

			const unsigned font = *reinterpret_cast<unsigned*>(node);
			if (!font || !page_readable(reinterpret_cast<const void*>(font), 0xC4))
				continue;
			if (*reinterpret_cast<unsigned*>(font) != 0x434722BCu)
				continue;
			if (*reinterpret_cast<int*>(font + 0x80) <= 0)
				continue;

			const unsigned end = *reinterpret_cast<unsigned*>(font + 0x8C);
			const bool missing = *reinterpret_cast<unsigned*>(node + 4) == 0;
			if (missing || end != seen[i])
			{
				if (refresh_slot_texture(node))
					seen[i] = end;
			}
		}
	}
}

// ---------------------------------------------------------------- 字形预热
// 引擎绘制路径只调用 vtable+0x14（查哈希），从不调用 +0x04（光栅化），
// 所以中文永远查不到。这里在绘制前主动调用 +0x04 把字形烤进图集/哈希。
typedef bool (__thiscall* font_lookup_fn)(void*, int, void*);
typedef bool (__thiscall* font_raster_fn)(void*, int);

static void warm_cjk_text(const wchar_t* text, bool include_ascii)
{
	if (!g_warm_enabled)
		return;
	if (g_cjk_ready && !g_prewarming)
		return;   // 已一次性预烤完，绘制期不再改图集/纹理
	if (!text || !g_core)
		return;

	// 每次都从真实 slot6 节点重新读字体：引擎在缩放/初始化变化时会重建字体集，
	// 缓存的 g_cjk_slot[0] 可能已经是被释放的旧对象。
	const unsigned slot = g_core + 0x108 + 0x48;
	if (!page_readable(reinterpret_cast<const void*>(slot), 12))
		return;
	const unsigned font = *reinterpret_cast<unsigned*>(slot);
	if (!font || !page_readable(reinterpret_cast<const void*>(font), 0xC4))
		return;
	if (*reinterpret_cast<unsigned*>(font) != 0x434722BCu)
		return;
	g_cjk_slot[0] = font;

	bool baked_any = false;

	for (int i = 0; text[i] && i < 256; ++i)
	{
		const unsigned ch = static_cast<unsigned>(text[i]);
		if (ch <= 0x7F && !include_ascii)
			continue;

		// lookup 会往 out 写 0x28 字节；给足 64 字节，避免覆盖相邻局部变量。
		unsigned out[16] = {};
		if (reinterpret_cast<font_lookup_fn>(0x43386484u)(
			reinterpret_cast<void*>(font), static_cast<int>(ch), out))
			continue;

		if (reinterpret_cast<font_raster_fn>(0x4332F22Fu)(
			reinterpret_cast<void*>(font), static_cast<int>(ch)))
			baked_any = true;
	}

	if (g_prewarming)
		return;   // 预烤阶段只烤字，最后统一建一次纹理

	// 预热后立刻补 slot6 纹理（这一版是验证过能显示“其他”的）
	const unsigned slot_node = g_core + 0x108 + 0x48;
	if (!page_readable(reinterpret_cast<const void*>(slot_node), 12))
		return;
	if (!g_texture_enabled)
		return;
	const bool no_texture = *reinterpret_cast<unsigned*>(slot_node + 4) == 0;
	if (baked_any || no_texture)
	{
		static unsigned last_refresh_tick = 0;
		const unsigned now = GetTickCount();
		// 缺失时立即建；图集增长最多 100ms 更新一次，避免首帧刷爆纹理
		if (no_texture || last_refresh_tick == 0 || now - last_refresh_tick >= 100)
		{
			// 不加 core+0xA8 锁：它不是标准 CRITICAL_SECTION，用 kernel32 的
			// EnterCriticalSection 会直接崩。纹理刷新本身已在渲染线程上。
			// 旧纹理可能仍被渲染器引用：不 Release，直接重建（少量泄漏可接受）
			if (!no_texture)
				*reinterpret_cast<unsigned*>(slot_node + 4) = 0;
			refresh_slot_texture(slot_node);
			last_refresh_tick = now;
		}
	}
}

// 一次性把所有翻译值里的字符烤进 slot6，然后只建一次纹理。
// 这样菜单/Lua 后续绘制时不会再有纹理重建，避免“抽搐”。
static void prewarm_all_translations()
{
	if (g_cjk_ready || !g_core || !g_cjk_slot[0])
		return;

	g_prewarming = true;
	for (int i = 0; i < g_pair_count; ++i)
		warm_cjk_text(g_pairs[i].to, true);
	g_prewarming = false;

	g_cjk_ready = true;
	refresh_slot_texture(g_core + 0x108 + 0x48);
}

// 由 stub 在替换文本后调用：译文含非 ASCII 时给下一次 font_select 打标记
extern "C" void __cdecl tr_check_cjk(const wchar_t* text)
{
	if (!text)
		return;

	for (int i = 0; text[i] && i < 256; ++i)
	{
		if (text[i] > 0x7F)
		{
			g_cjk_pending = 1;
			g_cjk_bind_tick = GetTickCount();
			warm_cjk_text(text, true);
			return;
		}
	}
}

static void tr_dump_fonts(unsigned renderer)
{
	g_renderer = renderer;
	const unsigned core = resolve_core(renderer);

	char buffer[128];
	char* p = log_append(buffer, "renderer=");
	p = log_append_hex(p, renderer);
	p = log_append(p, " core=");
	p = log_append_hex(p, core);
	log_line(buffer, p);

	if (!page_readable(reinterpret_cast<const void*>(core), 0x1C0))
	{
		log_text("core not readable - skip font dump");
		return;
	}

	p = log_append(buffer, "scale bits=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(core + 0x104));
	log_line(buffer, p);

	log_font_set("setA", core + 0x108);
	log_font_set("setB", core + 0x15C);
}

// ---------------------------------------------------------------- 追加 CJK 字体
// 复制 0x4334E194 的用法：create_font(face, height, flags) -> 字体对象。
// 然后直接把某个槽的字体对象换成它？不行（会破坏拉丁）。
// 这里先只创建并记录，真正的挂接方式等 dump 出 face 再定。
typedef void* (__fastcall* create_font_t)(const wchar_t* face, int height, int flags);

// ---------------------------------------------------------------- create_font 劫持
// 0x4334E194: void* __fastcall create_font(const wchar_t* face, int height, int flags)
// 引擎用它创建字体对象（字形缓存/图集都在里面）。这里只把 CJK 槽的 face 换掉，
// 其余原样转发，保证字体对象仍然由引擎自己的初始化流程创建。
static void* g_create_font_tramp = 0;
static bool g_create_font_hooked = false;
static bool g_face_swap_enabled = true;
static int g_font_create_logged = 0;

static bool wstr_same(const wchar_t* a, const wchar_t* b)
{
	if (!a || !b)
		return false;
	while (*a && *a == *b)
	{
		++a;
		++b;
	}
	return *a == *b;
}

static void* __fastcall tr_create_font(const wchar_t* face, int height, int flags)
{
	const wchar_t* original_face = face;
	if (g_face_swap_enabled && wstr_same(face, L"Malgun Gothic"))
	{
		face = L"Microsoft YaHei";
	}

	if (g_font_create_logged < 16)
	{
		++g_font_create_logged;
		char buffer[160];
		char* p = log_append(buffer, "create_font: face=\"");
		for (int i = 0; original_face && original_face[i] && i < 28; ++i)
			*p++ = (original_face[i] >= 0x20 && original_face[i] < 0x7F)
				? static_cast<char>(original_face[i]) : '.';
		p = log_append(p, "\" h=");
		p = log_append_dec(p, static_cast<unsigned>(height));
		p = log_append(p, " f=");
		p = log_append_hex(p, static_cast<unsigned>(flags));
		p = log_append(p, face != original_face ? " -> Microsoft YaHei" : "");
		log_line(buffer, p);
	}

	return reinterpret_cast<create_font_t>(g_create_font_tramp)(face, height, flags);
}

static void install_create_font_hook()
{
	if (g_create_font_hooked)
		return;

	const unsigned kCreateFont = 0x4334E194u;
	const unsigned kResume = 0x4334E1A0u;   // 前 12 字节之后

	if (!page_readable(reinterpret_cast<const void*>(kCreateFont), 12))
	{
		log_text("create_font: target not readable");
		return;
	}

	unsigned char* tramp = static_cast<unsigned char*>(
		VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!tramp)
	{
		log_pair("create_font: trampoline failed, gle=", GetLastError(), 0);
		return;
	}

	// 前 12 字节：55 8B EC 83 E4 F8 81 EC F0 00 00 00
	const unsigned char* src = reinterpret_cast<const unsigned char*>(kCreateFont);
	for (int i = 0; i < 12; ++i)
		tramp[i] = src[i];
	tramp[12] = 0xE9;
	*reinterpret_cast<int*>(tramp + 13) =
		static_cast<int>(kResume - (reinterpret_cast<unsigned>(tramp) + 17));

	DWORD old_protect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(kCreateFont), 16, PAGE_EXECUTE_READWRITE, &old_protect))
	{
		log_pair("create_font: VirtualProtect failed, gle=", GetLastError(), 0);
		return;
	}

	unsigned char* target = reinterpret_cast<unsigned char*>(kCreateFont);
	target[0] = 0xE9;
	*reinterpret_cast<int*>(target + 1) =
		static_cast<int>(reinterpret_cast<unsigned>(&tr_create_font) - (kCreateFont + 5));

	VirtualProtect(reinterpret_cast<void*>(kCreateFont), 16, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kCreateFont), 16);

	g_create_font_tramp = tramp;
	g_create_font_hooked = true;
	log_pair("create_font hooked: relay=", reinterpret_cast<unsigned>(&tr_create_font),
		static_cast<unsigned>(reinterpret_cast<unsigned>(tramp)));
}

static unsigned g_draw_seen[64][4];
static int g_draw_seen_count = 0;

// 把 setA 的 6 个槽全部换成同 height/flags 的雅黑字体对象。
// 实验结论（2026-11-06）：引擎不接受外部创建的字体对象 —— 画面变空白、
// 打开下拉框直接崩，因为字体纹理/字形缓存没进引擎自己的初始化流程。
// 因此默认关闭，只保留代码供以后用 #fontswap 再试。
static bool g_swap_fonts = false;
static bool g_fonts_swapped = false;

static void tr_swap_slot_fonts(unsigned renderer)
{
	const unsigned core = resolve_core(renderer);
	if (!page_readable(reinterpret_cast<const void*>(core), 0x1C0))
	{
		log_text("swap: core unreadable");
		return;
	}

	create_font_t create = reinterpret_cast<create_font_t>(0x4334E194);
	static const unsigned kSlots[6] = { 0x00, 0x0C, 0x18, 0x24, 0x30, 0x3C };

	for (int i = 0; i < 6; ++i)
	{
		unsigned* slot = reinterpret_cast<unsigned*>(core + 0x108 + kSlots[i]);
		if (!page_readable(slot, 4))
			continue;

		const unsigned old_font = *slot;
		if (!old_font || !page_readable(reinterpret_cast<const void*>(old_font), 12))
			continue;

		const int height = *reinterpret_cast<int*>(old_font + 4);
		const int flags = *reinterpret_cast<int*>(old_font + 8);

		char face[31] = {};
		append_face(face, old_font);

		void* font = create(L"Microsoft YaHei", height, flags);

		char buffer[224];
		char* p = log_append(buffer, "swap slot");
		p = log_append_dec(p, static_cast<unsigned>(i));
		p = log_append(p, " h=");
		p = log_append_dec(p, static_cast<unsigned>(height));
		p = log_append(p, " f=");
		p = log_append_hex(p, static_cast<unsigned>(flags));
		p = log_append(p, " old=");
		p = log_append_hex(p, old_font);
		p = log_append(p, " new=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(font)));
		p = log_append(p, " was=\"");
		p = log_append(p, face);
		p = log_append(p, "\"");
		log_line(buffer, p);

		if (font)
			*slot = reinterpret_cast<unsigned>(font);
	}
}

static bool has_non_ascii(const wchar_t* text)
{
	for (int i = 0; text && text[i] && i < 256; ++i)
	{
		if (text[i] > 0x7F)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------- 字形探针（只读）
// 用来判断“字体对象到底有没有 CJK 覆盖/字形”，不修改引擎状态。
//   +0xBC  覆盖位图（由 GetFontUnicodeRanges 填充），0x2000 字节 = 0x10000 位
//   +0x9C  字形哈希桶  +0xA0 桶数  +0x98 已缓存字形数
//   +0x74 图集光标 x   +0x78 图集光标 y   +0x7C 图集宽   +0x80 图集高
//   +0x14 字形查找（__thiscall, ret 8）
typedef bool (__thiscall* font_lookup_t)(void* self, int ch, void* out);

static int glyph_covered(unsigned obj, unsigned ch)
{
	if (!page_readable(reinterpret_cast<const void*>(obj + 0xBC), 0x2000))
		return -1;
	const unsigned word = *reinterpret_cast<unsigned*>(obj + 0xBC + ((ch >> 5) & 0x7FF) * 4);
	return static_cast<int>((word >> (ch & 31)) & 1);
}

static void probe_glyph(unsigned obj, unsigned ch, const char* tag)
{
	char buffer[256];
	char* p = log_append(buffer, "probe ");
	p = log_append(p, tag);
	p = log_append(p, " ch=");
	p = log_append_hex(p, ch);

	if (!obj || !page_readable(reinterpret_cast<const void*>(obj), 0xC4))
	{
		p = log_append(p, " obj=null");
		log_line(buffer, p);
		return;
	}

	const unsigned vt = *reinterpret_cast<unsigned*>(obj);
	p = log_append(p, " obj=");
	p = log_append_hex(p, obj);
	p = log_append(p, " vt=");
	p = log_append_hex(p, vt);

	if (vt != 0x434722BCu)
	{
		log_line(buffer, p);
		return;
	}

	const int cov = glyph_covered(obj, ch);
	p = log_append(p, " cov=");
	p = log_append_dec(p, cov >= 0 ? static_cast<unsigned>(cov) : 9u);

	unsigned out[16] = {};   // lookup 会写 0x28 字节，必须留足空间
	int hit = 0;
	if (page_readable(reinterpret_cast<const void*>(0x43386484u), 4))
	{
		font_lookup_t lookup = reinterpret_cast<font_lookup_t>(0x43386484u);
		hit = lookup(reinterpret_cast<void*>(obj), static_cast<int>(ch), out) ? 1 : 0;
	}
	p = log_append(p, " hit=");
	p = log_append_dec(p, static_cast<unsigned>(hit));

	if (hit)
	{
		p = log_append(p, " uv=");
		for (int i = 0; i < 4; ++i)
		{
			p = log_append_hex(p, out[i]);
			p = log_append(p, ",");
		}
	}

	log_line(buffer, p);
}

static void probe_font_object(unsigned obj, const char* tag)
{
	if (!obj || !page_readable(reinterpret_cast<const void*>(obj), 0xC4))
		return;

	char buffer[256];
	char* p = log_append(buffer, "font ");
	p = log_append(p, tag);
	p = log_append(p, " obj=");
	p = log_append_hex(p, obj);
	p = log_append(p, " cur=");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0x74));
	p = log_append(p, ",");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0x78));
	p = log_append(p, " atlas=");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0x7C));
	p = log_append(p, "x");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0x80));
	p = log_append(p, " glyphs=");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0x98));
	p = log_append(p, " buckets=");
	p = log_append_dec(p, *reinterpret_cast<unsigned*>(obj + 0xA0));
	p = log_append(p, " vec=");
	p = log_append_dec(p,
		(*reinterpret_cast<unsigned*>(obj + 0x8C) -
		 *reinterpret_cast<unsigned*>(obj + 0x88)) / 4);
	p = log_append(p, " tex=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(obj + 0xB8));
	p = log_append(p, " v84=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(obj + 0x84));
	log_line(buffer, p);
}

static void probe_slot_node(unsigned slot, const char* tag)
{
	if (!slot || !page_readable(reinterpret_cast<const void*>(slot), 12))
		return;

	char buffer[160];
	char* p = log_append(buffer, "slot ");
	p = log_append(p, tag);
	p = log_append(p, " node=");
	p = log_append_hex(p, slot);
	p = log_append(p, " [0]=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(slot));
	p = log_append(p, " [4]=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(slot + 4));
	p = log_append(p, " [8]=");
	p = log_append_hex(p, *reinterpret_cast<unsigned*>(slot + 8));
	log_line(buffer, p);
}

// ---------------------------------------------------------------- vtable 观测
// 字体 vtable 0x434722BC：
//   +0x04 = 0x4332F22F  光栅化/取字形（__thiscall, ret 4）
//   +0x14 = 0x43386484  字形哈希查找（__thiscall, ret 8）
// 这里只包一层日志，原样转发，行为不变。用来确认引擎到底有没有对这些字体对象
// 调光栅化/查找，以及用的是哪个对象。
static unsigned g_vt_raster_addr = 0;
static unsigned g_vt_lookup_addr = 0;
static int g_vt_cjk_logs = 0;
static int g_vt_ascii_logs = 0;
static int g_vt_raster_logs = 0;

// 用 __fastcall(self=ecx, dummy=edx, ...) 模拟 __thiscall 的 ABI：
//   this=ecx，其余参数照样在栈上，callee 清理，和原函数 ret N 完全一致。
static bool __fastcall vt_raster_log(void* self, void*, int ch)
{
	typedef bool (__thiscall* fn_t)(void*, int);
	const bool r = reinterpret_cast<fn_t>(g_vt_raster_addr)(self, ch);

	if (g_vt_raster_logs < 48)
	{
		++g_vt_raster_logs;
		char buffer[176];
		char* p = log_append(buffer, "vt04 obj=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(self)));
		p = log_append(p, " ch=");
		p = log_append_hex(p, static_cast<unsigned>(ch));
		p = log_append(p, " ret=");
		p = log_append_dec(p, r ? 1u : 0u);
		p = log_append(p, " call=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(_ReturnAddress())));
		log_line(buffer, p);
	}
	return r;
}

static bool __fastcall vt_lookup_log(void* self, void*, int ch, void* out)
{
	typedef bool (__thiscall* fn_t)(void*, int, void*);
	const bool r = reinterpret_cast<fn_t>(g_vt_lookup_addr)(self, ch, out);

	int* counter = (ch > 0x7F) ? &g_vt_cjk_logs : &g_vt_ascii_logs;
	const int limit = (ch > 0x7F) ? 48 : 16;
	if (*counter < limit)
	{
		++*counter;
		char buffer[176];
		char* p = log_append(buffer, "vt14 obj=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(self)));
		p = log_append(p, " ch=");
		p = log_append_hex(p, static_cast<unsigned>(ch));
		p = log_append(p, " ret=");
		p = log_append_dec(p, r ? 1u : 0u);
		p = log_append(p, " call=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(_ReturnAddress())));
		log_line(buffer, p);
	}
	return r;
}

// +0x30 = 0x43400283：返回 &font->texture（+0xB8）。绘制用它绑定纹理。
static void* __fastcall vt_texture_get(void* self, void*)
{
	if (!self)
		return 0;

	unsigned char* base = reinterpret_cast<unsigned char*>(self);
	unsigned* slot = reinterpret_cast<unsigned*>(base + 0xB8);

	static int logs = 0;
	if (logs < 30)
	{
		++logs;
		char buffer[208];
		char* p = log_append(buffer, "texget obj=");
		p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(self)));
		p = log_append(p, " tex=");
		p = log_append_hex(p, *slot);
		p = log_append(p, " v84=");
		p = log_append_hex(p, *reinterpret_cast<unsigned*>(base + 0x84));
		p = log_append(p, " vec=");
		p = log_append_dec(p,
			(*reinterpret_cast<unsigned*>(base + 0x8C) -
			 *reinterpret_cast<unsigned*>(base + 0x88)) / 4);
		p = log_append(p, " wh=");
		p = log_append_dec(p, *reinterpret_cast<unsigned*>(base + 0x7C));
		p = log_append(p, "x");
		p = log_append_dec(p, *reinterpret_cast<unsigned*>(base + 0x80));
		log_line(buffer, p);
	}

	return slot;
}

// 翻译表路径（resolver hook 和下面的热重载都会用到）
static wchar_t g_map_path[MAX_PATH] = {};

// ---------------------------------------------------------------- resolver hook
// 原生 resolver 的“三选一”入口 sub_433AB612(record, attempt)：
//   record+0x1FC 写 pose、+0x28 写 yaw、+0x230 拷贝 hitbox 矩阵、+0x59 写回 attempt。
// 不 patch 函数本体（省去跳板/序言处理），只改三处 call 立即数：
//   0x43349DE5  publish/propagate      (0x43349D92..0x43349E21)
//   0x4336C66F  advance_pending_attempt(0x4336C64E..0x4336C674)
//   0x4341386E  lag-record propagate   (0x43413823..0x4341389A)
// 作用：给“换边”加 0.35s 冷却，避免一次误判把整条历史链翻掉
//       （借鉴 Lua resolver 的 side 抖动抑制，对应重构工程的 feedback cooldown）。
// 默认开启；如果目标二进制换了版本导致地址校验失败，会自动跳过（patched=0），不会乱改。
using select_variant_t = void(__fastcall*)(int record, unsigned char attempt);

static select_variant_t g_select_variant = nullptr;
static const unsigned kSelectVariant = 0x433AB612u;
static const unsigned kSelectCallSites[] = { 0x43349DE5u, 0x4336C66Fu, 0x4341386Eu };
static bool g_resolver_hook_active = false;

struct resolver_flip_t
{
	int record;
	unsigned char attempt;
	unsigned long long tick;
	unsigned char pending;
	unsigned char pending_active;
	unsigned long long pending_since;
};
static const unsigned long long kResolverFlipCooldownMs = 350ull;   // 两次换边的最小间隔
static const unsigned long long kResolverFlipPersistMs = 120ull;    // 新选择要持续多久才采纳
static resolver_flip_t g_flip_slots[512];

static unsigned char resolver_policy(int record, unsigned char attempt)
{
	if (attempt > 2)
		attempt = static_cast<unsigned char>(attempt % 3);
	auto& slot = g_flip_slots[(static_cast<unsigned>(record) >> 4) & 511u];
	const unsigned long long now = GetTickCount64();
	if (slot.record != record || slot.tick == 0)
	{
		slot.record = record;
		slot.attempt = attempt;
		slot.tick = now;
		slot.pending_active = 0;
		return attempt;
	}
	if (attempt == slot.attempt)
	{
		slot.pending_active = 0;
		return attempt;
	}
	// 刚换过边：冷却期内一律拒绝，避免来回翻
	if (now - slot.tick < kResolverFlipCooldownMs)
		return slot.attempt;
	// 去抖：新选择必须持续一段时间才采纳（单帧误判不再生效）
	if (!slot.pending_active || slot.pending != attempt)
	{
		slot.pending = attempt;
		slot.pending_active = 1;
		slot.pending_since = now;
		return slot.attempt;
	}
	if (now - slot.pending_since < kResolverFlipPersistMs)
		return slot.attempt;
	slot.pending_active = 0;
	slot.attempt = attempt;
	slot.tick = now;
	return attempt;
}

static void __fastcall tr_select_variant(int record, unsigned char attempt)
{
	g_select_variant(record, resolver_policy(record, attempt));
}

static bool patch_resolver_call(unsigned site)
{
	if (!page_readable(reinterpret_cast<const void*>(site), 5))
		return false;
	unsigned char* bytes = reinterpret_cast<unsigned char*>(site);
	if (bytes[0] != 0xE8)
		return false;
	const int relative = *reinterpret_cast<const int*>(bytes + 1);
	const unsigned target = site + 5u + static_cast<unsigned>(relative);
	const unsigned self = static_cast<unsigned>(reinterpret_cast<unsigned>(&tr_select_variant));
	if (target != kSelectVariant && target != self)
		return false;
	DWORD old_protect = 0;
	if (!VirtualProtect(bytes, 5, PAGE_EXECUTE_READWRITE, &old_protect))
		return false;
	*reinterpret_cast<int*>(bytes + 1) = static_cast<int>(self - (site + 5u));
	VirtualProtect(bytes, 5, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), bytes, 5);
	return true;
}

// ---------------------------------------------------------------- 环境评分死区 ±25°
// 原生在 0x43349D0C..0x43349D26 内联了 “pose < -20 → 2 / pose > 20 → 1 / 否则 0”。
// 两个常量 (0x4347222C = -20.0f、0x4346EEB0 = +20.0f) 被别处引用（后者 29 处），不能改；
// 这 27 字节无外部跳转入口，整段换成 call 我们的实现（±25°），返回后自然落到
// 0x43349D27 继续写回 attempt。
extern "C" __declspec(naked) void tr_classify_environment()
{
	__asm
	{
		push 0C1C80000h            // -25.0f
		movss xmm0, dword ptr[esp]
		add esp, 4
		comiss xmm0, xmm2          // -25 ? pose
		jbe short classify_high
		mov al, 2
		ret
	classify_high:
		push 041C80000h            // +25.0f
		movss xmm0, dword ptr[esp]
		add esp, 4
		comiss xmm2, xmm0          // pose ? 25
		seta al
		ret
	}
}

static bool patch_environment_dead_zone()
{
	const unsigned site = 0x43349D0Cu;
	unsigned char* bytes = reinterpret_cast<unsigned char*>(site);
	if (!page_readable(reinterpret_cast<const void*>(site), 27))
		return false;
	// 原序言：F3 0F 10 05 2C 22 47 43  (movss xmm0, ds:4347222Ch)
	if (bytes[0] != 0xF3 || bytes[1] != 0x0F || bytes[2] != 0x10 || bytes[3] != 0x05)
		return false;
	const unsigned self =
		static_cast<unsigned>(reinterpret_cast<unsigned>(&tr_classify_environment));
	DWORD old_protect = 0;
	if (!VirtualProtect(bytes, 27, PAGE_EXECUTE_READWRITE, &old_protect))
		return false;
	bytes[0] = 0xE8;
	*reinterpret_cast<int*>(bytes + 1) = static_cast<int>(self - (site + 5u));
	for (int i = 5; i < 27; ++i)
		bytes[i] = 0x90;
	VirtualProtect(bytes, 27, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), bytes, 27);
	return true;
}

static void install_resolver_hook()
{
	if (g_resolver_hook_active)
		return;
	g_select_variant = reinterpret_cast<select_variant_t>(kSelectVariant);
	unsigned patched = 0;
	for (unsigned site : kSelectCallSites)
		if (patch_resolver_call(site))
			++patched;
	g_resolver_hook_active = patched != 0;
	patch_environment_dead_zone();
}

static void install_vtable_probe()
{
	static bool installed = false;
	if (installed)
		return;
	installed = true;

	unsigned* vt = reinterpret_cast<unsigned*>(0x434722BCu);
	if (!page_readable(vt, 0x40))
	{
		log_text("vtable probe: vtable not readable");
		return;
	}

	DWORD old_protect = 0;
	if (!VirtualProtect(vt, 0x40, PAGE_EXECUTE_READWRITE, &old_protect))
	{
		log_pair("vtable probe: VirtualProtect failed, gle=", GetLastError(), 0);
		return;
	}

	g_vt_raster_addr = vt[1];    // +0x04
	g_vt_lookup_addr = vt[5];    // +0x14
	vt[1] = static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_raster_log));
	vt[5] = static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_lookup_log));
	vt[12] = static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_texture_get));  // +0x30

	VirtualProtect(vt, 0x40, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), vt, 0x40);

	log_pair("vtable probe: raster orig=", g_vt_raster_addr,
		static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_raster_log)));
	log_pair("vtable probe: lookup orig=", g_vt_lookup_addr,
		static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_lookup_log)));
	log_pair("vtable probe: texture getter=", static_cast<unsigned>(reinterpret_cast<unsigned>(&vt_texture_get)), 0);
}

// ---------------------------------------------------------------- 纹理绑定观测
// 0x4339E532: renderer->bind_texture(tex, 4, 0)
//   __fastcall(ecx=renderer, edx=5, [esp+4]=tex, [esp+8]=4, [esp+0xC]=0)
// 原函数前 6 字节: 53 56 8B F1 8B DA（push ebx; push esi; mov esi,ecx; mov ebx,edx）
static int g_bind_logs = 0;

extern "C" void __cdecl tr_log_bind(unsigned renderer, unsigned tex, unsigned caller)
{
	// 只记：最前面的 8 次（拉丁基线）+ CJK 绘制前后的绑定
	static int baseline = 0;
	const unsigned now = GetTickCount();
	const bool cjk = g_cjk_bind_tick != 0 && (now - g_cjk_bind_tick) < 60;
	if (!cjk && baseline >= 8)
		return;
	if (g_bind_logs >= 200)
		return;

	++g_bind_logs;
	if (!cjk)
		++baseline;

	char buffer[208];
	char* p = log_append(buffer, cjk ? "bind[cjk]" : "bind");
	p = log_append(p, " tex=");
	p = log_append_hex(p, tex);
	p = log_append(p, " call=");
	p = log_append_hex(p, caller);
	if (renderer && page_readable(reinterpret_cast<const void*>(renderer), 0xF0))
	{
		p = log_append(p, " ren=");
		p = log_append_hex(p, renderer);
		p = log_append(p, " c4=");
		p = log_append_hex(p, *reinterpret_cast<unsigned*>(renderer + 0xC4));
		p = log_append(p, " c8=");
		p = log_append_hex(p, *reinterpret_cast<unsigned*>(renderer + 0xC8));
	}
	log_line(buffer, p);
}

extern "C" __declspec(naked) void tr_bind_relay()
{
	__asm
	{
		pushad
		mov  eax, [esp + 0x24]     // tex (第一个栈参数)
		mov  ecx, [esp + 0x18]     // renderer (原 ecx)
		mov  edx, [esp + 0x20]     // caller (原返回地址)
		push edx
		push eax
		push ecx
		call tr_log_bind
		add  esp, 12
		popad
		push ebx
		push esi
		mov  esi, ecx
		mov  ebx, edx
		mov  eax, 0x4339E538
		jmp  eax
	}
}

static void install_bind_hook()
{
	static bool installed = false;
	if (installed)
		return;
	installed = true;

	unsigned char* target = reinterpret_cast<unsigned char*>(0x4339E532u);
	DWORD old_protect = 0;
	if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old_protect))
	{
		log_pair("bind hook: VirtualProtect failed, gle=", GetLastError(), 0);
		return;
	}

	target[0] = 0xE9;
	*reinterpret_cast<int*>(target + 1) =
		static_cast<int>(reinterpret_cast<unsigned>(&tr_bind_relay) - (0x4339E532u + 5));

	VirtualProtect(target, 8, old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), target, 8);
	log_pair("bind hook: relay=", static_cast<unsigned>(reinterpret_cast<unsigned>(&tr_bind_relay)), 0);
}

static void probe_glyphs_once(const char* when)
{
	if (!g_cjk_slot[0] || !g_core)
		return;

	static int probes = 0;
	if (probes >= 3)
		return;
	++probes;

	log_text(when);

	probe_slot_node(g_core + 0x108 + 0x48, "real6");
	probe_slot_node(reinterpret_cast<unsigned>(&g_cjk_slot[0]), "fake6");

	probe_font_object(g_cjk_slot[0], "cjk(slot6)");
	probe_glyph(g_cjk_slot[0], 0x41, "cjk");
	probe_glyph(g_cjk_slot[0], 0x5176, "cjk");   // 其
	probe_glyph(g_cjk_slot[0], 0x4ED6, "cjk");   // 他
	probe_glyph(g_cjk_slot[0], 0x7CBE, "cjk");   // 精
	probe_glyph(g_cjk_slot[0], 0x51C6, "cjk");   // 准

	const unsigned latin = *reinterpret_cast<unsigned*>(g_core + 0x108);
	probe_font_object(latin, "latin(slot0)");
	probe_glyph(latin, 0x41, "latin");
	probe_glyph(latin, 0x5176, "latin");
}

// 每次 type-10 绘制都会调用（不管是否命中翻译），只记录：
//   - 第一次见到的 renderer（顺便 dump 它的字体表）
//   - 每个不同的 style flags 一次，并标明这次文本是中文还是拉丁
extern "C" void __cdecl tr_note(unsigned renderer, unsigned flags, const wchar_t* text)
{
	static unsigned seen_renderer[4] = {};
	static int seen_renderer_count = 0;

	bool new_renderer = false;
	if (renderer)
	{
		int i = 0;
		for (; i < seen_renderer_count; ++i)
			if (seen_renderer[i] == renderer)
				break;
		if (i == seen_renderer_count && seen_renderer_count < 4)
		{
			seen_renderer[seen_renderer_count++] = renderer;
			new_renderer = true;
		}
	}

	if (renderer)
	{
		// 热路径：只读 renderer+4 比较 core，避免每段文本都 VirtualQuery。
		const unsigned current = *reinterpret_cast<unsigned*>(renderer + 4);
		if (current != g_core || !g_core)
		{
			const unsigned core = resolve_core(renderer);
			if (core && page_readable(reinterpret_cast<const void*>(core), 0x1C0))
			{
				g_core = core;
				g_cjk_ready = false;
				const unsigned font = *reinterpret_cast<unsigned*>(core + 0x108 + 0x48);
				if (font)
					g_cjk_slot[0] = font;
				pregrow_slot6_atlas();
			}
		}
		else
		{
			// core 未变：只比较 slot6 字体指针（廉价），变了说明引擎重建了字体
			const unsigned font = *reinterpret_cast<unsigned*>(g_core + 0x108 + 0x48);
			if (font && font != g_cjk_slot[0])
			{
				g_cjk_slot[0] = font;
				g_cjk_ready = false;
				pregrow_slot6_atlas();
			}
		}

		install_font_select_hook();
	}

	// 抓到 core 后一次性预烤全部翻译值，之后不再改图集/纹理
	prewarm_all_translations();
}

extern "C" void __cdecl tr_draw_log(unsigned x, unsigned y, unsigned width, unsigned length,
	unsigned new_length, const wchar_t* text, unsigned renderer, unsigned flags)
{
	(void)renderer;
	(void)flags;
	(void)text;

	// 诊断日志已关闭
	return;

	if (g_add_cjk && !g_cjk_added)
	{
		g_cjk_added = true;

		const unsigned core = resolve_core(renderer);
		const unsigned first = page_readable(reinterpret_cast<const void*>(core + 0x108), 4)
			? *reinterpret_cast<unsigned*>(core + 0x108) : 0;

		int height = 14;
		int flags = 0;
		if (first && page_readable(reinterpret_cast<const void*>(first), 12))
		{
			height = *reinterpret_cast<int*>(first + 4);
			flags = *reinterpret_cast<int*>(first + 8);
		}

		create_font_t create = reinterpret_cast<create_font_t>(0x4334E194);
		void* font = create(L"Microsoft YaHei", height, flags);
		log_pair("cjk: create(font,height)=",
			static_cast<unsigned>(reinterpret_cast<unsigned>(font)),
			static_cast<unsigned>(height));

		if (font)
		{
			// 先只报告：槽 0/1/2/4 对应 style 0/1/2/4；直接替换会改变拉丁观感，
			// 所以这里仅记录，等确认哪一槽被 CJK 文本使用后再决定替换策略。
			log_text("cjk: font created (not attached yet)");
		}
	}

	if (g_draw_seen_count >= static_cast<int>(sizeof(g_draw_seen) / sizeof(g_draw_seen[0])))
		return;

	for (int i = 0; i < g_draw_seen_count; ++i)
	{
		if (g_draw_seen[i][0] == x && g_draw_seen[i][1] == y &&
			g_draw_seen[i][2] == width && g_draw_seen[i][3] == length)
			return;
	}

	g_draw_seen[g_draw_seen_count][0] = x;
	g_draw_seen[g_draw_seen_count][1] = y;
	g_draw_seen[g_draw_seen_count][2] = width;
	g_draw_seen[g_draw_seen_count][3] = length;
	++g_draw_seen_count;

	char buffer[256];
	char* p = log_append(buffer, "draw x=");
	p = log_append_hex(p, x);
	p = log_append(p, " y=");
	p = log_append_hex(p, y);
	p = log_append(p, " width(orig)=");
	p = log_append_hex(p, width);
	p = log_append(p, " len(orig)=");
	p = log_append_dec(p, length);
	p = log_append(p, " newlen=");
	p = log_append_dec(p, new_length);
	p = log_append(p, " textptr=");
	p = log_append_hex(p, static_cast<unsigned>(reinterpret_cast<unsigned>(text)));
	p = log_append(p, static_cast<unsigned>(reinterpret_cast<unsigned>(text)) >= kImageBase &&
		static_cast<unsigned>(reinterpret_cast<unsigned>(text)) < kImageEnd ? "(image)" : "(heap)");
	p = log_append(p, " text=\"");
	for (int i = 0; i < 24 && text && text[i]; ++i)
		*p++ = (text[i] >= 0x20 && text[i] < 0x7F) ? static_cast<char>(text[i]) : '.';
	p = log_append(p, "\"");
	log_line(buffer, p);
}

// ---------------------------------------------------------------- 翻译表文件
// 默认 <debug.exe 目录>\skeet_tr.txt（loader 通过共享内存传路径），
// 支持 UTF-8 / UTF-8 BOM / ANSI(GBK) / UTF-16LE / UTF-16BE，并做热重载。
static char g_file_bytes[32768];
static const int kWideBuffer = 16384;
static wchar_t g_file_wide[kWideBuffer];

static void parse_line(const wchar_t* line, int length)
{
	while (length > 0 && (line[length - 1] == L'\r' || line[length - 1] == L' ' || line[length - 1] == L'\t'))
		--length;
	while (length > 0 && (line[0] == L' ' || line[0] == L'\t'))
	{
		++line;
		--length;
	}
	if (length <= 0)
		return;

	if (line[0] == L'#')
	{
		// 指令行：#addcjk
		char directive[64] = {};
		int n = 0;
		for (int i = 1; i < length && n < 63; ++i)
		{
			wchar_t c = line[i];
			if (c >= L'A' && c <= L'Z')
				directive[n++] = static_cast<char>(c - L'A' + 'a');
			else if (c > 0 && c < 0x7F)
				directive[n++] = static_cast<char>(c);
		}

		const char* needle = "addcjk";
		for (int i = 0; directive[i]; ++i)
		{
			int k = 0;
			while (needle[k] && directive[i + k] == needle[k])
				++k;
			if (!needle[k])
			{
				g_add_cjk = true;
				break;
			}
		}
		return;
	}

	int eq = -1;
	for (int i = 0; i < length; ++i)
	{
		if (line[i] == L'=')
		{
			eq = i;
			break;
		}
	}
	if (eq <= 0 || eq + 1 >= length)
		return;

	wchar_t from[kFromMax] = {};
	wchar_t to[kToMax] = {};

	int n = 0;
	for (int i = 0; i < eq && n + 1 < kFromMax; ++i)
		from[n++] = line[i];
	while (n > 0 && (from[n - 1] == L' ' || from[n - 1] == L'\t'))
		from[--n] = 0;

	int begin = eq + 1;
	while (begin < length && (line[begin] == L' ' || line[begin] == L'\t'))
		++begin;

	n = 0;
	for (int i = begin; i < length && n + 1 < kToMax; ++i)
		to[n++] = line[i];
	while (n > 0 && (to[n - 1] == L' ' || to[n - 1] == L'\t'))
		to[--n] = 0;

	add_entry(from, to);
}

static void load_map_bytes(const unsigned char* bytes, unsigned size)
{
	if (!bytes || !size)
		return;

	int offset = 0;
	int wide_length = -1;
	const char* kind = "utf-8";

	const unsigned char b0 = bytes[0];
	const unsigned char b1 = size > 1 ? bytes[1] : 0;

	if (size >= 2 && b0 == 0xFF && b1 == 0xFE)
	{
		kind = "utf-16le";
		offset = 2;
		wide_length = static_cast<int>((size - 2) / 2);
		if (wide_length > kWideBuffer - 1)
			wide_length = kWideBuffer - 1;
		for (int i = 0; i < wide_length; ++i)
		{
			g_file_wide[i] = static_cast<wchar_t>(
				bytes[2 + i * 2] | (bytes[3 + i * 2] << 8));
		}
	}
	else if (size >= 2 && b0 == 0xFE && b1 == 0xFF)
	{
		kind = "utf-16be";
		offset = 2;
		wide_length = static_cast<int>((size - 2) / 2);
		if (wide_length > kWideBuffer - 1)
			wide_length = kWideBuffer - 1;
		for (int i = 0; i < wide_length; ++i)
		{
			g_file_wide[i] = static_cast<wchar_t>(
				(bytes[2 + i * 2] << 8) | bytes[3 + i * 2]);
		}
	}
	else
	{
		if (size >= 3 && b0 == 0xEF && b1 == 0xBB && bytes[2] == 0xBF)
		{
			kind = "utf-8 bom";
			offset = 3;
		}

		wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			reinterpret_cast<const char*>(bytes) + offset,
			static_cast<int>(size) - offset, g_file_wide, kWideBuffer - 1);

		if (wide_length <= 0)
		{
			kind = "ansi/gbk";
			wide_length = MultiByteToWideChar(CP_ACP, 0,
				reinterpret_cast<const char*>(bytes) + offset,
				static_cast<int>(size) - offset, g_file_wide, kWideBuffer - 1);
		}
	}

	if (wide_length <= 0)
	{
		log_text("map decode failed (utf-8/ansi/utf-16)");
		return;
	}

	int start = 0;
	for (int i = 0; i <= wide_length; ++i)
	{
		if (i == wide_length || g_file_wide[i] == L'\n')
		{
			parse_line(g_file_wide + start, i - start);
			start = i + 1;
		}
	}

	char buffer[128];
	char* p = log_append(buffer, "map loaded (");
	p = log_append(p, kind);
	p = log_append(p, "), entries=");
	p = log_append_dec(p, static_cast<unsigned>(g_pair_count));
	log_line(buffer, p);
}

static void load_map_file(const wchar_t* path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (file == INVALID_HANDLE_VALUE)
	{
		log_text("map file not found, using embedded/built-in table");
		return;
	}

	DWORD read = 0;
	const BOOL ok = ReadFile(file, g_file_bytes, sizeof(g_file_bytes) - 1, &read, 0);
	CloseHandle(file);

	if (!ok || !read)
	{
		log_text("map file exists but is empty/unreadable");
		return;
	}

	load_map_bytes(reinterpret_cast<const unsigned char*>(g_file_bytes), read);
}

static void map_file_path(wchar_t* path, int capacity)
{
	if (!path || capacity <= 1)
		return;

	path[0] = 0;
	const DWORD n = GetEnvironmentVariableW(L"SKEET_TR_FILE", path, static_cast<DWORD>(capacity));
	if (n > 0 && n < static_cast<DWORD>(capacity))
		return;

	if (g_shared_map[0])
	{
		int i = 0;
		for (; g_shared_map[i] && i + 1 < capacity; ++i)
			path[i] = g_shared_map[i];
		path[i] = 0;
		return;
	}

	const wchar_t* fallback = L"C:\\skeet_tr.txt";
	int i = 0;
	for (; fallback[i] && i + 1 < capacity; ++i)
		path[i] = fallback[i];
	path[i] = 0;
}

// ---------------------------------------------------------------- 共享内存读
static const unsigned kShareMagic = 0x52544B53u;   // 'SKTR'

struct tr_share_t
{
	unsigned magic;
	unsigned map_length;
	unsigned log_length;
	unsigned data_size;
	wchar_t  map_path[260];
	wchar_t  log_path[260];
	char     map_data[32768];
};

static void copy_wide(wchar_t* dst, int capacity, const wchar_t* src, unsigned length)
{
	if (!dst || capacity <= 0)
		return;
	if (static_cast<int>(length) >= capacity)
		length = static_cast<unsigned>(capacity - 1);
	for (unsigned i = 0; i < length; ++i)
		dst[i] = src[i];
	dst[length] = 0;
}

static void read_shared_paths()
{
	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\skeet_tr_cfg");
	if (!mapping)
		return;

	const tr_share_t* view = static_cast<const tr_share_t*>(
		MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(tr_share_t)));

	if (view && view->magic == kShareMagic)
	{
		copy_wide(g_shared_map, MAX_PATH, view->map_path, view->map_length);
		copy_wide(g_shared_log, MAX_PATH, view->log_path, view->log_length);

		g_embedded_size = view->data_size;
		if (g_embedded_size > sizeof(g_embedded_map))
			g_embedded_size = static_cast<unsigned>(sizeof(g_embedded_map));
		for (unsigned i = 0; i < g_embedded_size; ++i)
			g_embedded_map[i] = static_cast<unsigned char>(view->map_data[i]);
	}

	if (view)
		UnmapViewOfFile(view);
	CloseHandle(mapping);
}

// ---------------------------------------------------------------- 热重载
static FILETIME g_map_stamp = {};
static unsigned g_map_check_tick = 0;

static void remember_map_stamp()
{
	WIN32_FILE_ATTRIBUTE_DATA data = {};
	if (GetFileAttributesExW(g_map_path, GetFileExInfoStandard, &data))
		g_map_stamp = data.ftLastWriteTime;
}

static void reload_table()
{
	seed_builtin();

	// 优先使用注入器内嵌并通过共享内存传来的翻译表
	if (g_embedded_size)
		load_map_bytes(g_embedded_map, g_embedded_size);

	// 外部文件存在时仍然加载，可做热更新/覆盖
	load_map_file(g_map_path);
	build_translation_index();
	remember_map_stamp();
}

static void tr_maybe_reload()
{
	const unsigned now = GetTickCount();
	if (now - g_map_check_tick < 500)
		return;
	g_map_check_tick = now;

	if (!g_map_path[0])
		return;

	WIN32_FILE_ATTRIBUTE_DATA data = {};
	if (!GetFileAttributesExW(g_map_path, GetFileExInfoStandard, &data))
		return;

	if (data.ftLastWriteTime.dwLowDateTime == g_map_stamp.dwLowDateTime &&
		data.ftLastWriteTime.dwHighDateTime == g_map_stamp.dwHighDateTime)
		return;

	reload_table();
}

// ---------------------------------------------------------------- 等待与挂钩
static bool image_ready()
{
	if (!page_readable(reinterpret_cast<const void*>(kProbeVa), 1))
		return false;
	if (*(volatile unsigned char*)kProbeVa != 0x55)
		return false;

	if (!page_readable(reinterpret_cast<const void*>(kVtable), sizeof(unsigned)))
		return false;

	const unsigned first = *(volatile unsigned*)kVtable;
	return first >= kImageBase && first < kImageEnd;
}

static DWORD WINAPI init_thread(LPVOID)
{
	read_shared_paths();
	map_file_path(g_map_path, MAX_PATH);
	open_log();
	install_crash_handler();
	reload_table();

	{
		char buffer[160];
		char* p = log_append(buffer, "entries=");
		p = log_append_dec(p, static_cast<unsigned>(g_pair_count));
		p = log_append(p, ", waiting for image...");
		log_line(buffer, p);
	}

	int waited = 0;
	while (!image_ready() && waited < 60000)
	{
		Sleep(50);
		waited += 50;
	}

	if (!image_ready())
	{
		log_pair("timeout, waited(ms)=", static_cast<unsigned>(waited), 0);
		return 0;
	}

	void* previous = *(void**)kTextSlot;
	log_pair("image ready, slot=", kTextSlot, static_cast<unsigned>(reinterpret_cast<unsigned>(previous)));

	// 引擎创建字体之前先挂上 create_font，CJK 槽的 face 换成中文
	install_create_font_hook();
	// resolver 换边冷却（默认关闭，需要 resolver_hook.flag）
	install_resolver_hook();
	// 诊断用的 vtable / 纹理绑定观测已关闭（会拖慢并在日志里刷屏）

	if (previous == reinterpret_cast<void*>(&tr_text_hook))
	{
		log_text("already hooked");
		return 0;
	}

	DWORD old_protect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(kTextSlot), sizeof(void*),
		PAGE_READWRITE, &old_protect))
	{
		log_pair("VirtualProtect failed, gle=", GetLastError(), 0);
		return 0;
	}

	g_tr_orig = previous;
	*(void**)kTextSlot = reinterpret_cast<void*>(&tr_text_hook);

	VirtualProtect(reinterpret_cast<void*>(kTextSlot), sizeof(void*), old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kTextSlot), sizeof(void*));

	log_pair("hooked, hook=", (unsigned)&tr_text_hook, static_cast<unsigned>(reinterpret_cast<unsigned>(previous)));
	return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		CloseHandle(CreateThread(0, 0, init_thread, 0, 0, 0));
	}
	return TRUE;
}

// ---------------------------------------------------------------- 无 CRT 补丁
extern "C" void* __cdecl memset(void* dst, int value, unsigned count)
{
	unsigned char* p = static_cast<unsigned char*>(dst);
	while (count--)
		*p++ = static_cast<unsigned char>(value);
	return dst;
}

extern "C" void* __cdecl memcpy(void* dst, const void* src, unsigned count)
{
	unsigned char* d = static_cast<unsigned char*>(dst);
	const unsigned char* s = static_cast<const unsigned char*>(src);
	while (count--)
		*d++ = *s++;
	return dst;
}
