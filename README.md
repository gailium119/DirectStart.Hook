# Windows 8.1 Start Menu Revival — winmm.dll 独立版

把原来的 Windhawk mod（`win81-start-menu-revival_alpha-0_1_8.cpp`）合并进
`DirectStart.Hook`。不再需要 Windhawk：把生成的 `winmm.dll` 复制到 `C:\Windows`
并重启 explorer.exe 即可。

当前版本已按以下方式重构：

- **libvalinet**（`valinet/pdb/pdb.h`、`valinet/ini/ini.h`）负责 PDB 下载/解析和
  INI 读写，不再有自写的 Windhawk 兼容层。
- **MinHook** 只负责内联钩子本身。
- 依赖全部通过 **git submodule** 引用。
- 提供 **`DirectStart.slnx`**，VS 2022 直接打开/编译。
- 函数地址可以通过 **`DirectStart.ini`** 配置（也用作自动解析后的缓存）。

## 原理

- `winmm.dll` 是名称转发代理，导出与系统 winmm 相同的 180 个函数，全部转发到
  `C:\Windows\System32\winmm.dll`。
- explorer.exe 会导入 `WINMM.dll`。由于 winmm 不在 KnownDLLs 中，`C:\Windows`
  下的同名 DLL 会先于 System32 被加载，从而进入 explorer 进程。
- DLL 在 explorer 中启动工作线程，加载 `twinui.dll`、`dui70.dll`，用 MinHook 安装
  原 mod 的全部钩子。
- 符号解析：优先读取 `DirectStart.ini`；缺失项用 libvalinet 从微软公共符号服务器
  下载 PDB，再用 DbgHelp 解析，并把结果写回 INI。下次启动直接使用 INI，不再下载。

## 依赖与构建

要求：Visual Studio 2022（v143）+ Windows 10/11 SDK，x64。

```bat
git submodule update --init --recursive
build.cmd
```

`build.cmd` 会在缺少依赖时自动初始化 submodule，然后编译
`DirectStart.slnx`（Release|x64）。

也可以直接：

```bat
msbuild DirectStart.slnx /p:Configuration=Release /p:Platform=x64
```

或在 Visual Studio 2022 中打开 `DirectStart.slnx` 后生成。

产物：`build\x64\Release\winmm.dll`。

依赖的 submodule：

| 路径 | 项目 | 用途 |
| --- | --- | --- |
| `third_party/libvalinet` | https://github.com/valinet/libvalinet | PDB 下载/解析、INI |
| `third_party/minhook` | https://github.com/TsudaKageyu/minhook | 内联钩子（v1.3.4） |

## 安装

1. 以管理员身份打开任务管理器，结束 `explorer.exe`。
2. 把 `build\x64\Release\winmm.dll` 复制到 `C:\Windows\winmm.dll`。
   （不要放进 System32；那会全局替换系统 winmm。）
3. 重新启动 `explorer.exe`（任务管理器 → 运行新任务 → `explorer.exe`）。
4. 首次运行会下载 explorer/twinui/dui70 的 PDB，可能需要几十秒到几分钟。
   如果菜单没有马上出现，再重启一次 explorer。

卸载：结束 explorer，删除 `C:\Windows\winmm.dll`，重新启动 explorer。

注意：

- 仅支持 **Windows 8.1 x64**（代码里有 `_M_X64` 和 6.3 版本检查）。
- 与 ExplorerPatcher 等同样使用 `C:\Windows\winmm.dll` 的工具冲突。
- 其他进程加载该 DLL 时只做 winmm 转发，不安装任何钩子。

## 注册表设置

位置（HKCU 优先，缺失时回退 HKLM）：

```
HKEY_CURRENT_USER\Software\DirectStart\StartMenu
HKEY_LOCAL_MACHINE\SOFTWARE\DirectStart\StartMenu
```

| 值名 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `enabled` | REG_DWORD | 1 | 0 = 关闭开始菜单钩子（恢复 8.1 原生行为） |
| `log` | REG_DWORD | 0 | 1 = 把运行期钩子/诊断信息写入日志 |
| `theme` | REG_SZ | 空 | 开始菜单专用 msstyles 完整路径 |
| `muipath` | REG_SZ | 空 | explorer.exe.mui 完整路径 |
| `twinui_muipath` | REG_SZ | 空 | twinui.dll.mui 完整路径 |
| `theme_debug` | REG_DWORD | 0 | 记录主题类名匹配 |
| `symbols` | REG_SZ | 空 | PDB 下载目录（默认 `%LOCALAPPDATA%\DirectStart\symbols`） |
| `ini` | REG_SZ | 空 | INI 路径（默认 `%LOCALAPPDATA%\DirectStart\DirectStart.ini`） |

修改后需要重启 explorer 才生效。示例见 `settings.example.reg`。

## 函数地址 INI

默认路径：`%LOCALAPPDATA%\DirectStart\DirectStart.ini`。

```ini
[explorer]
CPearl::BuildStartMenu=0x12345
CTray::v_WndProc=0x12345

[twinui]
CImmersiveLauncher::ShowStartView=0x12345

[dui70]
DirectUI::Element::SetClass=0x247A0
```

- 节名：`explorer`、`twinui`、`dui70`（模块文件名去掉扩展名，小写）。
- 键名：PDB 公共符号名（原 mod 的完整签名会被自动归一化成这种形式）。
- 值的格式：
  - `0x12345`：**RVA**（模块基址 + 0x12345），推荐；
  - `module+0x12345`：同 RVA；
  - `va+0x7FFB12345678`：绝对 64 位虚拟地址（必须落在模块范围内）。
- 正常运行时不需要手写：resolver 会把解析结果自动写入 INI。
- `[meta]` 保存每个模块的构建指纹（文件时间 + 大小）。Windows 更新后指纹不匹配，
  该模块的旧地址会被忽略并重新解析，避免把钩子打到错误位置。
- 离线或 PDB 不可用时，可手工填写；要强制重新解析，删除 `[meta]` 或整个 INI。

示例见 `settings.example.ini`。

## 日志与符号

- 日志：`%LOCALAPPDATA%\DirectStart\StartMenu.log`（UTF-16）。初始化阶段始终记录；
  运行期钩子日志默认关闭，设 `log=1` 打开。日志超过 8 MB 时在下次启动轮换为 `.old`。
- 崩溃：注册了 VEH，把访问违例/栈回溯写入同一日志（即使 `log=0`）。
- PDB 缓存：`%LOCALAPPDATA%\DirectStart\symbols\explorer.pdb` 等（平铺）。
  由 libvalinet 通过 WinINet 下载；PDB 比模块文件旧时会自动重新下载。
- 如果系统无法访问微软符号服务器，可在 INI 中填写函数地址，或把 PDB 放到
  `symbols` 指向的目录。

## 代码结构

| 文件 | 作用 |
| --- | --- |
| `DirectStart.Hook/dllmain.cpp` | winmm 代理初始化、explorer 判定、工作线程 |
| `DirectStart.Hook/StartMenuHooks.cpp` | 原 Windhawk mod 的钩子逻辑（逻辑未改） |
| `DirectStart.Hook/DirectStart.h/.cpp` | 注册表设置、日志、INI、PDB/libvalinet 符号解析、MinHook 封装 |
| `DirectStart.Hook/WinMMProxy.*`, `winmm.def` | 180 个 winmm 导出代理（脚本生成） |
| `DirectStart.Hook/generate_winmm_proxy.ps1` | 重新生成代理时使用（需要 dumpbin） |
| `DirectStart.slnx` | VS 2022 解决方案 |
| `build.cmd` | 一键初始化 submodule 并编译 |

`StartMenuHooks.cpp` 中的符号表仍然保留原 mod 的完整签名；`DS_HookSymbols`
会把它归一化为 `Class::Function` 后与 INI/PDB 匹配。

## 已知限制

- 符号名依赖 8.1 的公共 PDB。`twinui` 的少数关键函数有原 mod 的特征码回退；
  explorer 的钩子没有特征码回退，因此 PDB 或 INI 地址至少要有一种。
- 首次启动时若 explorer 已构建开始菜单，钩子可能安装得太晚；重启 explorer 即可。
- 仅 64 位；ARM32/9600 主题等原 mod 支持范围未做额外验证。
