# SAI2 液化 / 渐变映射 / 缩时摄影

> 给 PaintTool SAI Ver.2 补上 **液化**、**渐变映射** 和 **缩时录像**。
> 通过 DLL 注入提供界面，图像处理走剪贴板桥 —— 不修改 SAI 本体。

> [!WARNING]
> **本项目仅用于为你合法拥有授权的软件补充功能，不包含任何破解、授权绕过或反检测代码。**
> DLL 注入存在导致宿主崩溃的风险，重要作品请先保存。使用后果自负。

---

## 功能

### 液化
- 7 种工具：向前推进 / 膨胀 / 收缩 / 顺时针 / 逆时针 / **平滑** / **重建**
- 笔刷大小、强度可调，`[` `]` 快调
- 逐笔撤销（最多 12 步）
- **放大绘制**：全屏窗口，滚轮缩放 5%~800%，右键拖动平移
- **精确覆盖**：先用「框选」记一次位置，之后放大窗会 1:1 盖在原画位置上

### 渐变映射
- 按像素亮度查 256 级色带，等价于 PS/CSP 的渐变映射
- 色标可编辑：拖动改位置、双击改颜色、右键删除、点空白处新增
- 5 个预设打底，渐变始终从原图重算，反复调不会越调越脏
- 「还原原图」一键回到抓图状态

### 缩时摄影
- 每 1/2/5/10 秒抓一帧，静止帧自动跳过
- Windows 自带 Media Foundation 编码 H.264 MP4，**不依赖 ffmpeg / OBS**
- 输出到 `sai2.exe` 同目录的 `timelapse\` 子文件夹
- 可收起的置顶小窗，录制中不挡画

### 抓图方式
| 方式 | 特点 |
|---|---|
| **② 剪贴板** | 自动发 `Ctrl+A` `Ctrl+C`，**保留 alpha**、不含工具栏、原始分辨率 |
| **① 框选** | 屏幕抓取，可只取画面一小块；支持移动/缩放选区 |

### 全局热键（SAI 在前台即可）
| 快捷键 | 作用 |
|---|---|
| `F9` | 进入液化（抓图 + 开大图窗） |
| `F10` | 退出液化，弹确认：「是」写回 /「否」放弃 |
| `Ctrl+Alt+G` / `Ctrl+Alt+A` / `Ctrl+Alt+Z` | 抓图 / 写回 / 撤销 |

---

## 原理（为什么这么做）

SAI2 是**闭源、无符号、无插件接口**的二进制。实测：

| 检查项 | 结果 |
|---|---|
| 导出表 | 不存在 Export Directory |
| 插件字符串 | 无 `plugin` / `.8bf` / `manifest` |
| 渲染后端 | 仅 GDI（`SetDIBitsToDevice`） |

所以没有"官方插件"这条路。本项目选择：

1. **注入 `saimod.dll` 到 `sai2.exe`** —— 只用来挂窗口、抓画面、接管快捷键
2. **图像走剪贴板桥** —— `Ctrl+A`/`Ctrl+C` 取当前图层 → 内存里处理 → `CF_DIBV5`(带 alpha) 写回 → `Ctrl+V` 贴成新图层
3. **完全不碰 SAI 的内部结构**（图层位图指针、撤销栈），因此 SAI 小版本更新也不会失效

代价是明确的：写回是**新建图层**，位置有轻微偏移，需要手动对位后 `Ctrl+E` 合并；也**无法**接管 SAI 的画笔事件（不能像内置工具那样直接用笔刷在画布上液化）。

---

## 目录结构

```
src/saimod.cpp     注入进 SAI 的插件主体（面板 / 液化 / 渐变 / 缩时）
src/launcher.cpp   启动器：拉起 SAI + 注入 DLL
src/inject.cpp     注入器：把 DLL 塞进 64 位 sai2.exe
build.bat          一键构建
docs/USAGE.md      详细使用说明
```

---

## 构建

需要 **Visual Studio 2019/2022（x64 工具链）**。

```bat
build.bat
```

产物在 `build\`：

```
build\saimod.dll
build\SAI2启动器.exe
build\sai_inject.exe
```

手动构建（等价）：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

cl /nologo /utf-8 /EHsc /O2 /MT src\saimod.cpp /LD /Fe:build\saimod.dll ^
   /link /DLL user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib

cl /nologo /utf-8 /EHsc /O2 /MT src\launcher.cpp /Fe:build\SAI2启动器.exe user32.lib
cl /nologo /utf-8 /EHsc /O2 /MT src\inject.cpp   /Fe:build\sai_inject.exe  user32.lib
```

---

## 安装

1. 把 `build\` 里的三个文件放到一个目录（建议 `sai2.exe` 同目录下的 `saimod\`）
2. 目录里放一个 `sai_path.txt`，写 SAI 主程序的完整路径：

```
D:\PaintTool SAI\sai2.exe
```

3. 双击 `SAI2启动器.exe` —— 会拉起 SAI 并注入，SAI 右侧出现面板

> 也可以先自己开 SAI，再双击启动器，它只会补注入。

---

## 已知限制

- 写回是**新建图层**，位置有轻微偏移（SAI 粘贴行为决定），需手动对位
- 一次只处理**当前选中图层**，不包含下面图层
- 抓图长边上限 **4096**（超过会缩放，写回时再还原）
- 缩时录像长边限制 **1920**
- 修改 DLL 后**必须重启 SAI** 才生效
- 注入行为可能被杀软判为 `Behavior:Win32/DefenseEvasion`，需要自行加排除或放行

---

## 免责声明

- 本项目只做**功能补充**，不包含任何破解、授权绕过或反检测代码
- 请在你**合法拥有授权**的软件上使用
- DLL 注入存在导致宿主程序崩溃的风险，重要文件请先保存
- 使用后果自负

## License

MIT
