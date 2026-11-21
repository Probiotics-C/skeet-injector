#include <Windows.h>
#include <winreg.h>
#include <string>
#include <thread>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwchar>
#include <vector>
#include <shellapi.h>

#include <d3d9.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx9.h"

#include "loader.h"
#include "resource.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// imgui_impl_win32.h 中该声明被官方放在 #if 0 里, 按官方说明在此自行前置声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------- 翻译表路径共享
// 注入进 csgo.exe 的 skeet_tr.dll 拿不到 debug.exe 所在目录，这里把
// "<debug.exe 目录>\skeet_tr.txt" 通过命名共享内存传给它。
struct tr_share_t
{
	unsigned magic;
	unsigned map_length;   // wchar 个数
	unsigned log_length;
	unsigned data_size;    // 内嵌翻译表字节数
	wchar_t  map_path[260];
	wchar_t  log_path[260];
	char     map_data[32768];
};

static HANDLE g_tr_share = nullptr;

static void publish_translation_path()
{
	wchar_t exe[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
		return;

	std::wstring dir(exe);
	const std::size_t slash = dir.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return;
	dir = dir.substr(0, slash + 1);

	const std::wstring map_path = dir + L"skeet_tr.txt";
	const std::wstring log_path = dir + L"skeet_tr.log";

	g_tr_share = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
		static_cast<DWORD>(sizeof(tr_share_t)), L"Local\\skeet_tr_cfg");
	if (!g_tr_share)
		return;

	void* view = MapViewOfFile(g_tr_share, FILE_MAP_WRITE, 0, 0, sizeof(tr_share_t));
	if (!view)
		return;

	tr_share_t* share = static_cast<tr_share_t*>(view);
	share->magic = 0x52544B53u;   // 'SKTR'
	share->map_length = static_cast<unsigned>(map_path.size());
	share->log_length = static_cast<unsigned>(log_path.size());
	wcsncpy_s(share->map_path, map_path.c_str(), _TRUNCATE);
	wcsncpy_s(share->log_path, log_path.c_str(), _TRUNCATE);

	// 把内嵌的翻译表直接放进共享内存：DLL 注入后无需外部文件即可汉化
	const std::vector<std::uint8_t> map = loader::embedded_tr_map();
	share->data_size = static_cast<unsigned>(map.size());
	if (share->data_size > sizeof(share->map_data))
		share->data_size = static_cast<unsigned>(sizeof(share->map_data));
	for (unsigned i = 0; i < share->data_size; ++i)
		share->map_data[i] = static_cast<char>(map[i]);

	UnmapViewOfFile(view);
}

// ---------------------------------------------------------------- 布局常量
static const int kSmallWidth = 373;
static const int kSmallHeight = 261;

static const int kMenuWidth = 654;
static const int kMenuHeight = 503;

static const float kFrameInset = 10.0f;    // 外框厚度
static const float kHiInset = 8.5f;        // 内圈高光线

// 菜单配色(贴近原版: 深灰黑底 + 亮白粗体 + 黄绿标题)
static const ImU32 kColorFrame = IM_COL32(30, 30, 30, 255);
static const ImU32 kColorHighlight = IM_COL32(50, 50, 50, 255);
static const ImU32 kColorContent = IM_COL32(14, 14, 14, 255);

static const int kGradC0[3] = { 96, 148, 180 };
static const int kGradC1[3] = { 167, 80, 165 };
static const int kGradC2[3] = { 186, 199, 99 };
static const float kBarAlphas[4] = { 0.30f, 1.00f, 0.88f, 0.30f };

static const ImU32 kColorStatus = IM_COL32(160, 160, 160, 255);
static const ImU32 kColorSuccess = IM_COL32(150, 200, 110, 255);
static const ImU32 kColorError = IM_COL32(220, 90, 90, 255);
static const ImU32 kColorCorner = IM_COL32(110, 110, 110, 255);

static const ImU32 kPanelBg = IM_COL32(35, 35, 35, 255);
static const ImU32 kPanelBorder = IM_COL32(51, 51, 51, 255);
static const ImU32 kContentBorder = IM_COL32(40, 40, 40, 255);
static const ImU32 kTitleGreen = IM_COL32(112, 174, 22, 255);   // #70AE16
static const ImU32 kTextWhite = IM_COL32(220, 220, 220, 255);
static const ImU32 kTextMain = IM_COL32(160, 160, 160, 255);
static const ImU32 kTextDim = IM_COL32(190, 190, 190, 255);
static const ImU32 kLinkColor = IM_COL32(74, 158, 255, 255);

static const ImU32 kButtonBg = IM_COL32(41, 41, 41, 255);
static const ImU32 kButtonBgHover = IM_COL32(37, 37, 37, 255);
static const ImU32 kButtonBgActive = IM_COL32(24, 24, 24, 255);
static const ImU32 kButtonBorder = IM_COL32(44, 44, 44, 255);
static const ImU32 kButtonBorderHover = IM_COL32(58, 58, 58, 255);

static const float kFontMainSize = 18.0f;    // 按钮/状态文字(粗体)
static const float kFontSmallSize = 13.0f;   // 角落小字(常规)
static const float kFontTextSize = 17.0f;    // 状态面板正文(常规)
static const float kFontTitleSize = 17.0f;   // 标题(Segoe UI Black)
static const float kFontLabelSize = 16.0f;   // Options/Status/勾选标签(粗体)
static const float kFontSplashSize = 20.0f;  // 第一层 loading(粗体)

// ---------------------------------------------------------------- 全局状态
static LPDIRECT3D9 g_d3d = nullptr;
static LPDIRECT3DDEVICE9 g_device = nullptr;
static D3DPRESENT_PARAMETERS g_params = {};
static HWND g_hwnd = nullptr;
static bool g_device_lost = false;
static bool g_running = true;

static ImFont* g_font_main = nullptr;    // 粗体 17
static ImFont* g_font_small = nullptr;   // 常规 12
static ImFont* g_font_text = nullptr;    // 常规 15
static ImFont* g_font_title = nullptr;   // 粗体 17
static ImFont* g_font_label = nullptr;   // 粗体 15
static ImFont* g_font_splash = nullptr;  // 粗体 26(第一层 loading)

static LPDIRECT3DTEXTURE9 g_icon_texture = nullptr;

enum class Stage { Waiting, Injecting, Injected, Failed };
static std::atomic<int> g_stage{ (int)Stage::Waiting };
static std::string g_fail_detail;

// ---------------------------------------------------------------- 三层界面
enum class Screen { Splash, Menu, Busy };

static Screen g_screen = Screen::Splash;
static double g_screen_start = 0.0;
static bool g_timeline_started = false;

static bool g_transition_active = false;
static double g_transition_start = 0.0;
static Screen g_transition_target = Screen::Menu;
static bool g_transition_switched = false;
static float g_fade_alpha = 0.0f;

static const double kTransitionDuration = 0.45;

static bool g_started_injection = false;
static std::atomic<int> g_inject_generation{ 0 };

static bool g_auto_launch = false;
static bool g_title_highlighted = false;
static bool g_status_selected[5] = { false, false, false, false, false };
static char g_updated_text[64] = "Updated 20/11/2026 19:12";

// 手动拖动(内容区空白处按住即可移动窗口)
static bool g_manual_drag = false;
static POINT g_drag_cursor = {};
static POINT g_drag_window = {};

static void begin_transition(Screen target)
{
	if (g_transition_active)
		return;

	g_transition_active = true;
	g_transition_start = ImGui::GetTime();
	g_transition_target = target;
	g_transition_switched = false;
}

static void resize_window(int width, int height);

static void update_transition()
{
	if (!g_transition_active)
	{
		g_fade_alpha = 0.0f;
		return;
	}

	const double t = (ImGui::GetTime() - g_transition_start) / kTransitionDuration;

	if (t < 0.5)
	{
		g_fade_alpha = (float)(t * 2.0);
	}
	else
	{
		if (!g_transition_switched)
		{
			g_transition_switched = true;
			g_screen = g_transition_target;
			g_screen_start = ImGui::GetTime();
			if (g_screen == Screen::Menu)
				resize_window(kMenuWidth, kMenuHeight);
			else
				resize_window(kSmallWidth, kSmallHeight);
		}
		g_fade_alpha = (float)((1.0 - t) * 2.0);
	}

	if (t >= 1.0)
	{
		g_transition_active = false;
		g_fade_alpha = 0.0f;
	}
}

// ---------------------------------------------------------------- 注入线程
static bool process_alive(DWORD pid)
{
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!process)
		return false;
	const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
	CloseHandle(process);
	return alive;
}


// ---------------------------------------------------------------- 诊断日志
// debug_log.txt 与 debug.exe 同目录, 每次点击 Load 重写。
// 只记录注入流程(不含作弊 DLL 内部日志), 出问题时据此定位。
static const bool kEnableInjectionLog = false;   // 关闭 debug_log.txt
static CRITICAL_SECTION g_log_cs;
static bool g_log_cs_ready = false;

static std::wstring log_file_path()
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring file(path);
	const std::size_t slash = file.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		file = file.substr(0, slash + 1);
	file += L"debug_log.txt";
	return file;
}

static void init_log()
{
	if (!kEnableInjectionLog)
		return;
	if (g_log_cs_ready)
		return;
	InitializeCriticalSection(&g_log_cs);
	g_log_cs_ready = true;

	const std::wstring file = log_file_path();
	FILE* handle = _wfopen(file.c_str(), L"w, ccs=UTF-8");
	if (handle)
	{
		fwprintf(handle, L"gamesense debug.exe injection log\n");
		fclose(handle);
	}
}

static void write_log(const char* text)
{
	if (!kEnableInjectionLog || !g_log_cs_ready)
		return;

	EnterCriticalSection(&g_log_cs);

	const std::wstring file = log_file_path();
	FILE* handle = _wfopen(file.c_str(), L"a, ccs=UTF-8");
	if (handle)
	{
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		fwprintf(handle, L"[%02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
		wchar_t wide[2048] = {};
		if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, _countof(wide)) > 1)
			fwprintf(handle, L"%s\n", wide);
		else
			fwprintf(handle, L"(bad utf8)\n");
		fclose(handle);
	}

	LeaveCriticalSection(&g_log_cs);
}

static void logf_(const char* format, ...)
{
	char buffer[1024] = {};
	va_list args;
	va_start(args, format);
	vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
	va_end(args);
	write_log(buffer);
}

static void StartInjection(bool auto_launch)
{
	if (g_started_injection)
		return;
	g_started_injection = true;

	g_stage.store((int)Stage::Waiting);
	g_fail_detail.clear();

	init_log();
	logf_("=== StartInjection(auto_launch=%d) ===", auto_launch ? 1 : 0);

	const int generation = g_inject_generation.load();
	std::thread([auto_launch, generation]()
	{
		bool launch_attempted = false;   // 自动拉起只做一次, 失败后等用户手动开
		DWORD tracked_pid = 0;
		bool base_reserved = false;

		for (;;)
		{
			if (!g_running || generation != g_inject_generation.load())
				return;

			const DWORD pid = loader::find_csgo_pid();

			// ---- 没有 csgo.exe: 一直等, 用户随时手动启动都算 ----
			if (!pid)
			{
				if (tracked_pid)
				{
					logf_("csgo pid %lu exited", tracked_pid);
					tracked_pid = 0;
					base_reserved = false;
				}

				if (auto_launch && !launch_attempted)
				{
					launch_attempted = true;
					const std::string error = loader::launch_csgo();
					logf_("launch_csgo -> %s", error.empty() ? "ok" : error.c_str());
					if (!error.empty())
						g_fail_detail = error;
				}

				g_stage.store((int)Stage::Waiting);
				Sleep(500);
				continue;
			}

			if (pid != tracked_pid)
			{
				tracked_pid = pid;
				base_reserved = false;
				logf_("csgo pid=%lu", pid);
			}

			// ---- 抢占 cheat 基址(一发现进程就抢) ----
			if (!base_reserved)
			{
				const std::string error = loader::reserve_cheat_base(pid);
				logf_("reserve 0x43310000 -> %s", error.empty() ? "ok" : error.c_str());
				if (!error.empty())
				{
					// 这个实例已经用掉 0x43310000: 提示并等它退出, 下次自动重试
					g_fail_detail = error;
					g_stage.store((int)Stage::Waiting);
					while (process_alive(pid) && g_running && generation == g_inject_generation.load())
						Sleep(500);
					tracked_pid = 0;
					continue;
				}
				base_reserved = true;
				g_fail_detail.clear();
			}

			// ---- 等主窗口: 卡在 Steam 报错/加载中的进程最多等 20 秒就放弃 ----
			bool ready = false;
			const ULONGLONG window_start = GetTickCount64();
			for (;;)
			{
				if (!g_running || generation != g_inject_generation.load())
					return;
				if (!process_alive(pid))
					break;
				if (loader::wait_for_game_ready(pid, 1000))
				{
					ready = true;
					break;
				}

				const ULONGLONG elapsed = GetTickCount64() - window_start;
				if (elapsed > 20000 && !loader::module_present(pid, "engine.dll"))
					break;
				if (elapsed > 120000)
					break;
			}

			logf_("window wait %llu ms ready=%d alive=%d engine=%d client=%d dx9=%d serverbrowser=%d",
				(unsigned long long)(GetTickCount64() - window_start),
				ready ? 1 : 0,
				process_alive(pid) ? 1 : 0,
				loader::module_present(pid, "engine.dll") ? 1 : 0,
				loader::module_present(pid, "client.dll") ? 1 : 0,
				loader::module_present(pid, "shaderapidx9.dll") ? 1 : 0,
				loader::module_present(pid, "serverbrowser.dll") ? 1 : 0);

			if (!ready || !process_alive(pid))
			{
				// 没进游戏画面或已退出: 释放预留, 等它退出后继续等下一个实例
				g_fail_detail = "game not ready - waiting for csgo";
				g_stage.store((int)Stage::Waiting);
				if (process_alive(pid))
					loader::release_cheat_base(pid);
				logf_("abandon this instance, release base");
				while (process_alive(pid) && g_running && generation == g_inject_generation.load())
					Sleep(500);
				tracked_pid = 0;
				base_reserved = false;
				continue;
			}

			Sleep(1500);
			if (!process_alive(pid))
			{
				tracked_pid = 0;
				base_reserved = false;
				continue;
			}

			g_stage.store((int)Stage::Injecting);
			logf_("injecting...");

			std::vector<std::uint8_t> image = loader::embedded_dll();
			logf_("embedded skeet.dll size=%u", (unsigned)image.size());
			if (image.empty())
			{
				g_fail_detail = "failed to read embedded skeet.dll";
				g_stage.store((int)Stage::Failed);
				return;
			}

			const ULONGLONG inject_start = GetTickCount64();
			std::string error = loader::inject_dll(pid, image, L"main");
			logf_("inject_dll -> %s (took %llu ms)",
				error.empty() ? "ok" : error.c_str(),
				(unsigned long long)(GetTickCount64() - inject_start));
			if (!error.empty())
			{
				g_fail_detail = error;
				g_stage.store((int)Stage::Waiting);
				while (process_alive(pid) && g_running && generation == g_inject_generation.load())
					Sleep(500);
				tracked_pid = 0;
				base_reserved = false;
				continue;
			}

			// ---- 可选的翻译模块(RCDATA 103): 失败只记日志, 不影响主流程 ----
			// 它自己会等 0x4331E000 变成 0x55 再挂 renderer 虚表 +0x3C。
			const std::vector<std::uint8_t> tr_image = loader::embedded_tr_dll();
			if (tr_image.empty())
			{
				logf_("embedded skeet_tr.dll not present - skip translation");
			}
			else
			{
				logf_("embedded skeet_tr.dll size=%u", (unsigned)tr_image.size());
				const ULONGLONG tr_start = GetTickCount64();
				const std::string tr_error = loader::inject_dll(pid, tr_image, L"tr");
				logf_("inject_dll(skeet_tr) -> %s (took %llu ms)",
					tr_error.empty() ? "ok" : tr_error.c_str(),
					(unsigned long long)(GetTickCount64() - tr_start));
			}

			// ---- 校验 skeet_t::map() 是否真的写进 0x43310000 ----
			bool mapped = false;
			std::uint64_t nonzero = 0;
			for (int i = 0; i < 60; ++i)
			{
				if (generation != g_inject_generation.load())
					return;
				if (!process_alive(pid))
					break;

				std::uint64_t count = 0;
				if (loader::cheat_image_stats(pid, &count))
				{
					nonzero = count;
					if (count > 0x1000)
					{
						mapped = true;
						break;
					}
				}
				Sleep(250);
			}

			logf_("post-check mapped=%d nonzero=%llu alive=%d serverbrowser=%d",
				mapped ? 1 : 0, (unsigned long long)nonzero, process_alive(pid) ? 1 : 0,
				loader::module_present(pid, "serverbrowser.dll") ? 1 : 0);

			if (!process_alive(pid))
			{
				g_fail_detail = "game closed right after injection";
				g_stage.store((int)Stage::Waiting);
				tracked_pid = 0;
				base_reserved = false;
				continue;
			}
			if (!mapped)
			{
				g_fail_detail = "dll loaded, image not mapped";
				g_stage.store((int)Stage::Waiting);
				while (process_alive(pid) && g_running && generation == g_inject_generation.load())
					Sleep(500);
				tracked_pid = 0;
				base_reserved = false;
				continue;
			}

			logf_("injection complete");
			g_stage.store((int)Stage::Injected);
			Sleep(4000);
			if (g_hwnd)
				PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
			return;
		}
	}).detach();
}

// ---------------------------------------------------------------- 绘制工具
static ImU32 gradient_color(int index, float alpha)
{
	const int* c = index == 0 ? kGradC0 : (index == 1 ? kGradC1 : kGradC2);
	return IM_COL32(c[0], c[1], c[2], (int)(alpha * 255.0f + 0.5f));
}

static ImVec2 text_extent(ImFont* font, float size, const char* text)
{
	return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

static void draw_frame(ImDrawList* draw)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;

	draw->AddRectFilled({ 0.0f, 0.0f }, size, kColorFrame);
	draw->AddRect({ kHiInset, kHiInset }, { size.x - kHiInset - 1.0f, size.y - kHiInset - 1.0f }, kColorHighlight);
	draw->AddRectFilled({ kFrameInset, kFrameInset }, { size.x - kFrameInset, size.y - kFrameInset }, kColorContent);

	const float left = kFrameInset;
	const float right = size.x - kFrameInset;
	const float mid = (left + right) * 0.5f;
	for (int i = 0; i < 4; i++)
	{
		const float y0 = kFrameInset + (float)i;
		const float y1 = y0 + 1.0f;
		const float alpha = kBarAlphas[i];

		const ImU32 c0 = gradient_color(0, alpha);
		const ImU32 c1 = gradient_color(1, alpha);
		const ImU32 c2 = gradient_color(2, alpha);

		draw->AddRectFilledMultiColor({ left, y0 }, { mid, y1 }, c0, c1, c1, c0);
		draw->AddRectFilledMultiColor({ mid, y0 }, { right, y1 }, c1, c2, c2, c1);
	}
}

static void small_exit_bounds(float& x0, float& y0, float& x1, float& y1)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float center_x = size.x * 0.5f;
	const float content_bottom = size.y - kFrameInset;
	const float width = 86.0f;
	const float height = 26.0f;
	x0 = center_x - width * 0.5f;
	y0 = content_bottom - 26.0f - height;
	x1 = x0 + width;
	y1 = y0 + height;
}

static void draw_small_exit_button(ImDrawList* draw)
{
	float x0, y0, x1, y1;
	small_exit_bounds(x0, y0, x1, y1);

	ImGui::SetCursorPos({ x0, y0 });
	const bool clicked = ImGui::InvisibleButton("##exit", { x1 - x0, y1 - y0 });
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();

	const ImU32 fill = held ? IM_COL32(24, 24, 24, 255)
		: (hovered ? IM_COL32(38, 38, 38, 255) : IM_COL32(29, 29, 29, 255));
	const ImU32 border = hovered ? IM_COL32(58, 58, 58, 255) : IM_COL32(44, 44, 44, 255);
	const ImU32 text_color = hovered ? IM_COL32(184, 184, 184, 255) : IM_COL32(168, 168, 168, 255);

	draw->AddRectFilled({ x0, y0 }, { x1, y1 }, fill);
	draw->AddRect({ x0, y0 }, { x1, y1 }, border);

	const char* label = "Exit";
	const ImVec2 extent = text_extent(g_font_main, kFontMainSize, label);
	draw->AddText(g_font_main, kFontMainSize,
		{ x0 + ((x1 - x0) - extent.x) * 0.5f, y0 + ((y1 - y0) - extent.y) * 0.5f },
		text_color, label);

	if (clicked)
		PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static void small_retry_bounds(float& x0, float& y0, float& x1, float& y1)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float center_x = size.x * 0.5f;
	const float content_bottom = size.y - kFrameInset;
	const float width = 86.0f;
	const float height = 26.0f;
	const float gap = 8.0f;
	x0 = center_x - 43.0f - gap - width;
	y0 = content_bottom - 26.0f - height;
	x1 = x0 + width;
	y1 = y0 + height;
}

static void draw_small_retry_button(ImDrawList* draw)
{
	float x0, y0, x1, y1;
	small_retry_bounds(x0, y0, x1, y1);

	ImGui::SetCursorPos({ x0, y0 });
	const bool clicked = ImGui::InvisibleButton("##retry", { x1 - x0, y1 - y0 });
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();

	const ImU32 fill = held ? IM_COL32(24, 24, 24, 255)
		: (hovered ? IM_COL32(38, 38, 38, 255) : IM_COL32(29, 29, 29, 255));
	const ImU32 border = hovered ? IM_COL32(58, 58, 58, 255) : IM_COL32(44, 44, 44, 255);
	const ImU32 text_color = hovered ? IM_COL32(184, 184, 184, 255) : IM_COL32(168, 168, 168, 255);

	draw->AddRectFilled({ x0, y0 }, { x1, y1 }, fill);
	draw->AddRect({ x0, y0 }, { x1, y1 }, border);

	const char* label = "Retry";
	const ImVec2 extent = text_extent(g_font_main, kFontMainSize, label);
	draw->AddText(g_font_main, kFontMainSize,
		{ x0 + ((x1 - x0) - extent.x) * 0.5f, y0 + ((y1 - y0) - extent.y) * 0.5f },
		text_color, label);

	if (clicked)
	{
		// 取消当前注入线程, 回到菜单重新 Load
		++g_inject_generation;
		g_started_injection = false;
		g_stage.store((int)Stage::Waiting);
		g_fail_detail.clear();
		begin_transition(Screen::Menu);
	}
}

static void draw_panel(ImDrawList* draw, ImVec2 p0, ImVec2 p1, const char* title, ImU32 background = kPanelBg, bool draw_border = true)
{
	draw->AddRectFilled(p0, p1, background);
	if (draw_border)
		draw->AddRect(p0, p1, kPanelBorder);

	if (title)
	{
		// 标题上移到面板顶边上, 用面板底色压住边框线 -> "标题嵌在边框里"的效果
		const ImVec2 extent = text_extent(g_font_label, kFontLabelSize, title);
		const ImVec2 position = { p0.x + 14.0f, p0.y - extent.y * 0.5f };
		draw->AddRectFilled({ position.x - 5.0f, position.y },
			{ position.x + extent.x + 5.0f, position.y + extent.y }, background);
		draw->AddText(g_font_label, kFontLabelSize, position, kTextWhite, title);
	}
}

static bool draw_button(ImDrawList* draw, const char* id, ImVec2 p0, ImVec2 p1, const char* label)
{
	ImGui::SetCursorPos(p0);
	const bool clicked = ImGui::InvisibleButton(id, { p1.x - p0.x, p1.y - p0.y });
	const bool hovered = ImGui::IsItemHovered();
	const bool held = ImGui::IsItemActive();

	draw->AddRectFilled(p0, p1, held ? kButtonBgActive : (hovered ? kButtonBgHover : kButtonBg));
	draw->AddRect(p0, p1, hovered ? kButtonBorderHover : kButtonBorder);

	const ImVec2 extent = text_extent(g_font_main, kFontMainSize, label);
	draw->AddText(g_font_main, kFontMainSize,
		{ p0.x + ((p1.x - p0.x) - extent.x) * 0.5f, p0.y + ((p1.y - p0.y) - extent.y) * 0.5f },
		kTextWhite, label);

	return clicked;
}

static bool draw_checkbox(ImDrawList* draw, const char* id, ImVec2 p0, float box, const char* label, bool& value)
{
	const ImVec2 p1 = { p0.x + box, p0.y + box };
	const ImVec2 label_extent = text_extent(g_font_label, kFontLabelSize, label);

	ImGui::SetCursorPos(p0);
	const bool clicked = ImGui::InvisibleButton(id, { (p1.x - p0.x) + 10.0f + label_extent.x, box });
	const bool hovered = ImGui::IsItemHovered();

	if (clicked)
		value = !value;

	draw->AddRectFilled(p0, p1, hovered ? IM_COL32(50, 50, 50, 255) : IM_COL32(41, 41, 41, 255));

	if (value)
	{
		const ImU32 tick = IM_COL32(235, 235, 235, 255);
		draw->AddLine({ p0.x + 4.0f, p0.y + box * 0.52f }, { p0.x + box * 0.42f, p0.y + box - 4.0f }, tick, 2.2f);
		draw->AddLine({ p0.x + box * 0.42f, p0.y + box - 4.0f }, { p0.x + box - 3.5f, p0.y + 4.0f }, tick, 2.2f);
	}

	draw->AddText(g_font_label, kFontLabelSize,
		{ p1.x + 10.0f, p0.y + (box - label_extent.y) * 0.5f }, kTextWhite, label);

	return clicked;
}

// ---------------------------------------------------------------- 图标
static bool find_csgo_exe(wchar_t* out, size_t out_count)
{
	const std::wstring path = loader::find_csgo_exe();
	if (path.empty())
		return false;
	wcsncpy_s(out, out_count, path.c_str(), _TRUNCATE);
	return true;
}

static bool load_icon_texture()
{
	if (g_icon_texture)
		return true;

	wchar_t exe_path[MAX_PATH * 2] = {};
	if (!find_csgo_exe(exe_path, _countof(exe_path)))
		return false;

	HICON icon = nullptr;
	if (ExtractIconExW(exe_path, 0, &icon, nullptr, 1) == 0 || !icon)
		return false;

	ICONINFO info = {};
	if (!GetIconInfo(icon, &info))
	{
		DestroyIcon(icon);
		return false;
	}

	BITMAP color_bitmap = {};
	if (!GetObject(info.hbmColor, sizeof(color_bitmap), &color_bitmap))
	{
		DeleteObject(info.hbmColor);
		DeleteObject(info.hbmMask);
		DestroyIcon(icon);
		return false;
	}

	const int width = color_bitmap.bmWidth;
	const int height = color_bitmap.bmHeight;
	if (width <= 0 || height <= 0 || width > 512 || height > 512)
	{
		DeleteObject(info.hbmColor);
		DeleteObject(info.hbmMask);
		DestroyIcon(icon);
		return false;
	}

	std::vector<std::uint8_t> pixels((size_t)width * height * 4, 0);
	bool has_alpha = false;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	HDC dc = CreateCompatibleDC(nullptr);
	if (dc && color_bitmap.bmBitsPixel == 32)
	{
		if (GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS))
		{
			for (size_t i = 3; i < pixels.size(); i += 4)
				if (pixels[i] != 0) { has_alpha = true; break; }
		}
	}

	if (!has_alpha && dc)
	{
		std::vector<std::uint8_t> mask((size_t)width * height * 4, 0);
		std::vector<std::uint8_t> color((size_t)width * height * 4, 0);
		BITMAPINFO mask_bmi = bmi;
		BITMAPINFO color_bmi = bmi;
		if (GetDIBits(dc, info.hbmMask, 0, height, mask.data(), &mask_bmi, DIB_RGB_COLORS) &&
			GetDIBits(dc, info.hbmColor, 0, height, color.data(), &color_bmi, DIB_RGB_COLORS))
		{
			const size_t count = (size_t)width * height;
			for (size_t i = 0; i < count; ++i)
			{
				pixels[i * 4 + 0] = color[i * 4 + 0];
				pixels[i * 4 + 1] = color[i * 4 + 1];
				pixels[i * 4 + 2] = color[i * 4 + 2];
				pixels[i * 4 + 3] = mask[i * 4] ? 0 : 255;
			}
		}
	}

	if (dc)
		DeleteDC(dc);

	if (info.hbmColor) DeleteObject(info.hbmColor);
	if (info.hbmMask) DeleteObject(info.hbmMask);
	DestroyIcon(icon);

	if (FAILED(g_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
		D3DPOOL_MANAGED, &g_icon_texture, nullptr)))
	{
		g_icon_texture = nullptr;
		return false;
	}

	D3DLOCKED_RECT locked = {};
	if (FAILED(g_icon_texture->LockRect(0, &locked, nullptr, 0)))
	{
		g_icon_texture->Release();
		g_icon_texture = nullptr;
		return false;
	}

	for (int y = 0; y < height; ++y)
		CopyMemory((std::uint8_t*)locked.pBits + (size_t)y * locked.Pitch,
			pixels.data() + (size_t)y * width * 4, (size_t)width * 4);

	g_icon_texture->UnlockRect(0);
	return true;
}

// ---------------------------------------------------------------- 第一层: 加载
static void draw_splash(ImDrawList* draw)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float center_x = size.x * 0.5f;
	const float center_y = (kFrameInset + (size.y - kFrameInset)) * 0.5f;

	int dots = 0;
	if (!g_transition_active)
	{
		const double elapsed = ImGui::GetTime() - g_screen_start;
		const int step = (int)(elapsed / 0.3);
		if (step >= 8)
		{
			begin_transition(Screen::Menu);
			dots = 0;
		}
		else
		{
			dots = step % 4;
		}
	}

	char text[32] = "loading";
	for (int i = 0; i < dots; ++i)
	{
		const size_t length = strlen(text);
		text[length] = '.';
		text[length + 1] = 0;
	}

	// "loading" 固定居中, 点号向右生长(避免整串文字随点数抖动)
	const ImVec2 base_extent = text_extent(g_font_main, kFontMainSize, "Loading");
	const float origin_x = center_x - base_extent.x * 0.5f;
	const float origin_y = center_y - base_extent.y * 0.5f;

	draw->AddText(g_font_main, kFontMainSize, { origin_x, origin_y }, kColorStatus, "Loading");

	if (dots > 0)
	{
		const char* dot_text = (dots == 1) ? "." : (dots == 2 ? ".." : "...");
		draw->AddText(g_font_main, kFontMainSize,
			{ origin_x + base_extent.x + 2.0f, origin_y }, kColorStatus, dot_text);
	}

	draw_small_exit_button(draw);
}

// ---------------------------------------------------------------- 第二层: 菜单
static void draw_menu(ImDrawList* draw)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;

	const float pad_x = 38.0f;
	const float top_y = 44.0f;
	const float gap = 12.0f;
	const float left_w = 356.0f;
	const float top_h = 214.0f;

	const ImVec2 left_p0 = { pad_x, top_y };
	const ImVec2 left_p1 = { pad_x + left_w, top_y + top_h };

	const ImVec2 opt_p0 = { left_p1.x + gap, top_y };
	const ImVec2 opt_p1 = { size.x - pad_x, top_y + top_h };

	const ImVec2 status_p0 = { pad_x, left_p1.y + gap };
	const ImVec2 status_p1 = { size.x - pad_x, size.y - 52.0f };

	// 内容区外框
	draw->AddRect({ pad_x - 12.0f, top_y - 12.0f }, { size.x - pad_x + 12.0f, size.y - 40.0f }, kContentBorder);

	// --- 左侧: 游戏信息 ---
	draw_panel(draw, left_p0, left_p1, nullptr);

	const float icon_px = 44.0f;
	const ImVec2 icon_p0 = { left_p0.x + 12.0f, left_p0.y + 12.0f };
	const ImVec2 icon_p1 = { icon_p0.x + icon_px, icon_p0.y + icon_px };

	const char* title_text = "Counter-Strike: Global Offensive";
	const ImVec2 title_extent = text_extent(g_font_title, kFontTitleSize, title_text);
	const ImVec2 title_pos = { icon_p1.x + 8.0f, icon_p0.y + 2.0f };

	// 图标+标题+周边边框整块: 点击切换高亮(标题变白 + 阴影层消失), 再次点击恢复
	const ImVec2 block_p0 = { left_p0.x + 6.0f, left_p0.y + 6.0f };
	const ImVec2 block_p1 = { left_p1.x - 6.0f, icon_p0.y + icon_px + 12.0f };
	ImGui::SetCursorPos(block_p0);
	ImGui::InvisibleButton("##title_block", { block_p1.x - block_p0.x, block_p1.y - block_p0.y });
	const bool title_hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemClicked())
		g_title_highlighted = !g_title_highlighted;

	// 鼠标悬停时总是出现阴影层级 #1C1C1C
	if (title_hovered)
		draw->AddRectFilled(block_p0, block_p1, IM_COL32(28, 28, 28, 255));

	if (g_icon_texture)
	{
		draw->AddImage((ImTextureID)(intptr_t)g_icon_texture, icon_p0, icon_p1);
	}
	else
	{
		draw->AddRectFilled(icon_p0, icon_p1, IM_COL32(198, 130, 40, 255), 4.0f);
		draw->AddText(g_font_main, kFontMainSize, { icon_p0.x + 11.0f, icon_p0.y + 16.0f },
			IM_COL32(30, 20, 10, 255), "CS");
	}

	draw->AddText(g_font_title, kFontTitleSize, title_pos,
		g_title_highlighted ? kTextMain : kTitleGreen, title_text);
	draw->AddText(g_font_text, kFontTextSize, { icon_p1.x + 8.0f, icon_p0.y + 26.0f },
		kTextDim, g_updated_text);

	// --- 右上: Options ---
	draw_panel(draw, opt_p0, opt_p1, "Options", IM_COL32(23, 23, 23, 255), false);

	const float button_left = opt_p0.x + 16.0f;
	const float button_right = opt_p1.x - 16.0f;
	const float button_height = 38.0f;

	if (draw_button(draw, "##load", { button_left, opt_p0.y + 40.0f },
		{ button_right, opt_p0.y + 40.0f + button_height }, "Load"))
	{
		if (!g_transition_active && g_screen == Screen::Menu)
		{
			StartInjection(g_auto_launch);
			begin_transition(Screen::Busy);
		}
	}

	if (draw_button(draw, "##exit", { button_left, opt_p0.y + 96.0f },
		{ button_right, opt_p0.y + 96.0f + button_height }, "Exit"))
		PostMessageW(g_hwnd, WM_CLOSE, 0, 0);

	draw_checkbox(draw, "##autolaunch", { button_left, opt_p0.y + 154.0f }, 18.0f, "Auto launch game", g_auto_launch);

	// --- 下方: Status ---
	draw_panel(draw, status_p0, status_p1, "Status", kPanelBg, false);

	// 状态文字底板 #2E2E2E
	const ImVec2 status_inner_p0 = { status_p0.x + 8.0f, status_p0.y + 12.0f };
	const ImVec2 status_inner_p1 = { status_p1.x - 8.0f, status_p0.y + 152.0f };
	draw->AddRectFilled(status_inner_p0, status_inner_p1, IM_COL32(46, 46, 46, 255));

	const char* lines[5] = {
		"Connected",
		"Welcome back, User",
		"Added \"-insecure\" in Counter-Strike: Global Offensive (params)",
		"Enjoy your hvh time",
		"Warning: Don't already start your Counter-Strike: Global Offensive!!!"
	};

	float line_y = status_p0.y + 18.0f;
	const float line_step = 22.0f;
	for (int i = 0; i < 5; ++i)
	{
		const ImVec2 extent = text_extent(g_font_text, kFontTextSize, lines[i]);
		const ImVec2 line_pos = { status_p0.x + 14.0f, line_y };

		char button_id[24];
		sprintf_s(button_id, "##status_line_%d", i);
		ImGui::SetCursorPos(line_pos);
		ImGui::InvisibleButton(button_id, { extent.x + 4.0f, kFontTextSize + 3.0f });

		if (ImGui::IsItemClicked())
			g_status_selected[i] = !g_status_selected[i];

		const bool highlighted = ImGui::IsItemHovered() || g_status_selected[i];

		// 阴影层级 #1C1C1C(悬停/选中时出现)
		if (highlighted)
		{
			draw->AddRectFilled({ line_pos.x - 5.0f, line_pos.y - 2.0f },
				{ status_p1.x - 8.0f, line_pos.y + kFontTextSize + 3.0f },
				IM_COL32(28, 28, 28, 255));
		}

		draw->AddText(g_font_text, kFontTextSize, line_pos,
			highlighted ? kTitleGreen : kTextMain, lines[i]);

		line_y += line_step;
	}

	const char* link = "join us on QQ";
	const ImVec2 link_extent = text_extent(g_font_text, kFontTextSize, link);
	ImGui::SetCursorPos({ status_p0.x + 14.0f, line_y });
	const bool link_clicked = ImGui::InvisibleButton("##qq", { link_extent.x, link_extent.y });
	const bool link_hovered = ImGui::IsItemHovered();

	draw->AddText(g_font_text, kFontTextSize, { status_p0.x + 14.0f, line_y },
		link_hovered ? IM_COL32(130, 195, 255, 255) : kLinkColor, link);

	if (link_hovered)
		SetCursor(LoadCursorW(nullptr, IDC_HAND));

	if (link_clicked)
		ShellExecuteA(nullptr, "open", "https://qm.qq.com/q/qEjDicANi0", nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- 第三层: 注入进度
static const char* status_text(Stage stage)
{
	switch (stage)
	{
	case Stage::Waiting:
		return g_fail_detail.empty() ? "Waiting for game" : g_fail_detail.c_str();
	case Stage::Injecting: return "csgo.exe found, injecting...";
	case Stage::Injected:  return "Injected, have fun!";
	default:               return g_fail_detail.c_str();
	}
}

static ImU32 status_color(Stage stage)
{
	switch (stage)
	{
	case Stage::Injected: return kColorSuccess;
	case Stage::Failed:   return kColorError;
	default:              return kColorStatus;
	}
}

static void draw_busy(ImDrawList* draw)
{
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	const float center_x = size.x * 0.5f;
	const float center_y = (kFrameInset + (size.y - kFrameInset)) * 0.5f;

	const Stage stage = (Stage)g_stage.load();
	const char* text = status_text(stage);
	ImU32 color = status_color(stage);

	if (stage != Stage::Failed)
	{
		const float phase = (float)ImGui::GetTime() * 2.2f;
		const float alpha = 0.5f + 0.5f * sinf(phase);
		const int pulsed = (int)(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
		color = (color & ~IM_COL32_A_MASK) | ((ImU32)pulsed << IM_COL32_A_SHIFT);
	}

	const ImVec2 extent = text_extent(g_font_main, kFontMainSize, text);
	draw->AddText(g_font_main, kFontMainSize,
		{ center_x - extent.x * 0.5f, center_y - extent.y * 0.5f }, color, text);

	if ((Stage)g_stage.load() == Stage::Failed)
		draw_small_retry_button(draw);

	draw_small_exit_button(draw);
}

// ---------------------------------------------------------------- 主绘制
static void draw_ui()
{
	update_transition();

	const ImVec2 size = ImGui::GetIO().DisplaySize;

	ImGui::SetNextWindowPos({ 0.0f, 0.0f });
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("##main", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw_frame(draw);

	{
		draw->AddText(g_font_small, kFontSmallSize,
			{ kFrameInset + 6.0f, kFrameInset + 6.0f }, kColorCorner, "Update:2026/11/20");

		const char* sign = "by ysj1337";
		const ImVec2 extent = text_extent(g_font_small, kFontSmallSize, sign);
		draw->AddText(g_font_small, kFontSmallSize,
			{ size.x - kFrameInset - 6.0f - extent.x, kFrameInset + 6.0f }, kColorCorner, sign);
	}

	switch (g_screen)
	{
	case Screen::Splash: draw_splash(draw); break;
	case Screen::Menu:   draw_menu(draw);   break;
	case Screen::Busy:   draw_busy(draw);   break;
	}

	if (g_fade_alpha > 0.001f)
	{
		const int alpha = (int)(g_fade_alpha * 255.0f + 0.5f);
		draw->AddRectFilled({ 0.0f, 0.0f }, size, IM_COL32(0, 0, 0, alpha));
	}

	// 内容区空白处按住即可拖动窗口(控件上不会触发)
	if (!g_manual_drag && ImGui::IsMouseClicked(0) &&
		!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
	{
		GetCursorPos(&g_drag_cursor);
		RECT window_rect = {};
		GetWindowRect(g_hwnd, &window_rect);
		g_drag_window.x = window_rect.left;
		g_drag_window.y = window_rect.top;
		g_manual_drag = true;
	}

	if (g_manual_drag)
	{
		if (!ImGui::IsMouseDown(0))
		{
			g_manual_drag = false;
		}
		else
		{
			POINT cursor = {};
			GetCursorPos(&cursor);
			SetWindowPos(g_hwnd, HWND_TOPMOST,
				g_drag_window.x + (cursor.x - g_drag_cursor.x),
				g_drag_window.y + (cursor.y - g_drag_cursor.y),
				0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
		}
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- 窗口尺寸
static bool g_window_placed = false;

static void resize_window(int width, int height)
{
	if (!g_hwnd)
		return;

	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_POPUP, FALSE);

	const int window_width = rect.right - rect.left;
	const int window_height = rect.bottom - rect.top;

	RECT work_area = {};
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0))
	{
		work_area.left = 0;
		work_area.top = 0;
		work_area.right = GetSystemMetrics(SM_CXSCREEN);
		work_area.bottom = GetSystemMetrics(SM_CYSCREEN);
	}

	int x = 0;
	int y = 0;

	if (!g_window_placed)
	{
		// 第一层: 默认屏幕中央
		x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
		y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
		g_window_placed = true;
	}
	else
	{
		// 后续层: 跟随上一层的位置, 只做工作区夹取, 不再回到中央
		RECT current = {};
		GetWindowRect(g_hwnd, &current);
		x = current.left;
		y = current.top;
		if (x + window_width > work_area.right) x = work_area.right - window_width;
		if (y + window_height > work_area.bottom) y = work_area.bottom - window_height;
		if (x < work_area.left) x = work_area.left;
		if (y < work_area.top) y = work_area.top;
	}

	SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, window_width, window_height, SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------- D3D9
static bool create_device(HWND hwnd)
{
	g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!g_d3d)
		return false;

	ZeroMemory(&g_params, sizeof(g_params));
	g_params.Windowed = TRUE;
	g_params.SwapEffect = D3DSWAPEFFECT_DISCARD;
	g_params.BackBufferFormat = D3DFMT_UNKNOWN;
	g_params.EnableAutoDepthStencil = TRUE;
	g_params.AutoDepthStencilFormat = D3DFMT_D16;
	g_params.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

	if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
		D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_params, &g_device)))
	{
		if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
			D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_params, &g_device)))
			return false;
	}
	return true;
}

static void reset_device()
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
	if (g_device->Reset(&g_params) == D3DERR_INVALIDCALL)
		return;
	ImGui_ImplDX9_CreateDeviceObjects();
}

static void destroy_device()
{
	if (g_icon_texture)
	{
		g_icon_texture->Release();
		g_icon_texture = nullptr;
	}
	if (g_device)
	{
		g_device->Release();
		g_device = nullptr;
	}
	if (g_d3d)
	{
		g_d3d->Release();
		g_d3d = nullptr;
	}
}

// ---------------------------------------------------------------- 窗口过程
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SIZE:
		if (g_device != nullptr && wParam != SIZE_MINIMIZED)
		{
			g_params.BackBufferWidth = LOWORD(lParam);
			g_params.BackBufferHeight = HIWORD(lParam);
			reset_device();
		}
		return 0;

	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		if ((wParam & 0xfff0) == SC_MINIMIZE)
			return 0;
		break;

	case WM_NCHITTEST:
	{
		// 四周的边框区域都可拖动窗口，中间内容区交给 ImGui 处理点击
		POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
		ScreenToClient(hWnd, &point);

		RECT client = {};
		GetClientRect(hWnd, &client);

		const LONG border = 18;
		if (point.x < border || point.y < border ||
			point.x >= client.right - border || point.y >= client.bottom - border)
			return HTCAPTION;

		return HTCLIENT;
	}

	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;

	case WM_DESTROY:
		g_running = false;
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- 字体加载
static ImFont* load_font(ImGuiIO& io, const char* path, float size)
{
	ImFont* font = io.Fonts->AddFontFromFileTTF(path, size);
	return font;
}

// ---------------------------------------------------------------- 入口
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	SetProcessDPIAware();

	// 先把 "<exe 目录>\skeet_tr.txt" 的路径发给待注入的 skeet_tr.dll
	publish_translation_path();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = L"gamesense_debug_class";

	HICON icon_big = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
	HICON icon_small = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	if (!icon_big)
		icon_big = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
	if (!icon_small)
		icon_small = icon_big;
	wc.hIcon = icon_big;
	wc.hIconSm = icon_small;

	if (!RegisterClassExW(&wc))
		return 1;

	const DWORD style = WS_POPUP;
	RECT rect = { 0, 0, kSmallWidth, kSmallHeight };
	AdjustWindowRect(&rect, style, FALSE);

	const int window_width = rect.right - rect.left;
	const int window_height = rect.bottom - rect.top;

	RECT work_area = {};
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0))
	{
		work_area.left = 0;
		work_area.top = 0;
		work_area.right = GetSystemMetrics(SM_CXSCREEN);
		work_area.bottom = GetSystemMetrics(SM_CYSCREEN);
	}
	const int window_x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
	const int window_y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;

	// WS_EX_TOPMOST: 始终保持最上层(不被 Steam 等窗口遮挡)
	g_hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"debug.exe", style,
		window_x, window_y, window_width, window_height,
		nullptr, nullptr, instance, nullptr);
	if (!g_hwnd)
		return 1;

	SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
	SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);

	if (!create_device(g_hwnd))
		return 1;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;

	g_font_main = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontMainSize);
	if (!g_font_main) g_font_main = io.Fonts->AddFontDefault();

	g_font_title = load_font(io, "C:\\Windows\\Fonts\\seguibl.ttf", kFontTitleSize);   // Segoe UI Black(更重, 更接近原版)
	if (!g_font_title) g_font_title = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontTitleSize);
	if (!g_font_title) g_font_title = g_font_main;

	g_font_label = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontLabelSize);
	if (!g_font_label) g_font_label = g_font_main;

	g_font_text = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontTextSize);
	if (!g_font_text) g_font_text = g_font_main;

	g_font_small = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontSmallSize);
	if (!g_font_small) g_font_small = g_font_text;

	g_font_splash = load_font(io, "C:\\Windows\\Fonts\\segoeuib.ttf", kFontSplashSize);
	if (!g_font_splash) g_font_splash = g_font_title;

	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX9_Init(g_device);

	load_icon_texture();

	ShowWindow(g_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hwnd);
	SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	MSG msg = {};
	while (g_running)
	{
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				g_running = false;
		}
		if (!g_running)
			break;

		if (IsIconic(g_hwnd))
			ShowWindow(g_hwnd, SW_RESTORE);

		if (g_device_lost)
		{
			if (SUCCEEDED(g_device->TestCooperativeLevel()))
			{
				reset_device();
				g_device_lost = false;
			}
			else
			{
				Sleep(10);
				continue;
			}
		}

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		if (!g_timeline_started)
		{
			g_timeline_started = true;
			g_screen_start = ImGui::GetTime();
		}

		draw_ui();

		ImGui::Render();

		g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
		g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

		g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
			D3DCOLOR_RGBA(14, 14, 14, 255), 1.0f, 0);

		if (g_device->BeginScene() >= 0)
		{
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			g_device->EndScene();
		}

		if (g_device->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST)
			g_device_lost = true;
	}

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	destroy_device();
	DestroyWindow(g_hwnd);

	return 0;
}
