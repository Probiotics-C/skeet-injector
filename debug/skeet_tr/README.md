# skeet_tr — 菜单文本翻译模块（测试版）

被 `debug.exe` 在注入 `skeet.dll` 之后**自动注入**（内嵌 RCDATA 103），把渲染器 type 10 文字事件里的
`const wchar_t*` 换成翻译表里的字符串。

## 1. 挂接点（静态反汇编验证）

镜像基址 `0x43310000`，renderer 虚表 `0x4346F46C`，type 10 文字在 `+0x3C`：

```
4339D0B8  55                push ebp              ; <- 虚表 +0x3C 里存的是这个 thunk
4339D0B9  8bec              mov ebp, esp
4339D0BB  83e4f8            and esp, -8
4339D0BE  8b4904            mov ecx, [ecx+4]      ; this 调整 renderer+4
4339D0C1  8d4518            lea eax, [ebp+0x18]   ; &text
4339D0C4  50                push eax
4339D0C5  ff7514            push [ebp+0x14]       ; width
4339D0C8  ff7510            push [ebp+0x10]       ; color
4339D0CB  ff750c            push [ebp+0xc]        ; y
4339D0CE  ff7508            push [ebp+8]          ; x
4339D0D1  e86746faff        call 0x4334173D       ; 实现体
4339D0D6  8be5              mov esp, ebp
4339D0D8  5d                pop ebp
4339D0D9  c21800            ret 0x18
```

vtable 边界上的参数（`__thiscall`）：

| 位置 | 含义 |
|---|---|
| `ecx` | renderer |
| `[esp+04]` | x |
| `[esp+08]` | y |
| `[esp+0C]` | color |
| `[esp+10]` | width |
| **`[esp+14]`** | **`const wchar_t* text`** ← 替换这个 |
| `ret 0x18` | 共 6 个栈参数（第 6 个与本模块无关） |

`[ebp+0x18]` 之所以等于入口 `[esp+0x14]`：`push ebp; mov ebp,esp` 之后 `ebp = entry_esp - 4`。

## 2. 为什么不能 `jmp 0x4334173D`

`0x4334173D` 是**实现体**，不是虚表里的值：

- 它收到的第 5 个参数是 `&text`（`wchar_t**`，`text_measure` 会推进指针），不是 `wchar_t*`；
- 它的 `ecx` 已经被 thunk 改成 `[ecx+4]`。

所以裸函数必须**保存虚表原值再跳回去**：

```asm
pushad
mov  ebx, esp                   ; pushad 后的基址
push dword ptr [ebx + 0x34]     ; text（入口 [esp+0x14] + 0x20）
call tr_lookup                  ; 命中返回译文，否则 0
add  esp, 4
test eax, eax
jz   tr_no_match
mov  [ebx + 0x34], eax          ; 1) 换指针
push eax
call tr_length
add  esp, 4
mov  [ebx + 0x38], eax          ; 2) 换长度（渲染器按字符数遍历）
mov  dword ptr [ebx + 0x30], 0  ; 3) width = 0 → 关掉 x 方向裁剪
tr_no_match:
popad
jmp  dword ptr [g_tr_orig]      ; = 0x4339D0B8，而不是 0x4334173D
```

实测（`test/hook_test.cpp`，本地伪造镜像 + 假虚表）：命中项的三个参数都被改写，未命中项原样透传。

## 2.1 为什么下拉框里只显示 `O` / `OF`（宽度裁剪）

type 10 的派发 thunk（`0x433CC8FB`）会推 **6 个参数**：

```
433CC902  movzx eax, word ptr [edx+0x14]   ; payload+0x14 = 文本长度(16 位)
433CC90A  push eax                          ; arg6 = length
433CC90B  lea edi, [edx+0x16]               ; payload+0x16 = 内联 UTF-16 文本
433CC90E  push edi                          ; arg5 = text
433CC90F  push dword ptr [edx+0x10]         ; arg4 = width   ← 裁剪宽度
433CC912  push dword ptr [edx+8]            ; arg3 = color
433CC915  push dword ptr [edx+4]            ; arg2 = y
433CC918  push dword ptr [edx]              ; arg1 = x
433CC91D  call dword ptr [esi+0x3c]
```

实现体（`0x4334173D`）把 `width` 当**对齐/裁剪上限**，再原样传给字形循环 `sub_1667C`：

```c
sub_7AA26(font, &text);              // 测出真实宽度 v26
if (width && v26 > width) v26 = width;   // 对齐用
...
v39 = 0x7FFFFFFF;
if (width) v39 = x + width;          // sub_1667C 里：绘制右边界
...
if (x_after_glyph >= v39) return;    // 超出就停
```

结论：

- 下拉项的事件里 `width` 只按原串 `"Off"` 的宽度给（约一个选项列宽），把 `"OFF"` 换进去后
  字形累加宽度越过 `x+width`，**渲染器直接停止 → 显示成 `O` / `OF`**；
- 闭合状态的 input 框 `width` 足够大，所以显示完整；
- 只改指针不改 `length` 时，若译文比原文短（`关闭` = 2 < `Off` = 3），渲染器会多读一个字符。

命中翻译时把 `width` 置 0 是安全的：0 表示“不裁剪”（`v39` 保持 `0x7FFFFFFF`），
而对齐用的宽度会退回“实测宽度”，右对齐/居中的位置反而更准。

## 3. 翻译表

默认硬编码一条（验证裁剪修复用的 ASCII 测试串）：

```
Off = OFF
```

确认下拉框里能完整显示 `OFF` 后，把 `tr.cpp` 里那行改成 `add_entry(L"Off", L"关闭");` 即可。

`skeet_tr.txt` 默认放在 **`debug.exe` 同目录**，格式每行 `原文=译文`，`#` 注释：

```
# skeet_tr.txt
Off=关闭
```

路径优先级：`SKEET_TR_FILE` 环境变量 → **loader 传进来的 `<debug.exe 目录>\skeet_tr.txt`** → `C:\skeet_tr.txt` 兜底。

> DLL 跑在 `csgo.exe` 里，拿不到 `debug.exe` 的目录。所以 loader 启动时（`wWinMain` →
> `publish_translation_path`）把一个 `{magic, length, wchar path[260]}` 结构写进命名共享内存
> `Local\skeet_tr_cfg`，DLL 用 `OpenFileMappingW` 按名字取。所以换目录只要跟着 `debug.exe` 走。

- 编码：UTF-8、UTF-8 **带 BOM**、ANSI(GBK)、UTF-16LE/BE 都支持。
- `Off = 关闭`（等号两边有空格）也能识别；键名大小写敏感。
- **热重载**：每 500ms 检查文件时间戳，改完存盘后下一帧就生效，不用重启 loader/游戏；
  `#addcjk` 也是热加的。
- 内置兜底：文件读不到时用 **探针串** `Off=OADilmn`（这几个字母确认有字形）：
  - 菜单里看到 `OADilmn` → 翻译表没读到；
  - 菜单里变空白 → 翻译表读到了，只是字体没有 CJK 字形（下一步加 `#addcjk`）。
- 日志：`<debug.exe 目录>\skeet_tr.log`（loader 通过共享内存把路径也传过去；目录不可写时退回 `%TEMP%`）。
  第一行就是 `map file loaded (utf-8 bom), entries=1` 或 `map file not found`。

## 4. 构建

```powershell
# 推荐：一条命令，按依赖顺序编 DLL -> 删 resource.res -> 编 exe
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Debug
```

或者用 VS 打开 `source code\debug.sln`（包含 `debug` / `skeet_tr` / `skeet` 三个工程，已配好依赖）直接生成。

产物：

| 文件 | 说明 |
|---|---|
| `source code\debug\skeet_tr.dll` | 本模块（22,016 B，只导入 kernel32） |
| `debug.exe` | loader（RCDATA 102 = skeet.dll，RCDATA 103 = skeet_tr.dll） |

注意：`rc.exe` 不跟踪 RCDATA 依赖，`skeet_tr` 的 PostBuild 会删掉
`obj\<Config>\resource.res`，强制 loader 重新嵌入，否则会嵌入旧 DLL。

## 5. 运行时日志

`%TEMP%\skeet_tr.log`（每次加载重写）：

```
gamesense skeet_tr log
entries=1, waiting for image...
image ready, slot=0x4346F4A8 -> 0x4339D0B8
hooked, hook=0x... -> 0x4339D0B8
draw x=0x0000007B y=0x0000012B width(orig)=0x0000002A len(orig)=3 newlen=3 text="Off"
draw x=0x0000007B y=0x0000020B width(orig)=0x00000014 len(orig)=3 newlen=3 text="Off"
```

`draw ...` 是诊断行：**每个不同的 (x, y, width, len) 只记一次**，记录的是**改写前**的原始参数：

- `width(orig)`：下拉项那个值明显更小，就是 `O`/`OF` 被截断的直接原因（现在已经置 0 绕开）；
- `len(orig)` / `newlen`：渲染器按 `len(orig)` 遍历字符，命中后会被改写成 `newlen`；
- `text`：命中的原文（ASCII 显示，非 ASCII 用 `.`）。

loader 侧日志（`debug_log.txt`）会多两行：

```
embedded skeet_tr.dll size=16896
inject_dll(skeet_tr) -> ok (took 31 ms)
```

## 5.1 字体诊断与 CJK 追加

菜单字体不是 GDI 直接画的：每个 font 对象自带 DC + DIB 图集（构造函数 `0x4334E194`，
参数 `(face, height, flags)`，内部 `CreateFontIndirectW` + `GetFontUnicodeRanges` + `CreateDIBSection`）。
渲染时逐字符在**字体链**里找第一个能画该字符的字体，找不到就跳过——所以缺字会静默消失。

`font_select`（`0x4332AF73`）返回 `renderer + (scale==1 ? 0x108 : 0x15C) + {0, 0xC, 0x18, 0x30}`，
槽位由样式标志位选择：`flags&2 -> +0x18`、`else flags&1 -> +0x0C`、`else flags&4 -> +0x30`、`else +0`。

**第一次命中翻译时**会在日志里 dump 一次字体信息：

```
renderer=0x4347992C
scale bits=0x3F800000
font[0] face=0x4345xxxx size=14 flags=0x0 "Verdana"          ← 7 个描述项 {face,size,flags}
...
setA +0x00000000 node=0x0xxxxxxx [font next ...]            ← 两条字体链的节点
setB +0x00000000 node=0x0xxxxxxx [...]
```

想直接试“追加 CJK 字体”，在 `C:\skeet_tr.txt` 里加一行指令：

```
#addcjk
Off=关闭
```

它会在 dump 之后用 `create_font(L"Microsoft YaHei", 描述表里的 height, flags)` 造一个字体对象，
包成链节点 `{font, 0, next=0}` 挂到两条链的每个槽尾部。日志会多两行：

```
cjk: create(font,height)=0x0xxxxxxx -> 0x0000000E
cjk: font appended to chains
```

注意：`0x108` 那条链是 `scale==1.0` 用的，`0x15C` 是缩放后用的，两边都挂。

## 6. 注意事项

- **宽度裁剪**：已在 stub 里把 `width` 清零（见 §2.1）。如果某个控件依赖裁剪来避免溢出，
  长译文可能稍微画出背景框外，属正常现象。
- **字体**：`关闭` 需要 CJK 字形。如果菜单里显示空白/方块，先用 ASCII 串
  （`add_entry(L"Off", L"OFF")`）确认渲染正常——能显示 `OFF` 就说明 hook 没问题，
  剩下的只是字体（需要 `cn_patch` 的字体替换生效，或菜单本身用的字体带 CJK）。
- 只挂 `type 10`（菜单/文字绘制）；控制台、Steam overlay 文字不走这条虚表。
- 挂钩幂等：槽里已经是本模块地址时直接返回，不会重复包一层。
- 每次注入都是新的模块实例（旧的在 %TEMP% 里留下的 `gs_*.dll` 由 loader 下次清理）。
- 作弊 unload / 进程退出时不还原虚表——槽会指向已释放模块，但那时进程已经结束，无需处理。
- 文本按 UTF-16 处理，替换串比原文长也没关系（原来的 `text_measure` 会重新量宽度）。
