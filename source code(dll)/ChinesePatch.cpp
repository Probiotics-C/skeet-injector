#include "pch.h"
#include "ChinesePatch.h"

#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace cn_patch {

namespace {

// ------------------------------------------------------------------ 配置

// 中文补丁文件名(CSGO 游戏根目录, 即 csgo.exe 所在目录)
const wchar_t kLanguageFileName[] = L"language.txt";
// 保持英文的词条列表(可选, 与 language.txt 同目录); 缺失时使用内置默认列表并自动生成
const wchar_t kSkipFileName[] = L"language_skip.txt";
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

// 【重要】始终跳过的词条(保持英文原文, 外部文件无法覆盖)。
//   Lua —— "Lua" 面板的 Tab 名。ui.new_checkbox / ui.reference 等 API 的
//          tab 参数按此名匹配, 翻译后加载使用 "LUA" 的脚本会报:
//          bad argument #1 to 'ui_new_checkbox' (invalid argument)
const wchar_t* const kAlwaysSkipKeys[] = {
    L"Lua",
};

// 默认保持英文列表(外部 language_skip.txt 缺失时使用, 并写入该文件供调整)。
// 内容 = 现有 Lua 脚本实际当作 ui.* 标识符引用的内置词条。
const wchar_t* kDefaultSkipTerms =
    L"Aimbot\nAmmobox\nApply to all\nLua\nLUA\nMiscellaneous\nPlayers\nPresets\nReset\nSettings\n"
    L"Weapon type\n"

;

bool g_initialized = false;
bool g_font_registered = false;
bool g_font_available = false;   // 字体可用(本次注册成功, 或系统里本来就有)
std::vector<translation_t> g_translations;

// Reverse map: translation -> original English (many-to-one).
// Keyed by FNV-1a hash of the translation: no allocation per lookup, and it
// works for translations that contain no CJK at all (e.g. "OSAA").
struct reverse_entry_t
{
    std::wstring target;
    std::vector<std::wstring> sources;
};
std::unordered_map<std::uint64_t, std::vector<reverse_entry_t>> g_reverse;
std::size_t g_reverse_count = 0;

std::uint64_t fnv1a_w(const wchar_t* text, int limit = 192)
{
    std::uint64_t hash = 1469598103934665603ull;
    int i = 0;
    for (; text[i] != 0; ++i)
    {
        if (i >= limit)
            return 0;   // 超长字符串不参与反查
        hash ^= (std::uint64_t)(std::uint16_t)text[i];
        hash *= 1099511628211ull;
    }
    return i == 0 ? 0 : hash;
}

void record_reverse(const std::wstring& target, const std::wstring& source)
{
    if (target.empty() || source.empty())
        return;

    const std::uint64_t hash = fnv1a_w(target.c_str());
    if (hash == 0)
        return;

    std::vector<reverse_entry_t>& bucket = g_reverse[hash];
    for (reverse_entry_t& entry : bucket)
    {
        if (entry.target != target)
            continue;
        for (const std::wstring& existing : entry.sources)
        {
            if (existing == source)
                return;
        }
        entry.sources.push_back(source);
        return;
    }

    bucket.push_back(reverse_entry_t{ target, std::vector<std::wstring>{ source } });
    ++g_reverse_count;
}

// 判断某个字符串是不是“译文”; 是的话返回它对应的英文原文列表
const std::vector<std::wstring>* reverse_sources(const wchar_t* text)
{
    if (text == nullptr)
        return nullptr;

    const std::uint64_t hash = fnv1a_w(text);
    if (hash == 0)
        return nullptr;

    auto it = g_reverse.find(hash);
    if (it == g_reverse.end())
        return nullptr;

    for (const reverse_entry_t& entry : it->second)
    {
        if (entry.target == text)
            return &entry.sources;
    }
    return nullptr;
}

std::vector<std::wstring> g_skip_terms;   // 生效的保持英文列表
std::wstring g_language_dir;              // language.txt 所在目录(用于生成 skip 文件)

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

// [ISOLATION TEST] 隔离开关目录: 由 initialize() 设为 skeet.dll / debug.exe 所在目录。
// 采用“开关文件”而非环境变量 —— 因为 skeet.dll 是在 csgo.exe 进程内被注入的,
// 读的是 csgo.exe 自己的环境变量块, 由 .bat 设置的变量根本传不进去。
std::wstring g_switch_dir;

// 开关是否生效: skeet.dll 目录下存在开关文件, 或(兼容)设置了同名环境变量。
bool switch_on(const wchar_t* file_name, const wchar_t* env_name)
{
    if (!g_switch_dir.empty() && file_exists(g_switch_dir + L"\\" + file_name))
        return true;

    // 手动映射时 dll 目录通常为空, 游戏目录也查一遍(隔离测试用)
    const std::wstring game_directory = module_directory(nullptr);
    if (!game_directory.empty() && file_exists(game_directory + L"\\" + file_name))
        return true;
    if (GetEnvironmentVariableW(env_name, nullptr, 0) != 0)
        return true;
    return false;
}

// [ISOLATION] 菜单池翻译白名单(menu_only.txt): 文件存在时, 菜单池(buf=skeetFonts)
//   只翻译其中列出的词条; 镜像(buf=skeet_bin)不受影响。用于二分定位问题词条。
std::vector<std::wstring> g_menu_only;
bool g_menu_only_loaded = false;
bool g_filter_menu = false;   // 仅在 patch_menu_strings 期间为 true
std::wstring g_skip_path;     // 实际读取成功的 language_skip.txt 路径(诊断用)

// ------------------------------------------------------------------ 保持英文列表

bool is_skip_key(const std::wstring& key)
{
    for (const wchar_t* skip : kAlwaysSkipKeys)
        if (key == skip)
            return true;

    for (const std::wstring& skip : g_skip_terms)
        if (key == skip)
            return true;

    return false;
}

// 读取外部保持英文列表(每行一个词条; '#' 开头或在行尾均为注释)
bool load_skip_file(const std::wstring& path)
{
    std::string data;
    {
        std::ifstream file(path.c_str(), std::ios::binary);
        if (!file.is_open())
            return false;
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);

    g_skip_terms.clear();

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

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        trim(line);
        if (line.empty())
            continue;

        std::wstring term = utf8_to_wide(line);
        if (!term.empty())
            g_skip_terms.push_back(std::move(term));
    }

    return true;
}

// [ISOLATION] 读取菜单池翻译白名单(menu_only.txt): 每行一个词条, '#' 为注释
bool load_menu_only_file(const std::wstring& path)
{
    std::string data;
    {
        std::ifstream file(path.c_str(), std::ios::binary);
        if (!file.is_open())
            return false;
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);

    g_menu_only.clear();

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

        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        trim(line);
        if (line.empty())
            continue;

        std::wstring term = utf8_to_wide(line);
        if (!term.empty())
            g_menu_only.push_back(std::move(term));
    }

    g_menu_only_loaded = true;
    return true;
}

bool menu_only_allowed(const std::wstring& term)
{
    for (const std::wstring& allow : g_menu_only)
        if (allow == term)
            return true;
    return false;
}

// 内置默认列表
void use_builtin_skip_terms()
{
    // 注意: 不清空, 与已读取的外部词条合并

    const std::wstring blob = kDefaultSkipTerms;
    std::size_t position = 0;
    while (position <= blob.size())
    {
        std::size_t eol = blob.find(L'\n', position);
        if (eol == std::wstring::npos)
            eol = blob.size();

        std::wstring term = blob.substr(position, eol - position);
        if (!term.empty())
            g_skip_terms.push_back(std::move(term));

        if (eol == blob.size())
            break;
        position = eol + 1;
    }
}

// 外部文件缺失时, 把默认列表写到 language.txt 同目录, 方便用户调整
void write_default_skip_file(const std::wstring& path)
{
    std::string content;
    content += "# language_skip.txt - terms that stay ENGLISH (one per line, '#' = comment)\r\n";
    content += "# These are used as identifiers by the Lua script API: the tab/container/name\r\n";
    content += "# arguments of ui.reference / ui.new_checkbox etc. are matched by these original\r\n";
    content += "# names. If translated, scripts referencing them fail with:\r\n";
    content += "#   bad argument #1 to 'ui_new_checkbox' (invalid argument)\r\n";
    content += "# Show a term in Chinese: delete/comment its line here (add back if a script errors).\r\n";
    content += "# New script error: add the reported name to this file.\r\n";
    content += "#\r\n";

    for (const std::wstring& term : g_skip_terms)
    {
        bool ascii = true;
        for (wchar_t character : term)
        {
            if (character < 0x20 || character > 0x7E)
            {
                ascii = false;
                break;
            }
            content += (char)character;
        }
        if (ascii)
            content += "\r\n";
    }

    std::ofstream file(path.c_str(), std::ios::binary);
    if (!file.is_open())
        return;
    file.write(content.data(), (std::streamsize)content.size());
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

        // 跳过被游戏内部当作标识符的词条(见 kAlwaysSkipKeys / language_skip.txt 说明)
        if (is_skip_key(source))
            continue;

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
    // [ISOLATION TEST] 开关文件 disable_strings.flag 或环境变量 DISABLE_STRINGS 生效时, 跳过字符串翻译
    if (switch_on(L"disable_strings.flag", L"DISABLE_STRINGS"))
        return;

    if (!buffer || size == 0 || g_translations.empty())
        return;

    for (const translation_t& translation : g_translations)
    {
        // [ISOLATION] 菜单池白名单过滤
        if (g_filter_menu && g_menu_only_loaded && !menu_only_allowed(translation.source))
            continue;

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
                record_reverse(translation.target, translation.source);
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

// 内嵌的辅助脚本: 每次注入时自动释放到 <游戏目录>\lua\hidden_features.lua
// 内容随 dll 版本更新(与内嵌内容不一致就覆盖)
static const char kHiddenFeaturesScript[] = R"LUA(
-- Hidden features: expose gamesense hidden developer items as visible controls.
if _G.__hidden_features_v1 then return end
_G.__hidden_features_v1 = true

local SOURCE_TAB       = "MISC"
local SOURCE_CONTAINER = "Settings"
local UI_TAB           = "MISC"
local UI_CONTAINER     = "Settings"

local function log(msg) pcall(print, "[hidden] " .. tostring(msg)) end

local function reference(item)
    local ok, ref = pcall(ui.reference, SOURCE_TAB, SOURCE_CONTAINER, item)
    if ok and ref ~= nil then return ref end
    log("reference failed: " .. item)
    return nil
end

-- 同名控件可能已存在(隐藏项), 失败时自动加 " #2" 后缀重试
local function new_with_fallback(factory, label, ...)
    local ok, item = pcall(factory, UI_TAB, UI_CONTAINER, label, ...)
    if ok and item ~= nil then return item end
    local ok2, item2 = pcall(factory, UI_TAB, UI_CONTAINER, label .. " #2", ...)
    if ok2 and item2 ~= nil then return item2 end
    log("create failed: " .. label)
    return nil
end

local function clamp(value, lo, hi)
    if type(value) ~= "number" then return lo end
    if value < lo then return lo end
    if value > hi then return hi end
    return value
end

local function add_checkbox(item, label)
    local ref = reference(item)
    if not ref then return end
    local current = false
    local ok, value = pcall(ui.get, ref)
    if ok and type(value) == "boolean" then current = value end
    local box = new_with_fallback(ui.new_checkbox, label)
    if not box then return end
    ui.set(box, current)
    ui.set_callback(box, function(ctrl)
        local ok2, value2 = pcall(ui.get, ctrl)
        if ok2 then pcall(ui.set, ref, value2) end
    end)
end

local function add_slider(item, label, min_value, max_value)
    local ref = reference(item)
    if not ref then return end
    local initial = min_value
    local ok, value = pcall(ui.get, ref)
    if ok and type(value) == "number" then
        initial = clamp(math.floor(value + 0.5), min_value, max_value)
    end
    local slider = new_with_fallback(ui.new_slider, label, min_value, max_value, initial)
    if not slider then return end
    ui.set_callback(slider, function(ctrl)
        local ok2, value2 = pcall(ui.get, ctrl)
        if ok2 then pcall(ui.set, ref, value2) end
    end)
end

add_checkbox("sv_maxusrcmdprocessticks_holdaim", "sv_maxusrcmdprocessticks_holdaim")
add_slider  ("sv_maxusrcmdprocessticks2", "sv_maxusrcmdprocessticks2", 0, 32)
add_slider  ("sv_maxunlag2", "sv_maxunlag2", 0, 1000)
add_slider  ("sv_clockcorrection_msecs2", "sv_clockcorrection_msecs2", 0, 200)
add_checkbox("Anti-untrusted", "Anti-untrusted")
add_checkbox("Allow custom game events", "Allow custom game events")
add_checkbox("Faster grenade toss", "Faster grenade toss")
add_checkbox("Fix networking", "Fix networking")

log("hidden features added")
)LUA";

static void deploy_bundled_script(const std::wstring& game_directory)
{
    char narrow[MAX_PATH * 2] = {};

    if (game_directory.empty())
    {
        return;
    }

    WideCharToMultiByte(CP_ACP, 0, game_directory.c_str(), -1, narrow, _countof(narrow), nullptr, nullptr);

    const std::wstring lua_directory = game_directory + L"\\lua";
    CreateDirectoryW(lua_directory.c_str(), nullptr);

    // 脚本里不含任何绝对路径; 路径只存在于 dll 内存里
    const std::string script(kHiddenFeaturesScript);

    // 只写一份到 <csgo目录>\lua\；路径来自运行时 csgo.exe 位置, 不是硬编码
    const std::wstring targets[1] = {
        lua_directory + L"\\hidden_features.lua"
    };

    for (int index = 0; index < 1; ++index)
    {
        const std::wstring& path = targets[index];

        std::string existing;
        {
            std::ifstream file(path.c_str(), std::ios::binary);
            if (file.is_open())
                existing.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        if (existing == script)
        {
            continue;
        }

        std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            continue;
        }

        file.write(script.data(), script.size());
        file.flush();
        file.close();

    }
}

void initialize(HMODULE self_module)
{
    if (g_initialized)
        return;
    g_initialized = true;


    // 【方案 A】中文化只在“显式放了补丁文件”时启用, 且只认 debug.exe / skeet.dll 所在目录,
    // 不再读取游戏根目录 —— 避免游戏目录里遗留的 language.txt 意外触发汉化。
    std::vector<std::wstring> directories;
    const std::wstring dll_directory = module_directory(self_module);   // skeet.dll / debug.exe 目录
    if (!dll_directory.empty())
        directories.push_back(dll_directory);
    g_switch_dir = dll_directory;   // [ISOLATION TEST] 开关文件在此目录查找
    const std::wstring game_directory = module_directory(nullptr);      // csgo.exe 目录
    if (!game_directory.empty() && game_directory != dll_directory)
        directories.push_back(game_directory);

    // 释放内嵌的 hidden_features.lua 到 游戏目录 下的 lua 文件夹
    deploy_bundled_script(game_directory);


    // 0) 先确定“保持英文”列表(skip): 优先外部文件, 否则用内置默认列表。
    //    必须在读取 language.txt 之前完成, 因为 load_language 在解析时就调用
    //    is_skip_key 过滤词条。
    bool skip_file_loaded = false;
    g_skip_terms.clear();
    for (const std::wstring& directory : directories)
    {
        if (load_skip_file(directory + L"\\" + kSkipFileName))
        {
            skip_file_loaded = true;
            g_skip_path = directory + L"\\" + kSkipFileName;
        }
    }
    use_builtin_skip_terms();          // 内置词条追加到外部列表之后(并集)

    // 0b) [ISOLATION] 菜单池翻译白名单(可选)
    for (const std::wstring& directory : directories)
    {
        if (load_menu_only_file(directory + L"\\" + L"menu_only.txt"))
            break;
    }

    // 1) 查找 language.txt(汉化的唯一触发点); 找不到则保持英文
    for (const std::wstring& directory : directories)
    {
        if (load_language(directory + L"\\" + kLanguageFileName))
        {
            g_language_dir = directory;
            break;
        }
    }

    if (g_language_dir.empty())
    {
        // 没放 language.txt: 完全不触碰菜单文案与字体, 保持原版英文
        return;
    }


    // 2) 外部列表文件缺失时生成一份默认列表, 便于用户自行调整
    if (!skip_file_loaded)
        write_default_skip_file(g_language_dir + L"\\" + kSkipFileName);

    // 3) 注册中文补丁字体(进程内私有, 不污染系统字体表)
    //    [ISOLATION TEST] 若存在 disable_font_register.flag, 连字体注册也跳过
    if (!switch_on(L"disable_font_register.flag", L"DISABLE_FONT_REGISTER"))
    {
        for (const std::wstring& directory : directories)
        {
            const std::wstring font_path = directory + L"\\" + kFontFileName;
            if (file_exists(font_path))
            {
                g_font_registered = AddFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr) != 0;
                break;
            }
        }
    }

    // [ISOLATION TEST] 开关文件 disable_fontswap.flag 或环境变量 DISABLE_FONTSWAP 生效时, 跳过字体替换(仅保留字符串翻译)
    if (switch_on(L"disable_fontswap.flag", L"DISABLE_FONTSWAP"))
        return;

    // 即使 FR_PRIVATE 注册失败, 字体也可能已被安装进系统 —— 探测一次实际可用性
    g_font_available = probe_font_available();
}


void patch_menu_strings(void* buffer, size_t size)
{
    // [ISOLATION TEST] disable_menu_strings.flag: 只跳过菜单字符串池副本的翻译
    if (switch_on(L"disable_menu_strings.flag", L"DISABLE_MENU_STRINGS"))
        return;
    g_filter_menu = true;   // 仅菜单池应用 menu_only.txt 白名单
    translate_in_place(static_cast<std::uint8_t*>(buffer), size);
    g_filter_menu = false;

}

void patch_image_strings(void* buffer, size_t size)
{
    // [ISOLATION TEST] disable_image_strings.flag: 只跳过镜像(skeet.bin)字符串的翻译
    if (switch_on(L"disable_image_strings.flag", L"DISABLE_IMAGE_STRINGS"))
        return;
    translate_in_place(static_cast<std::uint8_t*>(buffer), size);
}

bool active()
{
    return !g_translations.empty() || g_font_registered || g_font_available;
}

// [ISOLATION] 是否启用字体导入改写(见 header 说明)
bool font_import_hook_enabled()
{
    return !switch_on(L"disable_font_import.flag", L"DISABLE_FONT_IMPORT");
}

// [ISOLATION] 是否启用 _wcsicmp 反查(见 header 说明)
bool wcsicmp_hook_enabled()
{
    return !switch_on(L"disable_wcsicmp_hook.flag", L"DISABLE_WCSICMP_HOOK");
}

// ------------------------------------------------------------------ 查找层反查

namespace {

// [已废弃] 以前用“是否含 CJK”做快速过滤, 但译文里有纯 ASCII 的(如 OSAA),
// 会被误判为非译文而跳过反查; 现改为哈希查表, 不再依赖字符集。
void has_cjk_removed();

using wcsicmp_fn = int(__cdecl*)(const wchar_t*, const wchar_t*);
wcsicmp_fn g_real_wcsicmp = nullptr;

wcsicmp_fn real_wcsicmp()
{
    if (g_real_wcsicmp == nullptr)
    {
        HMODULE crt = GetModuleHandleW(L"msvcrt.dll");
        if (crt == nullptr)
            crt = LoadLibraryW(L"msvcrt.dll");
        if (crt != nullptr)
            g_real_wcsicmp = reinterpret_cast<wcsicmp_fn>(GetProcAddress(crt, "_wcsicmp"));
        if (g_real_wcsicmp == nullptr)
            g_real_wcsicmp = &_wcsicmp;
    }
    return g_real_wcsicmp;
}

} // namespace

int __cdecl hooked__wcsicmp(const wchar_t* lhs, const wchar_t* rhs)
{
    if (lhs == nullptr || rhs == nullptr)
        return real_wcsicmp()(lhs, rhs);

    const int direct = real_wcsicmp()(lhs, rhs);
    if (direct == 0)
        return direct;

    // 池里的名字可能已经换成了译文(中文, 或 "OSAA" 这种纯 ASCII 缩写),
    // 这里把译文还原成英文原文再比较, 脚本用英文名就能查到。
    // 注意: 两边都可能是译文, 不能只看一侧就提前返回。
    if (const std::vector<std::wstring>* sources = reverse_sources(lhs))
    {
        for (const std::wstring& source : *sources)
        {
            if (real_wcsicmp()(source.c_str(), rhs) == 0)
                return 0;
        }
    }

    if (const std::vector<std::wstring>* sources = reverse_sources(rhs))
    {
        for (const std::wstring& source : *sources)
        {
            if (real_wcsicmp()(lhs, source.c_str()) == 0)
                return 0;
        }
    }

    return direct;
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
