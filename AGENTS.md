# AGENTS.md

TsFileViewer 的项目上下文。内容从 README、CMake 配置、git 历史，以及本机 pi / Claude Code 的历史会话中提炼。

## 项目概览

Qt5 GUI 的 **只读** tsfile 查看器：打开 `.tsfile`，浏览 device/table → 参数树，右侧表格看值，QCustomPlot 画曲线。

- 同时支持两种数据模型：树模型（COMAC/DPR TsFileArchive 产出）和表模型（Apache upstream），也支持混合文件。
- 版本号唯一来源是 `Src/Version.h` 的 `APP_VERSION`；CI tag 需与之匹配（`v0.1.4` ↔ `"0.1.4"`）。当前 0.1.4。
- 仓库 `github.com/kkzi/TsFileViewer`，启动时会查 releases 检查更新。

## 构建与测试

必须先有 MSVC 环境。直接用仓库脚本：

```bat
_build.bat              :: 默认 msvc-release
_build.bat msvc-debug
```

`_build.bat` 内部 `call` VS2019 Professional 的 `vcvars64.bat`，然后 `cmake --preset` + `cmake --build --preset`，末尾打印 `BUILD_EXIT=%ERRORLEVEL%`（0 才算通过）。

从 MSYS/git-bash 调用：`cd /d/Code/TsFileViewer && cmd //c _build.bat`。

- Preset：`msvc-release` / `msvc-debug`，Ninja 生成器，编译器写死为 `cl`。
- 依赖：Qt 5.15.2（`QTDIR` 环境变量）、VS2019 x64。
- 首次检出要 `git submodule update --init`（`3rd/tsfile`、`3rd/qcustomplot`）。
- 产物：`Build\Release\bin\TsFileViewer.exe`，`qcustomplot.dll` 会拷到旁边；Qt DLL 需在 PATH 或另行部署。
- 无自动化测试。验证方式是构建 + 启动 + 用真实数据集人工核对。
- CI：`.github/workflows/build.yml`，windows-2022 + `ilammy/msvc-dev-cmd` + `jurplel/install-qt-action`，tag `v*` 时 `windeployqt` 打包发 release。`permissions: contents: write` 是发布上传必需的。

## 目录结构

```
Src/MainWindow.{h,cpp}     主窗口：工具栏、参数树、表格、plot、统计栏、导出（最大的文件）
Src/TsFileDocument.{h,cpp} 数据层：后台 Worker 线程，打开/查询/导出，多文件路由表
Src/Models.{h,cpp}         ParamTreeModel（device→参数两层树）、ValueTableModel
Src/Theme.h                扁平单色主题（songbird 风格）
Src/PathBridge.h           （已删，恒等透传层内联；历史见陷阱一节）
Src/LoadingOverlay.{h,cpp} 打开/翻页/导出期间的半透明遮罩
Src/TableFixture.cpp       控制台小工具，生成表模型 tsfile 供手测（COMAC 归档只有树模型）
Tools/SpanProbe.cpp        诊断工具：footer/chunk 级统计探针
Tools/SslProbe.cpp         诊断工具：打印 OpenSSL 加载状态并做一次真实 HTTPS GET
Tools/IconGen.cpp          图标生成
3rd/tsfile                 submodule，Apache tsfile C++（fork: kkzi/tsfile，分支 fix/cpp-ts2diff-float-double-batch-prefix）
3rd/qcustomplot            submodule，QCustomPlot 2.1.1 CMake 镜像
```

## 关键决策

**多文件聚合**（`git 651fa34`）
- 入口是 Open 对话框的**多选文件**，不是"Open Folder"。曾实现过 Open Folder 按钮，后按要求移除（那版 `entryList` 还有返回文件名而非路径的 bug）。
- 合并键 `device + '\x01' + measurement`；树/表两种模型的参数进同一张 `routes_` 表。
- 参数按 device 聚合成两层树，**不是**拉平列表（早期方案是拉平，后被修正）。
- 打开时只读 footer（schema + `Statistic` 的 `count_`/`start_time_`/`end_time_`，零数据解码），所以秒级完成。`Statistic` 这三个成员是 public，可直接读。
- 翻页：全局行号 → 定位文件 → `queryByRow` 本地 offset，跨文件边界时一页拼两段，UI 无感。
- 单文件与多文件共用同一条聚合路径：`Worker::open()` 只额外做修复流程（RestorableTsFileIOWriter 就地截断，多文件模式刻意不做），然后调 `openFiles({path}, repaired, truncated)`。单文件模式下 codec 采集（逐序列 `get_timeseries_schema` 头读）才会运行，多文件跳过。查询/导出统一的 Segs 段列表：单文件 = 只有一段的路由。（2026-09-08 合并，此前是两条独立打开路径 + `files_.size()==1` 退化分支。）
- 坏文件默认跳过并计数（`skippedFileCount`），不逐个弹修复对话框；单文件模式仍提供 `RestorableTsFileIOWriter` 就地修复（`git 1107986`）。

**如实反馈，不掩盖**
- 用户明确要求：tsfile 自身有问题导致显示/统计异常时，viewer 应如实反馈，而不是替它兜底。
- 检测到损坏或矛盾时，左侧树用**红色字体**标记文件/参数（`ParamRoute::suspicious`），跳过的文件在 tooltip 里按路径列出。
- 乱序行按原样展示，**不静默重排**。

**span 用 max−min，不用 last−first**（`git 74a5b85`）
- `last - first` 只有序列单调时才是时长。见下方陷阱。

**时间单位：按幅值归一 ms/us/ns → 内部毫秒**（2026-08-29 定案，同日修订）
- 生态事实：IoTDB 引擎有 `timestamp_precision` 配置（ms/us/ns，默认 ms，首启后不可改），发行包 `iotdb-system.properties.template:1243`；java tsfile 的 `tools/DateTimeUtils.getInstantWithPrecision` 接受三种精度。但精度只存在引擎配置里，**tsfile 文件本身不记录单位**，C++ 库也不感知（int64 透传）。
- 所以 TsFileArchive 写微秒是合法的 us 模式，不是偏离规范；viewer 作为独立 reader 只能按幅值判断：真实采集数据都在 2000 年后 —— ms ≤ ~1e14（到公元 5138 年）、us 在 [1e14, 1e17)、ns ≥ ~1e17，三个数量级互不重叠。
- 实现：`normalizeTsToMs()`（`TsFileDocument.cpp`）统一归一到内部毫秒；作用于 footer 统计（路由排序/重叠检测/对话框）和两处行循环（查询/CSV 导出）。
- 决策史（避免重弯路）：同日早些时候曾因「QuickStart 文档写 ms」定案严格按毫秒、不兼容 TsFileArchive；核实 IoTDB 配置后修订为幅值归一。QuickStart 那句只描述默认精度。
- 边界：真正的 1973 年前的 us 数据会被误判为 ms（数量级重叠区），对飞行测试数据不成立。

**呈现格式约定**
- 时间：表格 Time 列、tracer 提示为 `hh:mm:ss.zzz`；toolbar Range 为 `yyyy-MM-dd hh:mm:ss.zzz`；plot x 轴用自定义 `ClockTicker` 按当前 range 自适应（<0.5s → 毫秒，<1.5 天 → `hh:mm:ss`，更长 → 带日期）。
- 数值：统一走 `ValueTableModel::formatValue`（English locale，千分位，**不用科学计数法**）；浮点类型 `f,6` 六位小数，整数类型（INT32/INT64/TIMESTAMP）**无小数**，BOOLEAN 显示 true/false（plot 用 0/1，y 轴整数刻度）。覆盖 Value 列、min/max、状态栏、plot tooltip、tracer、y 轴刻度（`plot_->setLocale` + `setNumberFormat("f")`，精度按类型 0/6）。mean 始终保留小数（均值本质可分数）。CSV 导出：数值保留原始全精度，时间为秒（毫秒精度，尾零去掉）。
- 交互细节：搜索框 300ms 防抖，Enter 立即生效；plot 缩放有界（缩小不超过 `(0.8·min, 1.2·max)`，放大至少留 10 个点）；滚动条宽/高为 10px（半厚，handle 最小 32px 保证可抓）；tracer 只在显式激活时出现（双击/Enter/点击曲线），普通选中行不触发。状态栏不重复参数栏的 min/max/mean/n，只放 SFID 分析这类独有信息。

**代码与提交约定**
- 代码文件只用英文，必要处才加注释（来自全局 `~/.claude/CLAUDE.md`）。
- 对话和文档用中文。
- 提交信息用英文，祈使句 + 冒号分组，如 `Multi-file open: aggregate params across selected files`。

## 陷阱与教训

**footer 统计可能撒谎，且会毒化拼接顺序**（已确诊，`git 74a5b85`）

某个 W0 尾文件里 `TE_REAR_1` 的最后一个 chunk 统计是坏的：

```
chunk 7   11:29:04.750 .. 11:30:06.625  count=16
chunk 8   11:00:08.375 .. 11:30:08.250  count=16   <== BACK JUMP
```

timeseries index 的统计是对下属 chunk 做 min/max 归并，所以这一个坏 chunk 把整个文件的记录起始时间拽回 30 分钟前。`openFiles()` 按该 footer 起始时间排序决定拼接顺序，于是尾文件被排到最前，`span = last - first` 算出 **−0.125 s**。

- 已修的是**呈现**：span 改为 `max(ts) - min(ts)`，矛盾参数标红。
- **未修**：底层拼接顺序仍由坏统计决定，那些行在表格和曲线里依然乱序。备选方案是把排序键从文件级 index 统计换成 chunk 0 的 `start_time_`（chunk 按时间顺序写入，所以 chunk 0 反映真实首行），加 `startTs <= endTs` 守卫。
- 注意历史记录里的冲突：早期 Claude 会话曾断言该 chunk 的**数据**也是垃圾（时间戳解出 `-8.2e18`）。后续 pi 会话推翻了这个结论——那些垃圾来自 `SpanProbe --rows` 模式自身的 bug（同一 149 行序列吐出 3537 万行、每行时间戳相同、不同次运行垃圾还不一样），不是文件的事实。该模式已删除；结论只停在 footer 层面。

**诊断要用 footer/chunk 级，不要全量扫行**

`Tools/SpanProbe.cpp` 共 5 个模式（用法注释在文件头）：

| 模式 | 用途 | 代价 |
|---|---|---|
| `--list <file>` | 列出 device.measurement 名字 | footer |
| `--stats <dir> <dev> <meas>` | 只看 footer 聚合统计 | footer |
| `--chunks <dir> <dev> <meas>` | **per-chunk 统计，首选** | footer |
| `--range <startMs>,<endMs> <dir> <dev> <meas>` | 时间窗内的行，用来分辨「坏统计」还是「真乱序」 | 只解码相交 chunk |
| 无 flag（`<dir> <dev> <meas>`） | 每文件全行扫描 | 极慢 |

- `--chunks` 直接读 footer 里的 per-chunk 统计，零数据解码，每文件秒级，能暴露**文件内部**的乱序，并标 `<== BACK JUMP`。
- 全行扫描模式跑了一个多小时、生成 1.8 GB 输出后被放弃。21 GB 数据集顺序扫描约需 1.5 小时。
- `SpanProbe` 必须传 Windows 风格路径（`C:/...`）；传 MSYS 路径（`/c/...`）会静默退出 1。
- device 名传入时不要带 `--list` 多打印出来的那层 `root.` 前缀。
- `--rows` 模式已删除（bug 无法低成本修复且曾误导排查，见上方历史冲突条目）。
- 单独编 SpanProbe 而不进 MSVC 环境会挂在找不到 `sstream`。

**tsfile 库细节**
- `SimpleList::Iterator` 用 `it.get()`，不是 `*it`（`common/container/list.h:59` 只有 `T& get()`，没有 `operator*`）。
- `AlignedTimeseriesIndex::get_statistic()` 是虚函数，已委托到 `value_ts_idx_`，不需要 `dynamic_cast`。
- `Statistic` 的 `count_`/`start_time_`/`end_time_` 是 public（`common/statistic.h:122-124`），可零解码直接读。注意 `count_` 是 **`int32_t`**，跨文件累加总行数必须用 `qint64`。
- 内嵌 tsfile 用 ExternalProject 接入，因为它的子 CMakeLists 假定自己是顶层项目（依赖 `CMAKE_SOURCE_DIR`）。
- Windows 下 `zlibstatic[d].lib` 按配置切换名字，`tsfile.lib` 两种配置同名。
- 非 ASCII 路径：不再有桥接层（`PathBridge.h` 已于 2026-09-08 删除）。fork 打了 UTF-8 open 补丁后，库自己把 UTF-8 字节转宽字符调 `_wopen`，路径恒等透传即可。老的 8.3 短路径 / ACP 转码桥接（`git 2339ce4`）在那之后变成**有害**的：ACP 回退会把 GBK 字节喂给库，UTF-8→宽字符转换直接拒绝（错误码 28），已于 `git 20e4e38` 移除。注意 README 的 Notes 一节仍在描述旧桥接，**已过期，以代码为准**。
- 历史上两次 bump submodule 修的都是 schema 编码上报错误（GORILLA/LZ4 被误标 PLAIN、解出垃圾），编码现在从 chunk header 字节读。

**构建时的常见摩擦**
- 链接失败报输出文件被占用 = 旧的 `TsFileViewer.exe` 还在跑。它是只读查看器无未保存状态，直接结束进程再链接。
- Qt 过滤树时会忘记展开状态，改过滤条件后需要重新 `expandAll()`。

**参数规模**
- 真实数据集每文件约 18,800 个参数，96k 唯一参数、24 亿数据点、5.57 GB 是常态。所以 `get_timeseries_metadata()` 的反序列化是打开耗时的主要来源，任何按参数遍历的操作都要当心。

## 偏好约定

- 中文对话，代码英文。
- 先分析可行性再实施；大改动给出方案和工作量估计，等确认再动手。
- 改完要编译验证（`BUILD_EXIT=0`），必要时启动程序自查。
- 遇到需要长时间跑的诊断，先问清是等结果还是直接改。
- 临时产物（探针输出、临时构建目录）用完清掉。
- 不改动源 tsfile 数据。

## 待办与未验证项

- **拼接顺序仍未修**：只修了 span 的呈现。见上方 chunk 0 排序键方案。
- **无法复验负 span 修复**：原始数据集 `c:\fts_perf_runs\c105_tsfile_150m_1800s_20260827_081013\` 排查中途被删，硬链接副本也一并没了。同族 `c105_tsfile_100m_v2_20260827_084737` 尾文件干净，复现不出。要复验得重新生成一份带同类损坏尾文件的数据集。
- **chunk 8 是数据坏还是只统计坏**：未判定（`--rows` 探针已删，若需重探可用 `--range` 时间窗模式）。
- **`SpanProbe --rows` 去留**：已删（2026-09-08）。
- ~~更新检查存疑~~：已在 `git faa85e2` 解决。根因是 QtNetwork 运行时加载 OpenSSL 而 Qt 因授权不分发它，portable zip 里没有，更新检查静默失败于 "TLS initialization failed"。CI 打包步骤现在从 pinned Git-for-Windows 2.36.1 MinGit 取 `libssl`/`libcrypto` 1_1-x64（1.1.1 时代最后的官方二进制）放到 exe 旁边，`supportsSsl()=1`、releases API 可正常拉取。
- ~~Linux 理论可行~~：**三平台 CI 全绿**（2026-09-08，PR #1/#2）。windows-2022 + Qt 5.15.2（发版链不变）；ubuntu-24.04 + **Qt 6.8.2 LTS**；macos-14 arm64 + Qt 6.8.2（clang_64 通用包，原生跑）。CMake 双支持 `find_package(QT NAMES Qt6 Qt5)`，本机 Qt 5.15.2 照常编。POSIX 仅出 tar.gz artifact，不绑 Qt 运行时、不进 release。
- Qt/aqt 架构名陷阱（版本敏感）：Qt 5 → `win64_msvc2019_64`/`gcc_64`/`clang_64`；Qt 6.7+ Linux → **`linux_gcc_64`**（不是 gcc_64 也不是 linux64）；Qt 6.8 macOS → **`clang_64`**（唯一名，通用二进制）。Qt 6.8 无 qtsvg 模块（IconGen 改为 WIN32-only 后不再需要）。遇错用 `aqt list-qt <host> desktop --arch <ver>` 探针问真相。
- POSIX 构建陷阱：① zlib 官方 CMake 在 UNIX 下把 `zlibstatic` 重命名为 `z` → 链接 `libz.a`；② `qint64`（long long）与 `int64_t`（long）在 gcc/clang 下是不同类型，`qMin/std::min` 模板推导失败（MSVC 下同为 long long 不会暴露）；③ PR 的 `github.ref_name` 是 `N/merge`，含斜杠，打包文件名要消毒。

---

<!-- init-snapshot date=2026-08-29 commit=fab819a sessions=pi:1,claude:2,codex:0 -->
