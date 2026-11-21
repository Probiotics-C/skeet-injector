#pragma once
//
// ChinesePatch —— gamesense 破解(1star 修复版) 中文补丁适配模块
//
// 适配的文件(放在 CSGO 游戏根目录, 即 csgo.exe 所在目录; 也支持与 skeet.dll 同目录):
//   language.txt                        —— "英文=中文" 对照表(UTF-8), 由中文补丁提供
//   GenSenMaruGothic TW TTF Medium.ttf  —— 中文补丁字体(菜单 CJK 文本用)
//
// 工作原理:
//   1) 启动时读取 language.txt。菜单英文字符串以 UTF-16 存放在两个位置:
//        a. 菜单字符串池(menu_fonts 的运行时副本)   -> patch_menu_strings()
//        b. 镜像内的静态字符串(skeet.bin .data 区)  -> patch_image_strings()
//      在游戏读取它们之前, 把 "英文 + NUL NUL" 槽位原地替换为 "中文 + NUL" 并清零多余
//      字节(仅当中文码字数 <= 英文码字数, 保证不越界)。
//   2) fix_imports 把 gdi32!CreateFontIndirectW 的解析结果换成 hooked_CreateFontIndirectW:
//      菜单创建 CJK 字体 "Malgun Gothic" 时改用 GenSenMaruGothic TW, 使其字形表覆盖
//      中文(菜单的字符->字体回退依赖创建时查询的 Unicode 范围, 其余字体不变)。
//
#include <Windows.h>

namespace cn_patch {

// 读取 language.txt / 注册 TTF 私有字体。必须在 skeet_t 构造时(映射之前)调用。
void initialize(HMODULE self_module);

// 就地翻译 UTF-16 字符串数据块(菜单字符串池副本 / 镜像原始缓冲区)
void patch_menu_strings(void* buffer, size_t size);
void patch_image_strings(void* buffer, size_t size);

// 是否检测到中文补丁文件(任意一个)
bool active();

// gdi32!CreateFontIndirectW 包装(替换菜单 CJK 字体)
HFONT WINAPI hooked_CreateFontIndirectW(const LOGFONTW* logfont);

} // namespace cn_patch
