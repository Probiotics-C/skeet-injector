# gamesense (skeet) 汉化注入器

面向旧版 CS:GO（`csgo.exe`）的 gamesense / skeet 汉化方案：渲染层翻译模块 + 自研注入器加载器。

> 本项目为 Windows 平台 C++ 逆向与注入技术研究项目，演示了远程注入（`CreateRemoteThread + LoadLibraryW`）、基址预留、渲染器虚表 Hook、字体替换与翻译表热重载等技术。汉化不改动任何被 Lua 按名查找的字符串，存档 / CFG / Lua 互不影响。

---

## 功能特性

- **全菜单中文**：翻译表 499 条词条，其中 483 条已译成中文；12 条（如 `G3SG1 / SCAR-20`、`AWP`）保持原文
- **不动索引**：只在渲染时替换绘制参数，`ui.reference` / `ui.new_checkbox` 等按英文名查找的路径完全不受影响
- **单文件分发**：`debug.exe` 内嵌 `skeet.dll` + `skeet_tr.dll` + 翻译表，运行时释放到 `%TEMP%`，注入后自动清理
- **自动重试**：点一次 Load 后持续监听 `csgo.exe`，自动拉起失败、你手动启动游戏，照样会被注入
- **热重载翻译**：改 `skeet_tr.txt` 存盘，下一帧生效，不用重开游戏或注入器
- **免字体文件**：`create_font` 钩子把 CJK 字体槽 `Malgun Gothic` 换成系统自带 `Microsoft YaHei`
- **Resolver 补强**：在原生“三选一”层面加**换边冷却 350 ms + 去抖 120 ms**，并把环境评分死区从 ±20° 放宽到 **±25°** —— 只调选择时机与分档，不改 VM 里的候选生成

## 技术栈

| 组件 | 技术 |
|---|---|
| 注入器界面 | C++ / ImGui + Direct3D 9，纯代码绘制的三层窗口（Splash / Menu / Busy） |
| 注入方式 | `VirtualAllocEx` 预留基址 → 临时文件 → `CreateRemoteThread` 调目标进程 `LoadLibraryW` |
| 翻译模块 | C++ DLL，Hook 渲染器虚表 `+0x3C`（type 10 文字事件），改写 `text` / `length` / `width` |
| 字体处理 | Hook `create_font`（`0x4334E194`），CJK 槽位替换为 `Microsoft YaHei` |
| 翻译表 | UTF-8 文本，格式 `原文=译文`；经命名共享内存 `Local\skeet_tr_cfg` 传给 DLL，500 ms 热重载 |
| Resolver 补强 | 包装 `select_variant`（`0x433AB612`）的三处 `call`，并替换 `0x43349D0C` 处 27 字节内联判档块；全部在 `skeet_tr.dll` 内完成，地址不符自动跳过 |
| 构建 | Visual Studio 2022 / MSBuild，工具集 v143，平台 Win32 |

## 项目结构

```
├── debug.exe                # 注入器（构建产物，内嵌三个资源）
├── skeet_tr.txt             # 翻译表（可选；放在 exe 同目录即启用热重载）
├── debug.sln                # 解决方案：debug + skeet_tr + skeet(DLL)
├── debug/                   # 注入器工程
│   ├── main.cpp             # 三层 UI、注入流程、共享内存发布
│   ├── loader.cpp(.h)       # 基址预留、临时文件注入、结果校验
│   ├── build.ps1            # 一键构建（DLL → 翻译模块 → 注入器）
│   ├── skeet.dll            # 内嵌资源 RCDATA 102（由 DLL 工程自动同步）
│   ├── skeet_tr.dll         # 内嵌资源 RCDATA 103
│   ├── skeet_tr.txt         # 内嵌资源 RCDATA 104
│   ├── skeet_tr/            # 翻译 + resolver 补强模块（tr.cpp + 模块级 README）
│   └── imgui/               # 界面库
└── source code(dll)/        # 作弊 DLL 工程
    ├── skeet.cpp / ChinesePatch.* / dllmain.cpp
    ├── Binary/  VM/  Json/  MinHook/  SkeetSDK/
    └── Release/skeet.dll    # 构建产物（PostBuild 自动复制到 ..\debug\）
```

## 构建说明

**环境要求**：Windows 10/11 + [Visual Studio 2022](https://visualstudio.microsoft.com/)（工具集 v143，平台 Win32）

```powershell
powershell -ExecutionPolicy Bypass -File ".\debug\build.ps1" -Configuration Debug
```

脚本按依赖顺序执行四步：`skeet.dll`（固定 Release|Win32）→ `skeet_tr.dll` → 删除 `resource.res` 强制重新嵌入 → `debug.exe`，产物输出到本目录。

> 注意：作弊 DLL 工程开启了 `/GL` 全局优化，一次全量代码生成需要 3~6 分钟，期间只显示「正在生成代码」，
> 请勿中断（Ctrl+C 会报 `LNK1257: 代码生成失败` 并删除 `Release\skeet.dll`）。
> 日常只改注入器时，用 `debug.sln` 按 F7 即可，`skeet` 工程默认不参与生成。

## 使用说明

1. 安装 VC++ 运行库（`VCRUNTIME140` / `MSVCP140`）
2. 确认 Steam 已启动，游戏为旧版 `csgo.exe`（不是 CS2），启动项建议带 `-insecure`
3. 双击 `debug.exe`，勾选 `Auto launch game` 后点 **Load**
4. 状态栏显示 `Injected, have fun!` 后注入器自动退出，进游戏按 **Delete** 打开菜单

> 若提示「无法验证发布者」，是文件带 Internet 标记且无数字签名所致：右键 `debug.exe` → 属性 → 勾选「解除锁定」，
> 或执行 `Get-ChildItem -Recurse -File | Unblock-File`。运行日志见同目录 `debug_log.txt` 与 `skeet_tr.log`。

## 工作原理

**注入期**：`debug.exe` 持续监听 `csgo.exe`，发现进程后立即用 `VirtualAllocEx` 抢占 `0x43310000`（作弊镜像按该基址写死了绝对地址），待 `Valve001` 主窗口出现后，把内嵌的 `skeet.dll` 写入 `%TEMP%\gs_<pid>_<tick>.dll`，经 `CreateRemoteThread + LoadLibraryW` 加载，再用同样方式加载 `skeet_tr.dll`，最后读回 `0x43310000` 校验镜像已写入。

**汉化期**：`skeet_tr.dll` 等作弊镜像就绪（`0x4331E000` 首字节为 `0x55`）后，Hook 渲染器虚表 `0x4346F46C + 0x3C` 的 type 10 文字绘制；命中翻译时替换 `text` 指针、改写字符数并把 `width` 置 0（否则下拉框会被裁剪成 `O` / `OF`）。同时 Hook `create_font`，把 `Malgun Gothic` 换成 `Microsoft YaHei`，因此无需随包分发字体文件。

**解析期**：`skeet_tr.dll` 还会给原生 resolver 打两处补丁。其一，把“三选一”函数 `sub_433AB612(record, attempt)` 的三处调用点（`0x43349DE5` / `0x4336C66F` / `0x4341386E`）包一层：同一 record 在 350 ms 内不允许换边，且新选择要持续 120 ms 才采纳，避免一次误判把整条历史链翻过去。其二，环境评分里内联的“`pose < -20 → 2` / `pose > 20 → 1`”判档（`0x43349D0C..0x43349D26`）整段换成 ±25° —— 两个原始常量被别处大量引用，不能直接改，所以替换的是这段无外部跳转入口的代码块。

> 这两处补丁按固定地址定位，并在改写前校验原始字节；换 csgo / skeet 版本后会自动跳过（不匹配就不改），需要重新定位。

**关于手动映射**：早期尝试过纯手动映射，但 MSVC 的线程安全静态变量会读取 `__declspec(thread)` 的 TLS 槽，手动映射未分配 TLS 块导致 `0xC0000005`；模块不在加载器链表还会让 C++ 异常变成 `STATUS_INVALID_EXCEPTION_HANDLER`。故改用 `LoadLibraryW` 由系统加载器处理 TLS / SEH / 资源。

## 更新日志

| 日期 | 内容 |
|---|---|
| 2026-10-18 | ImGui 注入器界面；作弊 DLL 内嵌进 EXE；初版汉化（仅部分菜单可用）；修复 unload 后 `0x43310000` 报错 |
| 2026-10-24 | 重写注入器菜单；修复 LoadLibraryA / GetProcAddress 解析、目标模块未就绪、OpenProcess 失败；注入器日期改为跟随系统时间 |
| 2026-10-25 | 注入器 UI 更新；自动释放 `hidden_features.lua`（用于打开 `sv_maxusrcmdprocessticks2`）；汉化补丁部分更新 |
| 2026-11-01 | 汉化补丁内嵌进 DLL；注入器窗口位置跟随、Steam 未启动时的等待与自动重试流程；移除 DLL 内部全部调试日志 |
| 2026-11-06 | 新增渲染层翻译模块 `skeet_tr.dll`：Hook 渲染器虚表 `+0x3C`、宽度裁剪修复（下拉框不再显示成 `O` / `OF`）、翻译表 500 ms 热重载、`create_font` 换成中文字体 |
| 2026-11-13 | 修复菜单掉帧 |
| 2026-11-14 | 目录重整：DLL 工程迁至 `source code(dll)`，`debug.sln` 提到 `source code\`，构建流程改为 `build.ps1` 一键四步 |
| 2026-11-20 | 注入器 UI 日期更新为 2026/11/20；`skeet_tr.dll` 新增 resolver 补强：换边冷却 350 ms、去抖 120 ms、环境评分死区 ±25°，默认开启且不写日志 |

## 免责声明与法律声明

- 本项目**仅供个人学习与技术研究**，用于演示 Win32 注入、PE 加载、渲染层 Hook 与字体替换等技术。
- 本项目包含对第三方付费软件（gamesense / skeet）的破解与汉化内容，**与原作者及任何游戏厂商无任何关联**；相关名称与商标归其各自所有者所有。
- **不得**将本项目用于线上对局、商业用途或任何违反游戏服务条款的场景。
- 使用本程序产生的一切后果（账号封禁、数据损失、法律风险等）由**使用者自行承担**，作者概不负责。
- 本项目按「现状」提供，不附带任何明示或默示的担保。

## License

本仓库未附带开源许可证（LICENSE），默认保留所有权利，未经许可不得二次分发。
