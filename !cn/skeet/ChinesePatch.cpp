#include "pch.h"
#include "ChinesePatch.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace cn_patch {

namespace {

// ------------------------------------------------------------------ 配置

// 中文补丁文件名(CSGO 游戏根目录, 即 csgo.exe 所在目录)
const wchar_t kLanguageFileName[] = L"language.txt";
const wchar_t kFontFileName[] = L"GenSenMaruGothic TW TTF Medium.ttf";

// 需要被替换掉的字体名 —— gamesense 菜单原本的 CJK 字体(中文经该字体渲染)
const wchar_t kReplaceFaceFrom[] = L"Malgun Gothic";
// 替换为的字体全名 —— GenSenMaruGothic TW TTF Medium.ttf 的 Full Name
const wchar_t kReplaceFaceTo[] = L"GenSenMaruGothic TW TTF Medium";

// ------------------------------------------------------------------ 状态

struct translation_t {
    std::wstring source;   // 英文原文(ASCII)
    std::wstring target;   // 中文译文(UTF-16); target.size() <= source.size()
};

// 译文过长(放不进原文槽位)时的短译备选
struct fallback_t {
    const wchar_t* source;
    const wchar_t* target;
};
const fallback_t kShortFallbacks[] = {
    { L"Yaw",          L"\u5DE6\u53F3" },                 // 短译“左右”(原译“左右角度”放不进 3 字符槽)
    { L"Dump MM wins", L"\u6253\u5370MM\u80DC\u573A" },   // 短译“打印MM胜场”(原译过长)
};

bool g_initialized = false;
bool g_font_registered = false;
bool g_font_available = false;   // 字体可用(本次注册成功, 或系统里本来就有)
std::vector<translation_t> g_translations;

// ------------------------------------------------------------------ 工具

void trim(std::string& text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (unsigned char)text[begin] <= 0x20)
        ++begin;
    while (end > begin && (unsigned char)text[end - 1] <= 0x20)
        --end;
    text = text.substr(begin, end - begin);
}

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty())
        return {};

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), (int)text.size(), nullptr, 0);
    if (length <= 0)
        length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring result((std::size_t)length, L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), result.data(), length);
    if (written <= 0)
        return {};

    result.resize((std::size_t)written);
    return result;
}

std::wstring module_directory(HMODULE module)
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(module, path, (DWORD)_countof(path));
    if (length == 0 || length >= _countof(path))
        return {};

    std::wstring result(path, length);
    const std::size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};

    result.resize(slash);
    return result;
}

bool file_exists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// ------------------------------------------------------------------ language.txt 读取

bool load_language(const std::wstring& path)
{
    std::string data;
    {
        std::ifstream file(path.c_str(), std::ios::binary);
        if (!file.is_open())
            return false;
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // 去掉可能的 UTF-8 BOM
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);

    std::size_t position = 0;
    while (position < data.size())
    {
        std::size_t eol = data.find('\n', position);
        if (eol == std::string::npos)
            eol = data.size();

        std::string line = data.substr(position, eol - position);
        position = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        trim(line);

        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
            continue;

        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        trim(key);
        trim(value);
        if (key.empty() || value.empty())
            continue;

        // 头两行是补丁元数据, 不是词条
        if (key == "language" || key == "compatibility")
            continue;

        // 英文原文必须是可打印 ASCII
        bool ascii = true;
        for (char character : key)
        {
            if ((unsigned char)character < 0x20 || (unsigned char)character > 0x7E)
            {
                ascii = false;
                break;
            }
        }
        if (!ascii)
            continue;

        std::wstring source(key.begin(), key.end());
        std::wstring target = utf8_to_wide(value);
        if (target.empty())
            continue;

        if (target.size() > source.size())
        {
            // 放不进原文槽位: 尝试短译备选, 否则跳过(保持英文, 避免越界损坏相邻字符串)
            bool replaced = false;
            for (const fallback_t& fallback : kShortFallbacks)
            {
                if (source == fallback.source && wcslen(fallback.target) <= source.size())
                {
                    target = fallback.target;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                continue;
        }

        g_translations.push_back({ std::move(source), std::move(target) });
    }

    return true;
}

// ------------------------------------------------------------------ 字符串替换

void translate_in_place(std::uint8_t* buffer, std::size_t size)
{
    if (!buffer || size == 0 || g_translations.empty())
        return;

    for (const translation_t& translation : g_translations)
    {
        const std::size_t characters = translation.source.size();
        const std::size_t slot = (characters + 1) * 2;   // 原文槽位(含结尾 00 00)
        if (slot > size)
            continue;

        const std::size_t target_bytes = translation.target.size() * 2;
        const wchar_t first = translation.source[0];
        const std::uint8_t first_low = (std::uint8_t)(first & 0xFF);
        const std::uint8_t first_high = (std::uint8_t)((first >> 8) & 0xFF);

        std::uint8_t* cursor = buffer;
        std::uint8_t* last = buffer + (size - slot);
        while (cursor <= last)
        {
            // 快速过滤: 首字符 + 槽位末尾必须是双字节零
            if (cursor[0] == first_low && cursor[1] == first_high &&
                cursor[slot - 2] == 0 && cursor[slot - 1] == 0 &&
                std::memcmp(cursor, translation.source.c_str(), characters * 2) == 0)
            {
                std::memcpy(cursor, translation.target.c_str(), target_bytes);
                std::memset(cursor + target_bytes, 0, slot - target_bytes);
                cursor += slot;
            }
            else
            {
                ++cursor;
            }
        }
    }
}

int CALLBACK face_enum_callback(const LOGFONTW* logfont, const TEXTMETRICW*, DWORD, LPARAM data)
{
    bool* found = reinterpret_cast<bool*>(data);
    if (logfont != nullptr && _wcsicmp(logfont->lfFaceName, kReplaceFaceTo) == 0)
    {
        *found = true;
        return 0;   // 找到一个就够了
    }
    return 1;
}

// 探测 "GenSenMaruGothic TW TTF Medium" 是否对 GDI 可用(本次 FR_PRIVATE 注册成功, 或系统里已安装)
bool probe_font_available()
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;

    LOGFONTW filter = {};
    filter.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(filter.lfFaceName, LF_FACESIZE, kReplaceFaceTo, _TRUNCATE);

    bool found = false;
    EnumFontFamiliesExW(dc, &filter, (FONTENUMPROCW)&face_enum_callback, (LPARAM)&found, 0);
    DeleteDC(dc);
    return found;
}

} // namespace

// ------------------------------------------------------------------ 初始化

void initialize(HMODULE self_module)
{
    if (g_initialized)
        return;
    g_initialized = true;

    std::vector<std::wstring> directories;
    const std::wstring game_directory = module_directory(nullptr);      // csgo.exe 目录
    if (!game_directory.empty())
        directories.push_back(game_directory);
    const std::wstring dll_directory = module_directory(self_module);   // skeet.dll 目录(备用)
    if (!dll_directory.empty() && dll_directory != game_directory)
        directories.push_back(dll_directory);

    // 读取 language.txt
    for (const std::wstring& directory : directories)
    {
        if (load_language(directory + L"\\" + kLanguageFileName))
            break;
    }

    // 注册中文补丁字体(进程内私有, 不污染系统字体表)
    for (const std::wstring& directory : directories)
    {
        const std::wstring font_path = directory + L"\\" + kFontFileName;
        if (file_exists(font_path))
        {
            g_font_registered = AddFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr) != 0;
            break;
        }
    }

    // 即使 FR_PRIVATE 注册失败, 字体也可能已被安装进系统 —— 探测一次实际可用性
    g_font_available = probe_font_available();
}

void patch_menu_strings(void* buffer, size_t size)
{
    translate_in_place(static_cast<std::uint8_t*>(buffer), size);
}

void patch_image_strings(void* buffer, size_t size)
{
    translate_in_place(static_cast<std::uint8_t*>(buffer), size);
}

bool active()
{
    return !g_translations.empty() || g_font_registered || g_font_available;
}

// ------------------------------------------------------------------ 字体创建包装

using create_font_indirect_w_t = HFONT(WINAPI*)(const LOGFONTW*);
create_font_indirect_w_t g_real_create_font_indirect_w = nullptr;

HFONT WINAPI hooked_CreateFontIndirectW(const LOGFONTW* logfont)
{
    if (!g_real_create_font_indirect_w)
    {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi)
            gdi = LoadLibraryW(L"gdi32.dll");
        if (!gdi)
            return nullptr;
        g_real_create_font_indirect_w = reinterpret_cast<create_font_indirect_w_t>(
            GetProcAddress(gdi, "CreateFontIndirectW"));
        if (!g_real_create_font_indirect_w)
            return nullptr;
    }

    // 已注册调色字体后, 把菜单的 CJK 字体换成中文补丁字体; 其余字体原样传递
    if (g_font_available && logfont != nullptr && _wcsicmp(logfont->lfFaceName, kReplaceFaceFrom) == 0)
    {
        LOGFONTW patched = *logfont;
        wcsncpy_s(patched.lfFaceName, LF_FACESIZE, kReplaceFaceTo, _TRUNCATE);
        return g_real_create_font_indirect_w(&patched);
    }

    return g_real_create_font_indirect_w(logfont);
}

} // namespace cn_patch
