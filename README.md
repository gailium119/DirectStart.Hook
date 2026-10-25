# Windows 8.1 Start Menu Revival — d3d11.dll 独立版

把原来的 Windhawk mod（`win81-start-menu-revival_alpha-0_1_8.cpp`）合并进
`DirectStart.Hook`。不再需要 Windhawk：把生成的 `d3d11.dll` 复制到 `C:\Windows`
并重启 explorer.exe 即可。

当前版本：

- 注入 DLL 从 `winmm.dll` 改为 **`d3d11.dll`**（部分 explorer build 不导入 winmm）。
- 参考 **ExplorerPatcher 22000**（`Backports_22000_675_45`）的代理/代码组织方式，
  把原来的单一 hook cpp 拆成 `dllmain`、`StartMenu*`、`DirectStart`、`PatternScan`
  等文件。
- PDB/INI 使用 **libvalinet**，内联钩子使用 **MinHook**。
- 依赖通过 **git submodule** 引用；提供 **`DirectStart.slnx`**，可直接编译。
- 函数地址可读写 **`DirectStart.ini`**；特征码匹配封装在
  **[PatternScan.h](DirectStart.Hook/PatternScan.h)**（libvalinet 风格）。

## 注入方式：d3d11.dll

- `d3d11.dll` 是名称转发代理，导出与系统 `C:\Windows\System32\d3d11.dll` 相同的
  51 个函数，全部转发到真实 d3d11。
- 真实 d3d11 在**第一次导出调用**时由 `D3D11Proxy_Dispatch` 懒加载，避免在代理
  DllMain 的 loader lock 中运行 d3d11 自己的 DllMain。
- d3d11 不在 KnownDLLs 中，所以 `C:\Windows\d3d11.dll` 会先于 System32 被加载。
- 工作线程只在 `explorer.exe` 中启动并安装开始菜单钩子；其他进程加载时只做转发。
- 目标 explorer 必须导入 `D3D11.dll`。本机 Win11 的 explorer 导入的是 `dxgi.dll`，
  因此这里按你的要求使用 d3d11；如果某个 8.1 build 连 d3d11 也不导入，可用
  `generate_proxy.ps1` 生成其他名字的代理，并同步修改 `dllmain.cpp` 中的
  `LoadLibraryExW(L"d3d11.dll")`/`TargetName`。

## 参考 ExplorerPatcher 22000

`third_party/ExplorerPatcher` 是 git submodule，固定在
`Backports_22000_675_45` 分支（commit `5d8a238`），仅作为代码结构和代理/初始化
思路的参考，**不参与本工程编译**，也不需要初始化它内部的嵌套 submodule。

本工程借鉴/对应的部分：

| ExplorerPatcher 22000 | 本工程 |
| --- | --- |
| `dllmain.c` 的代理 DLL 加载与 explorer 判定 | `dllmain.cpp` |
| `hooking.h` 的钩子封装 | `DirectStart.cpp` 的 `DS_HookFunction` |
| `symbols.c` 的 PDB/符号加载 | `DirectStart.cpp` + libvalinet |
| `StartMenu.c` / `utility.c` 的功能拆分 | `StartMenu*.cpp` / `DirectStart.cpp` |

注意：EP 22000 实际使用的是 `dxgi.dll`；本工程只采用它的代码组织方式，注入目标
改为 `d3d11.dll`。

## 依赖与构建

要求：Visual Studio 2022（v143）+ Windows 10/11 SDK，x64。

```bat
git submodule update --init
build.cmd
```

`build.cmd` 缺少 libvalinet/minhook 时会自动执行
`git submodule update --init`，然后编译 `DirectStart.slnx`（Release|x64）。

也可以直接：

```bat
msbuild DirectStart.slnx /p:Configuration=Release /p:Platform=x64
```

或在 Visual Studio 2022 中打开 `DirectStart.slnx`。

产物：`build\x64\Release\d3d11.dll`。

| 路径 | 项目 | 用途 |
| --- | --- | --- |
| `third_party/libvalinet` | https://github.com/valinet/libvalinet | PDB 下载/解析、INI |
| `third_party/minhook` | https://github.com/TsudaKageyu/minhook | 内联钩子（v1.3.4） |
| `third_party/ExplorerPatcher` | https://github.com/valinet/ExplorerPatcher | 22000 参考代码（不编译） |

## 安装

1. 以管理员身份打开任务管理器，结束 `explorer.exe`。
2. 把 `build\x64\Release\d3d11.dll` 复制到 `C:\Windows\d3d11.dll`。
   （不要放进 System32。）
3. 重新启动 `explorer.exe`。
4. 首次运行会下载 explorer/twinui/dui70 的 PDB，可能需要几十秒到几分钟。
   如果菜单没有马上出现，再重启一次 explorer。

卸载：结束 explorer，删除 `C:\Windows\d3d11.dll`，重新启动 explorer。

注意：

- 仅支持 **Windows 8.1 x64**。
- 与同样使用 `C:\Windows\d3d11.dll` 的工具冲突。
- 只会在 `explorer.exe` 中安装钩子。

## 注册表设置

位置（HKCU 优先，缺失时回退 HKLM）：

```
HKEY_CURRENT_USER\Software\DirectStart\StartMenu
HKEY_LOCAL_MACHINE\SOFTWARE\DirectStart\StartMenu
```

| 值名 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | REG_DWORD | 1 | 0 = 关闭开始菜单钩子 |
| `log` | REG_DWORD | 0 | 1 = 运行期详细日志 |
| `theme` | REG_SZ | `%ProgramData%\DirectStart\aero.msstyles` | 开始菜单专用 msstyles 路径 |
| `muipath` | REG_SZ | `%ProgramData%\DirectStart\<系统语言>\explorer.exe.mui`，缺失时 en-US | explorer.exe.mui 路径 |
| `twinui_muipath` | REG_SZ | `%ProgramData%\DirectStart\<系统语言>\twinui.dll.mui`，缺失时 en-US | twinui.dll.mui 路径 |
| `theme_debug` | REG_DWORD | 0 | 记录主题类名 |
| `trace` | REG_DWORD | 0 | 预留 trace 开关，默认关闭 |
| `symbols` | REG_SZ | 空 | PDB 目录（默认 `%LOCALAPPDATA%\DirectStart\symbols`） |
| `ini` | REG_SZ | 空 | INI 路径（默认 `%LOCALAPPDATA%\DirectStart\DirectStart.ini`） |

修改后需重启 explorer。示例见 `settings.example.reg`。

## 函数地址 INI

默认 `%LOCALAPPDATA%\DirectStart\DirectStart.ini`：

```ini
[explorer]
CPearl::BuildStartMenu=0x12345
CTray::v_WndProc=0x12345

[twinui]
CImmersiveLauncher::ShowStartView=0x12345

[dui70]
DirectUI::Element::SetClass=0x247A0
```

- 节名：`explorer`、`twinui`、`dui70`。
- 键名：PDB 公共符号名；原 mod 的完整签名会自动归一化。
- 值格式：`0x12345`（RVA，推荐）、`module+0x12345`、`va+0x7FFB...`（绝对地址）。
- 已知 SHA-256 的模块直接使用编译进程序的 `KnownSymbols.g.h`，不读取、也不写 `[meta]`。
- 同一个模块可以包含多个版本，按 `模块名 + SHA-256` 选择；高版本 8.1 没有 PDB 时使用
  `StartMenuTwinUI.cpp` 的特征码扫描结果生成地址表。
- 未知 SHA-256 才回退到 INI/PDB；INI 只作为手工/离线地址表。
- 内置表按 `explorer.exe`、`twinui.dll`、`dui70.dll` 分别记录 SHA-256、文件版本和函数 RVA，
  源文件头部注释如：
  ```cpp
  // explorer.exe
  // Version: 6.3.9600.18009 (winblue_ltsb.150810-0600)
  // SHA256:  B2DAAB5598DAB842BEE3C397B46320D1402B44173A6527BCC33D6D28AFFA21E6
  ```
- 当前已内置：
  - `explorer.exe` 6.3.9600.18009 / 18231 / 18460
  - `twinui.dll` 6.3.9600.18231 / 18460 / 20363 / 23449
  - `dui70.dll` 6.3.9600.17415 / 23064
- 重新生成内置表：

  ```powershell
  python tools/gen_known_symbols.py --input _analyze/known_modules.json
  ```

  输入 JSON 是外置文件，不提交到 git（`.gitignore` 已忽略 `_analyze/` 和
  `tools/known_symbols.json`）。它读取 `_analyze/symbols/*.tsv` 并重写
  `DirectStart.Hook/KnownSymbols.g.h`。高版本 TwinUI 的 TSV 由
  `tools/check_twinui_signatures.py --emit-tsv` 生成。

示例见 `settings.example.ini`。

### 用已知 SHA-256 固化地址

`tools/pin_symbols.py` 按模块 SHA-256 记录/恢复函数地址，不猜测地址：

```powershell
# 记录（地址来自 PDB/IDA/日志）
python tools/pin_symbols.py --module C:\Windows\System32\twinui.dll `
  --database _analyze/known_symbols.json `
  --symbol "CImmersiveColor::GetColor=0x1AA754" `
  --symbol "CPearlResourceManager::_UpdateLayerVisibility=0x169034"

# 当前二进制 SHA-256 与记录一致时写入 INI
python tools/pin_symbols.py --module C:\Windows\System32\twinui.dll `
  --database _analyze/known_symbols.json `
  --ini %LOCALAPPDATA%\DirectStart\DirectStart.ini --apply
```

数据库按 `模块名 -> SHA256 -> 函数名 -> module+0xRVA` 保存；哈希不匹配时不会套用旧地址。

## 特征码匹配

`PatternScan.h` 仿照 libvalinet 的头文件风格，提供：

```c
BOOL VnPatternParse(const char* text, VnPattern* pattern);
const BYTE* VnPatternFindInRange(const BYTE* start, SIZE_T size,
                                 const VnPattern* pattern, SIZE_T* matchCount);
const BYTE* VnPatternFindInModule(HMODULE module,
                                  const VnPattern* pattern, SIZE_T* matchCount);
PVOID VnPatternFindUnique(HMODULE module, const char* text, SIZE_T* matchCount);
```

`StartMenuTwinUI.cpp` 的 PDB 失败回退使用它；`??`/`?` 为通配符，匹配只扫描可执行
节，`VnPatternFindUnique` 仅在唯一匹配时返回地址。

TWinUI 覆盖检查：

```powershell
python tools/check_twinui_signatures.py `
  --source DirectStart.Hook/StartMenuTwinUI.cpp `
  --dll _ref/twinui-symbols.dll
```

它会验证每个必需符号都有回退特征码/派生逻辑，并在目标 DLL 中确认特征码唯一命中。

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `DirectStart.Hook/dllmain.cpp` | d3d11 代理初始化、explorer 判定、工作线程 |
| `DirectStart.Hook/StartMenu.cpp` | 初始化/卸载、MUI/主题、CoCreateInstance |
| `DirectStart.Hook/StartMenuTwinUI.cpp` | twinui 钩子、磁贴/CHost/AdjustName、twinui 符号表 |
| `DirectStart.Hook/StartMenuExplorer.cpp` | CPearl/CTray/DesktopV2/GridPane 等 explorer 钩子与符号表 |
| `DirectStart.Hook/StartMenuPearl.cpp` | Pearl 颜色/动画钩子 |
| `DirectStart.Hook/StartMenuDui.cpp` | dui70 钩子与符号表 |
| `DirectStart.Hook/StartMenuDiagnostics.cpp` | 窗口/Apps 枚举、DuiTree、VEH |
| `DirectStart.Hook/DirectStart.h/.cpp` | 注册表设置、日志、INI、libvalinet PDB、MinHook |
| `DirectStart.Hook/PatternScan.h` | libvalinet 风格特征码匹配 |
| `DirectStart.Hook/D3D11Proxy.*`, `d3d11.def` | 51 个 d3d11 导出代理（脚本生成） |
| `DirectStart.Hook/D3D11ProxyDispatch.asm` | 懒加载/参数保持的转发分发器 |
| `DirectStart.Hook/generate_proxy.ps1` | 生成任意 DLL 的代理 |
| `DirectStart.slnx` | VS 2022 解决方案 |

## 日志与符号

- 日志：`%LOCALAPPDATA%\DirectStart\StartMenu.log`（UTF-16），`log=1` 打开运行期日志。
- 崩溃：`log=1` 时 VEH 把访问违例/栈回溯写入日志；默认 `log=0` 不创建日志文件。
- PDB：`%LOCALAPPDATA%\DirectStart\symbols\explorer.pdb` 等（平铺），libvalinet
  通过 WinINet 下载；比模块文件旧时自动重新下载。
- 离线：填写 INI，或把 PDB 放到 `symbols` 目录。

## 已知限制

- 需要目标 explorer 导入 `D3D11.dll`；否则换代理 DLL。
- 符号名依赖 8.1 公共 PDB；twinui 有特征码回退，explorer 没有。
- 首次启动时若 explorer 已构建开始菜单，钩子可能安装得太晚；重启 explorer 即可。
- 仅 64 位；Win8.1 实机效果需在目标系统验证。

## 已知现象：All apps 不足一页时文字变黑

这是一个已知的显示差异，当前不修复。

- 当 All apps 条目不足一页、无需滚动时，Explorer 使用 `DesktopProgramsMFU` 的 `SysListView32`，文字为黑色。
- 当条目超过一页、出现滚动时，Explorer 使用 `Desktop NSCHost` 的 `SysTreeView32`，文字为白色。
- 两者是 Explorer 的两个独立窗格，并不是简单的一段颜色判断；强制切换窗格会导致开始菜单空白或无法打开。
- 逐项 `NM_CUSTOMDRAW` 可以统一为白色，但会明显增加列表绘制开销，造成开始菜单卡顿。
- 通过 `GetThemeColor`/`LVM_SETTEXTCOLOR` 设置文字色又会被 ListView/TreeView 的视觉样式覆盖，无法稳定生效。

因此该现象只在“新装系统、应用数量很少、All apps 不满一页”时出现，通常应用装多后自然消失。若无法接受，可自行安装更多应用或使用第三方开始菜单工具。
