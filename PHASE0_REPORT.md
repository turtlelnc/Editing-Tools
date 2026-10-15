# Phase 0 实测报告

日期：2026-10-14。环境：Apple M3 MacBook Air，8 GB，macOS 26.6；FFmpeg 9.0.2、MLT 7.42.0、GStreamer/GES 1.28.8。原架构两份文档保持原样；新增的是验证程序和证据，不是产品实现。

**结论：Apple 原生 RAW 解码已跑通；MLT 值得优先进入后端正确性验证，但不能整工程直接加载；SQLite 保留，50 轨同步 ripple 的绝对起点方案未达到目标。Phase 0 尚未全面通过。**

## 1. Apple ProRes RAW 原生解码

实测素材为 Apple/iPhone 拍摄的 ProRes RAW HQ（`aprh`），4224×3024。来源是 MotionCam 公开对比素材的 `01_A001_09211049_C068.mov`。原文件约 4.37 GB；用 HTTP Range 获取头 16 MiB、尾 2 MiB，通过临时稀疏文件定位容器元数据，**仅保留前两帧完整 packet**，转封装为 `phase0/media/raw-first2.mov`。原临时稀疏文件已删除；样本 SHA-256 位于 `phase0/results/raw-sample.sha256`。

- `VTDecompressionSessionCreate`：普通 ProRes、RAW、RAW HQ 均返回 0；`VTIsHardwareDecodeSupported` 均 true；创建的会话硬件属性为 true。这是会话能力证据，不是该真实文件的硬件吞吐证明。
- `AVAssetReader`：真实 RAW HQ 两帧均得到 4224×3024 的 RGBA half-float 像素缓冲，最终状态 completed、error=nil；两次运行的像素 SHA-256 一致。一次两帧读取加哈希约 0.79 s，进程峰值 RSS 124.7 MiB。**不能将这个包含启动、复制和哈希的数字当持续解码 fps。**
- 原生 PNG 预览已目视检查，是可识别的海滩图像；应用源媒体的 180° `preferredTransform` 后输出 `phase0/results/raw-preview.png`。这是默认色彩转换预览，未与 Final Cut 的显影结果对标。
- 同一素材，FFmpeg 软件和 `-hwaccel videotoolbox` 两条路径均输出 0 帧，报 `Not yet implemented in FFmpeg`。`-decoders` 列出 `prores_raw` **不等于**这份 RAW HQ 素材可以解码。
- 直接从 `AVAssetReader(outputSettings:nil)` 获取压缩 sample 的额外探针没有获得有效 sample；不能用该探针确认真实 RAW 文件的强制硬件解码。它不推翻已成功的半精度像素输出路径。

**决策：macOS 增加 AVFoundation 原生媒体桥接，是有实测支撑的方向。** 先用该路径解码或生成代理，再交给跨平台渲染执行器。当前未验证其他相机、非 HQ RAW、白平衡/曝光控制、长片吞吐、HDR 显影与授权分发，因此不能宣称完整 RAW 产品支持。

证据：`native_probe.txt`、`native-decode.log`、`native-repeat.log`、`native-preview.log`、`raw-vt.log`、`raw-sw.log`。

## 2. GES 与 MLT：实际运行比较

统一基本条件：640×360、30 fps、相同 H.264 源、FFV1 视频输出、不输出音频。1000-clip 时间线每 clip 取源首帧；源资产复用。8 层测试为相同画面叠加，顶部不透明，属于解码/合成 smoke，不证明复杂透明混合正确性。时间包含启动和输出，数值是单次筛查，不是统计稳定的排名。

| 工作负载 | MLT 时间 / 峰值 RSS | GES 时间 / 峰值 RSS |
|---|---:|---:|
| 单 clip，4 秒 | 0.34 s / 68.3 MiB | 0.90 s / 113.0 MiB |
| 8 层，4 秒 | 1.50 s / 158.4 MiB | 2.07 s / 240.2 MiB |
| 1000 个单帧 clip | 1.09 s / 70.1 MiB | 105.06 s / 145.3 MiB |

GES 的 1000-clip 数字来自没有竞争 GES 进程、独立输出路径的重复运行，退出码 0。首轮重试有进程干扰和超时，不能作为有效比较证据。**单帧 clip 是刻意放大切换/seek 成本的压力条件**；MLT 复用同一 producer，GES 创建独立 clip/source，测试反映当前适配路径的差异，不能推出所有实际工程有约 100 倍差距。

正确性检查比计时更影响选型：

- MLT 单 clip 输出 120 帧；GES 单 clip / 8 层输出 121 帧；GES 100 clip 输出 101 帧、独立 1000 clip 输出 1001 帧。GES 的期望区间内亮度哈希相同，但额外末帧使严格帧数验收失败，适配器需要明确半开区间和尾帧处理。
- 未标记色彩空间的源在 MLT 默认 BT.709 profile 下出现亮度差异。给源显式写入 BT.709 标签后，MLT 的 120 帧亮度逐帧完全一致。这个控制排除了把色彩解释差异直接算成解码错误的混淆。
- 即使明确色彩空间，MLT 整帧 YUV420 哈希仍不完全一致；按帧序号对齐的 PSNR：Y=inf，U=38.24 dB，V=36.79 dB，平均=42.22 dB。剩余差异与内部色度格式转换相容，但尚未对其路径作完整归因；不能宣布像素完全等价。以毫秒 PTS 直接 framesync 会错配帧，报告只采用帧号对齐结果。

MLT 大型 XML 首帧测试使用 C API：加载 XML、获取长度、seek、拉一帧和像素，不渲完整文件。

| 条目数 | 加载并取首帧 | 峰值 RSS |
|---:|---:|---:|
| 1000 | 0.19 s | 48.5 MiB |
| 10000 | 12.09 s | 108.2 MiB |
| 100000 | 120 s 超时终止 | 未取得完整数值 |

这是当前 XML 组织/加载路径的负面筛查，不证明所有 MLT API 都同样慢。**数据库局部查询 → 小范围后端图仍然必要；不能把整个十万条目数据库直接转为后端常驻 XML。** GES 的十万条目加载未实测。

包体：MLT bottle 解压约 5.0 MiB；完整 GStreamer bottle 约 227 MiB；现有 FFmpeg prefix 约 52 MiB。这些都**不含完整依赖闭包，不是最终 App 安装体积**。完整 Homebrew 安装因磁盘预算被停止，使用项目内重定位 bottle；缺少的两个必要小依赖 orc、json-glib 单独安装。MLT 实测仅加载 core/xml/avformat 模块；现有 FFmpeg 构建含 GPL，故这一实验环境不构成 LGPL 分发验收。

**决策：暂以 MLT 为下一轮执行器候选，保留 GES；尚不能将 MLT 定为已通过正确性集的主后端。** 仍需透明合成、转场、变速、字幕、音频尾音、NTSC 与随机 seek 的共同金标准。

证据：`backend.json`、`ges.json`、`ges-isolated.json`、`output-verification.json`、`tagged-control.json`、`tagged-psnr-frameindex.log`。

## 3. 大型工程：数据库与编辑

初始测试覆盖 1 万/10 万 item、20/50 轨；另含每 item 10 个关键帧、同数量 marker，运行 1000 轮局部查询→ripple→立即 undo→往返校验。每十轮一次全轨 ripple；每百轮一次 100 项参数更新。SQLite WAL、synchronous=FULL。

首轮 marker 缺少 `(track,t)` 索引，50 轨 ripple p99 为 283 ms。**该结果不能独立证明布局失败。** 补齐索引后，重新用相同 seed 的 50 轨工作负载验证，并让分块方案使用相同随机操作抽样、相同参数更新：

| 10 万 item、100 万关键帧、10 万 marker、50 轨 | 绝对起点，补齐索引 | 分块相对起点，256 项/块 |
|---|---:|---:|
| 范围查询 p95 | 0.589 ms | 0.944 ms |
| ripple p99 | 251.759 ms | 20.773 ms |
| undo p99 | 258.522 ms | 20.052 ms |
| 进程峰值 RSS | 21.6 MiB | 20.5 MiB |

绝对起点方案范围查询通过 `<20 ms`；ripple 未通过复审 `<200 ms`。100 个**参数更新**一批 p95 0.955 ms，不能当作 100 个 ripple 的批量通过证据。

额外写放大控制：复制同一规模数据库，清空 WAL、关闭自动 checkpoint，做一次从开头影响全部 50 轨的 ripple。绝对起点新增 WAL 6,237,712 bytes，分块新增 1,277,232 bytes。它测的是此次新 WAL 文件字节数，**不是操作系统/SSD 实际写入字节数**。长循环中 WAL 文件复用，文件增长为 0 不代表没有写入；原始该指标明确保留限制。索引基线 WAL 峰值约 8.9 MiB，显式 passive checkpoint 最大 1.85 ms；自动 checkpoint 成本包含在操作延迟内。

1000 轮立即 undo 的最终起点均正确；其耗时没有随历史出现明显增长。但这不是长撤销链测试。关键帧作为局部时间的静态行存在，未做曲线编辑；字幕、涟漪删除、redo、渲染后再编辑、缓存闭包和复用率均未覆盖。

**决策：保留 SQLite；分块布局值得进入完整编辑语义验证。** 目前只有静态分块，插入、删除、跨块移动、重平衡与崩溃恢复未验证。两布局性能差异是筛查证据，不是最终存储选型验收。

证据：`storage-original-unindexed.json`、`storage-indexed-control.json`、`chunk.json`、`wal-control.json`。

## 4. FFmpeg 规模与分段反例

100/500/1000 个独立输入 demuxer，各限制一个解码线程，concat 每输入首帧，均成功退出：

| 输入数 | 时间 | 峰值 RSS |
|---:|---:|---:|
| 100 | 0.35 s | 306.9 MiB |
| 500 | 1.30 s | 915.0 MiB |
| 1000 | 3.07 s | 1055.8 MiB |

1000 输入另记录 peak memory footprint 约 2.76 GiB。重复同一路径、只取首帧，不代表 1000 个独立大素材的持续播放；线程限制也属于实验条件。**“必然崩溃”被本例否定，“资源成本明显”得到支持。** 需要分段/局部图的理由应是测量成本与语义，而非假设输入数必然导致失败。

相同 4 秒素材切为两个 2 秒片段，FFV1 编码后流拷贝拼接：

- 无状态基线：120/120 帧，全部帧哈希一致。
- `tmix=frames=3`：帧数仍 120，但第 60、61 帧哈希不同；无预热切段破坏时域状态。这个反例足以否决“所有效果都可按 clip 边界独立切段”的规则。

小媒体上 H.264/ProRes Proxy 三次单帧请求约 35–127 ms，低于 1 s；不代表 4K/长 GOP 大文件 p95。两种素材软件和 VideoToolbox 都能解码 120 帧；4 秒小样本含启动的软解较快，不能据此否定硬件路径。H.264/HEVC VideoToolbox 还各完成 30 帧编码。当前 FFmpeg 有 xfade/overlay，**没有 ass/subtitles 滤镜**，字幕烧录能力未通过。

## 5. Phase 0 验收状态与下一步

已取得：真实 RAW HQ 原生像素输出；GES/MLT 两后端实际启动与渲染；十万条目千轮数据库筛查；FFmpeg 千输入测量；时域切段反例；小样本代理/硬件编解码探测。

尚未完成的原文/复审验收：精简 LGPL 构建与依赖闭包体积、真实 4K 长片/多机 RAW、完整音视频困难时间线、1 小时 NTSC 同步、字幕、变速、随机 seek 时序滤镜、GES 十万条目加载、100 个复杂 Op 原子批量、长 undo/redo 链、缓存正确性/复用率、20 个实际 MCP 工具 token 占用。当前只有文档，后几项需要先有最小领域/缓存/MCP 原型。

下一步最有价值的是：用**明确色彩与区间语义的小范围 MLT 适配器**跑困难音视频正确性集；同时验证分块模型完整编辑语义。上述验收通过前，暂停把通用分段/缓存当作既定架构。

## 6. 复现与来源

脚本均在 `phase0/`；结果与完整命令在 `phase0/results/`。`storage_bench.py`、`chunk_bench.py`、`media_bench.py`、`backend_bench.py`、`ges_bench.py` 为筛查程序；`native_decode.swift` 为原生 RAW 解码验证；`mlt_probe.c` 区分“XML 首帧拉取”与整条导出。运行脚本会重建其专用合成数据库和输出媒体；请勿把真实工程放到这些专用文件名下。

本次磁盘初始仅约 2.5 GiB；生成物约数百 MiB，完整依赖安装中途停止。没有删除既有工程、历史媒体或用户文件。比较没有重复统计置信区间，部分测试有同机并行负载与磁盘压力，因此以能力/风险筛查解释。

- [Apple ProRes RAW 的公开 API](https://developer.apple.com/documentation/avfoundation/avvideocodectype/proresraw)
- [Apple 官方授权与 SDK 信息](https://support.apple.com/en-us/118584)
- [FFmpeg 9.0 RAW 解码实现](https://ffmpeg.org/doxygen/9.0/prores__raw_8c_source.html)
- [GES 官方架构](https://gstreamer.freedesktop.org/documentation/gst-editing-services/index.html)
- [GESPipeline：播放和编码](https://origin.gstreamer.freedesktop.org/documentation/gst-editing-services/gespipeline.html)
- [MLT XML 官方文档](https://www.mltframework.org/docs/mltxml/)
- [Homebrew GStreamer（含 GES）](https://formulae.brew.sh/formula/gstreamer)
- [本次 RAW 素材公开入口](https://f005.backblazeb2.com/file/MotionCam-Public/2025/ProResRAW_vs_MotionCamRAW/ProResRAW_vs_MotionCamRAW.html)
