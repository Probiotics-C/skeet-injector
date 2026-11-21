# 目标客户端本地化补丁适配说明（1star 修复版）

## 背景
参考实现支持两份本地化文件(放在 CSGO 游戏根目录, 即 csgo.exe 所在目录):

- `language.txt` —— UTF-8 编码的 "英文=中文" 对照表(带 `language=chinese` / `compatibility=true` 头部行)
- `GenSenMaruGothic TW TTF Medium.ttf` —— 中文菜单字体

本源码(1star 修复版)原本不读取这两个文件, 现已完成适配。

## 改动文件
- **新增** `skeet/ChinesePatch.h` / `skeet/ChinesePatch.cpp` —— 中文补丁模块(注意: 源文件保存为 UTF-8 带 BOM, 否则中文注释在中文代码页下会导致编译错误)
- **修改** `skeet/skeet.cpp`
  - 构造函数开头调用 `cn_patch::initialize(base)`(读取 language.txt、注册字体)
  - 读取 skeet.bin 资源后调用 `cn_patch::patch_image_strings(skeet_bin, size)`
  - 复制 menu_fonts 后调用 `cn_patch::patch_menu_strings(skeetFonts, sizeof(menu_fonts))`
  - `fix_imports()` 中把 `gdi32.dll!CreateFontIndirectW` 解析为 `cn_patch::hooked_CreateFontIndirectW`
- **修改** `skeet/skeet.vcxproj` / `skeet.vcxproj.filters`(加入以上新文件)

## 工作原理
1. **字符串翻译(数据层)**
   菜单英文字符串以 UTF-16 存放在两个位置:
   - 菜单字符串池(menu_fonts 的运行时副本, 约 272 条词条、296 处替换点)
   - 镜像内静态字符串(skeet.bin .data 区: Always on / On hotkey / Toggle / Off hotkey)

   在游戏读取它们之前, 把 "英文 + 00 00" 槽位原地改写为 "中文 + 00 00" 并清零槽位剩余字节;
   **仅当中文码字数 <= 英文码字数时替换**, 保证不越界损坏相邻字符串。
   个别译文过长(放不进槽位)的条目使用内置短译: `Yaw→左右`、`Dump MM wins→打印MM胜场`;
   其余过长条目自动跳过(保留英文)。

2. **字体(渲染层)**
   启动时对 TTF 调用 `AddFontResourceExW(FR_PRIVATE)`(进程内私有注册, 不污染系统字体表;
   若字体已安装进系统也能探测到), 然后拦截 `gdi32!CreateFontIndirectW`:
   菜单创建 CJK 字体 **"Malgun Gothic"** 时改用 **"GenSenMaruGothic TW TTF Medium"**, 
   使中文有字形可用(菜单的字符→字体回退依赖字体创建时查询的 Unicode 覆盖)。
   Verdana / Calibri / Small Fonts / Segoe UI Symbol(图标)等其余字体**不受影响**。

## 构建
Visual Studio 打开 `skeet.sln` → 选择 `Release | Win32` → 生成。
或命令行: `MSBuild skeet.vcxproj /p:Configuration=Release /p:Platform=Win32`
(项目 PlatformToolset 为 v145; 用旧版 VS 的可临时改成 v143)

## 使用
保持 `language.txt` 和 `GenSenMaruGothic TW TTF Medium.ttf` 在游戏根目录
(放 skeet.dll 同目录亦可, 代码会两种都找), 正常用 1star.exe 加载即可。

## 已验证
- 编译通过(v143 工具集实测), 产出 skeet.dll(约 13.9 MB)
- 字符串替换逻辑与参考实现**逐字节一致**(菜单池 296 处 + 镜像 4 处)
- 字体替换实测: 请求 Malgun Gothic → 实际得到 GenSenMaruGothic TW TTF Medium, 中文 '中/用' 可正常光栅化; 请求 Verdana 则原样通过

## 已知限制(如需进一步优化可继续迭代)
- 以 **ASCII(窄字符)** 存储的字符串未被翻译(例如 Highest damage / Cycle / Low / High / 部分武器名 / 内置 Lua 脚本名等)。
  这类字符串无法原地写入中文字节, 需更深入的渲染层 hook 才能翻译。
- Hrisito 版专有选项(Hrisito mode / Hrisito DVD 等)在本版镜像中不存在, 对应词条会被自动跳过。
- 若实测个别位置中文显示为空白/方块, 说明该处文本走了其它字体(把字体替换表扩一扩即可, 见 `ChinesePatch.cpp` 中 `kReplaceFaceFrom`)。
