# ARCHITECTURE.md — 面向 AI Agent 的轻量视频剪辑引擎(MCP)

状态:技术调研 + 架构设计(未写实现代码)。调研日期 2026-10-14。
读者:后续负责实现的 Codex / 工程师。

## 0. 阅读约定与调研局限

证据标签:
- **[F]** 已验证事实:本次调研直接读取了来源(见第 10 节)。
- **[I]** 推断:由事实或通用工程经验推出,未在本次验证。
- **[H]** 假设/目标:需要 Phase 0 实测确认,不得当作事实使用。

**调研局限(重要):**
- WebSearch 全程不可用;GitHub API 被限流;WebFetch 对多数站点报错。因此**没有**做"现有视频剪辑 MCP 服务器"的普查,也**没有**读 GStreamer GES、Olive、OpenShot、Natron 的资料。第 9 节列出未调查项。
- 本机未安装 ffmpeg(`which ffmpeg` 无输出),**没有任何性能基准数据**。所有性能数字都是 [H] 目标。
- 关于 MLT、OTIO 的"谁在用、内部细节"中,凡未在第 10 节出处中出现的,均标 [I]。

---

## 1. 目标、非目标与关键假设

目标:单进程、本地运行的剪辑引擎;AI 通过 MCP 查询局部状态、做精确编辑、验证结果;支持视频/音频/字幕/转场/特效/预览/导出;macOS Apple Silicon 优先。

非目标(v1):实时 GUI 播放、云端协作、自研编解码器、自研 GPU 合成器。

关键假设(请在开工前确认,任一变化会改动方案):
1. **"预览"= 按需渲染单帧/低分辨率片段/缩略图拼图,返回给 AI 或人类查看**,而不是带播放头的实时播放器。依据 [I]:MCP 客户端是 Agent,没有屏幕交互循环。这一假设让我们避免自研实时播放引擎,是最大的简化。若需要实时 GUI,放到 Phase 4 另立前端(读同一个项目库)。
2. "超大型工程" = 单项目 10^4–10^5 个 clip、数十条轨道、数百个源媒体文件、数小时时长 [H]。Phase 0 用合成项目压测。
3. 不分发二进制时,GPL 不是问题;若要分发,需走 LGPL 路线(见 §4.3)。

---

## 2. 架构决策摘要(ADR 速览)

| # | 决策 | 状态 |
|---|------|------|
| D1 | 项目存储用 **SQLite(WAL)**,不用整文档 JSON/XML | 推荐,Phase 0 压测确认 |
| D2 | 时间线数据模型**借用 OTIO 的概念与 opentime 语义**,OTIO 仅作导入/导出格式,不作运行时存储 | 推荐 |
| D3 | 渲染后端 v1 用 **FFmpeg(子进程 + 程序生成的 filtergraph)**,置于 `MediaBackend` 抽象之后 | 推荐 |
| D4 | 渲染采用**分段计划(segment planner)**:图规模随"同时活跃轨道数"而非 clip 总数增长 | 核心设计 |
| D5 | 实现语言:**Rust**(Go 为可接受的备选) | 判断,非事实;见 §4.1 |
| D6 | MCP 面:少量(约 20 个)工具,批量原子编辑 + 修订号 + 局部查询 + 验证工具;导出用 Tasks 扩展并提供轮询回退 | 推荐 |
| D7 | 代理(proxy)/缩略图/波形 = 后台作业 + 内容哈希缓存 | 推荐 |
| D8 | MLT 作为"FFmpeg filtergraph 路线撞墙时"的备选渲染后端,不作为 v1 基础 | 推荐 |

---

## 3. 现有方案调研与比较

### 3.1 候选

| 方案 | 已验证事实 | 对本项目的判断 |
|------|-----------|---------------|
| **FFmpeg(libav* / CLI)** | [F] macOS VideoToolbox 解码 H.263/H.264/HEVC/MPEG-1/2/4p2/ProRes;编码 H.264/HEVC/ProRes。[F] `-hwaccel` 对不支持的流自动回退软解。[F] 硬件 H.264 解码器通常只支持 4:2:0 8-bit。[F] 恒定质量 `-q:v` 仅 Apple Silicon、FFmpeg ≥ 4.4。 | **v1 媒体后端**。覆盖解码/编码/滤镜/混音,无需自研。 |
| **MLT** | [F] LGPL 多媒体框架,"为视频剪辑设计"。[F] 模型:producer → playlist(轨道)→ multitrack → tractor(挂 filter/transition);MLT XML 文档推荐"为复杂工程最大化效率"的组织方式。[I] 被 Shotcut/Kdenlive 使用(本次未核实)。[I] 项目文档为整文档 XML,增量/局部查询需自建。[I] 其部分模块带来额外依赖和可能的 GPL 许可(未核实)。 | **备选后端**。模型成熟,但引入较重依赖和一套自己的运行时,与"轻量 + 增量 + AI 局部查询"目标不完全贴合。我们可由数据库生成 MLT XML 作为后端 B。 |
| **OpenTimelineIO (OTIO)** | [F] "编辑信息的交换格式与 API",**不是媒体容器**。[F] C++ 核心 + Python 绑定;含零依赖的 `opentime` 时间库。[F] 适配器含 FCP XML、AAF、CMX3600 EDL。[F] 类:Timeline、Stack、Track、Clip、Gap、Transition、Effect、LinearTimeWarp、Marker、External/Missing/Generator/ImageSequenceReference。[F] README 称成熟且广泛部署。[I] 内存模型是整文档加载,不适合局部/增量。 | **借概念,不借运行时**。数据模型与时间语义直接对齐;导入导出走 OTIO 以获得 NLE 互通。 |
| **GStreamer GES** | 本次抓取失败,**未调查** | 先验判断 [I]:macOS 打包体积和插件体系偏重。留作 Phase 0 补查项。 |
| **AVFoundation / Core Image / Metal** | 本次未调查 | 仅限 Apple,违背"未来可跨平台";Phase 4 可作为 macOS 优化后端(Metal 合成/特效)。 |
| 自研引擎(libav 解码 + 自写合成) | — | 工作量最大,v1 不做;`MediaBackend` 抽象保留此路。 |

### 3.2 结论
v1 = **自建"文档层 + 渲染计划层" + FFmpeg 执行层**。这样自研量集中在 AI 真正需要的部分(局部查询、精确编辑、验证),而不是编解码/合成。

---

## 4. 技术选型

### 4.1 实现语言(判断,非事实)
评估维度:安装体积、内存占用、FFI 难度(libav/Metal/VideoToolbox)、跨平台、MCP SDK 成熟度、可维护性、AI 辅助编码友好度。

| 语言 | 优点 | 缺点 |
|------|------|------|
| **Rust**(推荐) | [F] MCP 官方 SDK Tier 1。[I] 单一静态二进制、无 GC、无需运行时;枚举/类型系统适合"编辑操作 + 有理数时间"这类易错领域;rusqlite 成熟;以后做 objc2/Metal FFI 或 libav FFI 可行。 | [I] 编译慢、学习曲线;ffmpeg 绑定 crate 的维护状况**未核实**(所以 v1 不依赖它)。 |
| Go(备选) | [F] 官方 SDK Tier 1。[I] 单二进制、并发作业编排简单、上手快。 | [I] 进程内调 libav 需 cgo,较别扭;有理数/操作类型建模表达力弱于 Rust。 |
| TypeScript/Node | [F] SDK Tier 1。 | [I] 需要携带运行时,体积/内存违背目标。 |
| Python | [F] SDK Tier 1。 | [I] 运行时与依赖分发重,违背目标。 |
| Swift | 原生 macOS API | [F] MCP SDK 仅 Tier 3;跨平台弱。 |
| C++ | OTIO/MLT 同语言 | [I] 开发/维护成本最高,MCP 无官方 SDK(未见于列表)。 |

关键点:v1 通过**子进程**调用 ffmpeg/ffprobe,语言选择与 libav 绑定质量**解耦**;真正需要进程内 libav 是 Phase 4 的事,届时可在 Rust 内用一个小 C shim 或单独 helper 进程。**如果团队更熟悉 Go,Phase 1–3 用 Go 同样合理。**

### 4.2 项目存储:SQLite
- 理由 [I]:局部范围查询(按轨道+时间区间)、事务性批量编辑、增量更新、崩溃安全、零服务进程。整文档 JSON/XML 每次编辑都 O(N) 读写。
- 表(最小集):`media`、`track`、`item`(clip/title/subtitle 统一,含 `track_id,start,dur,src_in,media_id,speed,props_json`)、`transition`、`effect`(含关键帧 JSON)、`marker`、`oplog(rev, ops_json, inverse_json)`、`render_cache`。
- 索引:`item(track_id, start)`;`item(media_id)`。
- **不存间隙**:空隙由绝对起点隐含。涟漪(ripple)编辑 = 对 `(track_id, start >= x)` 的批量 `UPDATE start = start + d`。[H] 在 10^5 行量级应为毫秒~数十毫秒级,Phase 0 必须实测;若不达标再引入顺序键/树结构(不要预先做)。
- 单写者(引擎进程),WAL 允许后台作业并发读。
- 不引入 ORM。

### 4.3 FFmpeg 的分发与许可
- [F] FFmpeg 官方 LGPL 合规最简路径:不用 `--enable-gpl` / `--enable-nonfree`、动态链接、随附源码;并**避免 libx264(GPL)**。
- 推论 [I]:LGPL 构建下 macOS 的 H.264/HEVC 编码走 VideoToolbox(`h264_videotoolbox`/`hevc_videotoolbox`,[F] 存在)。其画质/码控相对 x264 的差异**未实测**,Phase 0 评估。
- 策略:引擎在 PATH 或内置位置寻找 ffmpeg(可配置路径),启动时探测 `-encoders`/`-filters`/`-hwaccels` 能力并向 AI 暴露(`get_capabilities`)。个人使用可直接用用户自装的 ffmpeg。
- 安装体积:[H] 精简 LGPL 动态构建应显著小于完整构建,**数值未知,Phase 0 测量**。

### 4.4 时间表示
- 采用 OTIO `opentime` 的 `RationalTime(value, rate)` 语义 [F: 其存在];在内部统一为 **i64 有理数 / 整数 tick**,禁止浮点秒。
- 视频以帧为单位对齐,音频以采样为单位对齐;NTSC(30000/1001)必须用有理数表示。所有 MCP 输入接受 `"hh:mm:ss:ff"`、帧号或 `{value,rate}`,输出统一回显规范形式。

---

## 5. 核心架构

```
          MCP (JSON-RPC)
AI Agent ───────────────► [mcp-server]  tools / resources / tasks
                              │
                 ┌────────────┼──────────────┐
                 ▼            ▼              ▼
          [timeline-core]  [render]        [jobs]
           模型/操作/校验   计划→分段图→后端   代理/缩略图/波形/导出
                 │            │              │
                 ▼            ▼              ▼
             [store]      [media]  MediaBackend trait
            SQLite WAL    ├ FfmpegProcessBackend (v1)
                          ├ (可选) MltBackend
                          └ (Phase 4) LibavInProcess / Metal
```

模块边界(Codex 按此建 crate/包):
- `timeline-core`:纯逻辑,无 IO。RationalTime、实体、**Op 枚举**(insert/move/trim/split/delete/ripple_delete/set_param/add_transition/add_subtitle…)、每个 Op 的 `apply` 与 `inverse`、不变量校验(同轨不重叠、时间非负、引用存在、源范围不越界)。**这是测试重心**,用属性测试覆盖 apply+inverse 往返。
- `store`:SQLite 读写、oplog、修订号、范围查询。
- `media`:`probe`(ffprobe → 流信息、时长、时间基、是否可变帧率、是否长 GOP)、指纹(路径+大小+mtime+头尾采样哈希;避免整文件哈希)、`MediaBackend` trait(`render_frame`, `render_range`, `extract_waveform`, `make_proxy`, `export`)。
- `render`:见 §6.2,输出与后端无关的 `RenderPlan`。
- `jobs`:带优先级的作业队列、并发上限、取消、进度、缓存键。
- `mcp-server`:仅做协议适配与参数校验,不含业务逻辑。

---

## 6. 大型工程的主要瓶颈与对策

| # | 瓶颈 | 为何成问题 | 对策 | 证据 |
|---|------|-----------|------|------|
| B1 | 整文档加载/保存 | 每次编辑 O(N);AI 一次对话会做数百次编辑 | SQLite + 增量事务(§4.2) | [I] |
| B2 | AI 上下文被时间线撑爆 | 10^5 个 clip 的 JSON 远超上下文 | 默认只返回摘要;所有列表接口强制 `range`/`track`/`limit`/`cursor`;结果可 `fields` 裁剪;返回计数而非全集 | [I] |
| B3 | 涟漪编辑/批量移动 | 绝对起点需整段平移 | 单条 SQL 批量更新;Phase 0 验证,不达标再优化 | [H] |
| B4 | 超大 filtergraph | 把所有 clip 放进一个 `filter_complex` 会导致句柄/内存/解析时间爆炸 | **分段计划**:按边界(任一 clip/转场/字幕起止)切时间片,每片只含活跃项,图规模 ∝ 活跃轨道数 | [I] 设计推断,Phase 0 验证 |
| B5 | 长 GOP 精确定位 | 单帧预览要从前一个关键帧解码 | 预览用 intra-frame 代理(如 ProRes Proxy/低码率全 I 帧);导出始终回源 | [I] |
| B6 | 打开解码器数量 | 百个源同时打开耗尽句柄/内存 | 分段后每段只开活跃源;作业并发上限;Phase 4 进程内解码器池带 LRU | [I] |
| B7 | 导入时探测上千媒体 | 串行 ffprobe 太慢 | 并行有限度探测 + 指纹缓存(同一文件不重复) | [I] |
| B8 | 重复渲染 | 小改动不应重渲全片 | 分段缓存键 = 哈希(该段涉及的 item/effect 参数 + 源指纹 + 渲染设置);只重渲命中失败的段 | [I] |
| B9 | 导出画质与时间 | 段间缓存中间件若有损会二次压缩 | 中间件用高码率/近无损(ProRes/高码率 H.264);未改动段可考虑流拷贝拼接(需在关键帧对齐)。**Phase 3 先做 spike 验证可行性,失败则退回按时间窗裁剪的单遍导出** | [H] |
| B10 | 关键帧动画 | 许多滤镜参数只支持表达式,不是任意曲线 | 先支持线性/保持两种插值并映射为 ffmpeg 表达式;更复杂曲线推迟到自定义效果阶段 | [I] |

### 6.1 增量更新语义
- 每次 `apply_edits` 产生新修订号 `rev`。读接口可带 `if_rev`;写接口必须带 `base_rev`,不一致则返回当前 rev 与冲突说明(乐观并发,防止 AI 基于过期视图编辑)。
- 写结果回显**受影响项**及受影响的时间范围,让 AI 无需再查即可核对。
- 脏区间 = 所有 Op 影响的 `[t0,t1)` 并集 → 使对应段缓存失效。

### 6.2 渲染计划
1. 输入:`(rev, [t0,t1), 渲染设置)`。
2. 切段:收集范围内所有项的边界点,排序去重。
3. 每段:查询活跃的 video/audio/subtitle 项及其 effect/transition,生成与后端无关的 `SegmentPlan`(层序、源范围、速度、变换、混合)。
4. 后端把 `SegmentPlan` 翻译为 ffmpeg 参数(输入 `-ss/-t`,`trim/setpts/scale/overlay/xfade/amix/afade/volume`,字幕走 libass 相关滤镜 [I] 取决于 ffmpeg 构建是否含 libass,**启动探测**)。
5. 段结果落盘到缓存目录并拼接。
- 预览/导出共用同一计划,仅渲染设置不同(分辨率、质量、是否用代理)。

---

## 7. MCP 接口设计

规范事实 [F]:
- 最新规范版本 2026-07-28;请求无状态、自包含,逐请求协商能力。
- 工具支持 `outputSchema`、`structuredContent`、`annotations`、`resource_link`,并有分页与缓存工具。
- 长任务由 **Tasks 扩展**(`io.modelcontextprotocol/tasks`)承载:服务器决定是否返回任务句柄;状态 `working/input_required/completed/failed/cancelled`;客户端必须声明支持,**服务器不得向未声明的客户端返回任务**;各宿主支持度不一。

设计原则:
1. 工具数控制在约 20 个(工具列表本身占 Agent 上下文)。
2. 所有工具返回 `structuredContent`,并声明 `outputSchema`;错误信息要可操作("clip c_17 与 c_22 在 V2 轨 [00:10:02:00, 00:10:03:12) 重叠")。
3. 无状态规范 vs 有状态引擎:项目状态在 SQLite,不在会话;工具带可选 `project`(缺省 = 当前默认项目)。
4. 标注只读/破坏性/幂等(annotations),但 [F] 规范说明这些注解对不可信服务器不可信——本地自有服务器场景无碍。

工具清单(草案):

| 类别 | 工具 |
|------|------|
| 项目 | `project_open/create`、`project_summary`(轨道数、总时长、item 数、rev、告警计数)、`get_capabilities` |
| 媒体 | `media_import`(批量)、`media_list`(分页)、`media_info` |
| 查询 | `timeline_query`(轨道+时间范围+类型+limit/cursor)、`item_get`(含相邻项)、`find_items`(按媒体/文本/标记/区间)、`marker_list` |
| 编辑 | `apply_edits`(Op 数组,原子,`base_rev`,`dry_run`)、`undo/redo`、`rev_diff(a,b)` |
| 验证 | `validate`(重叠、缺失媒体、越界、静音/黑场/音频削波检测)、`render_frame(t)`(返回图片)、`render_preview(range, scale)`(返回文件 `resource_link`)、`contact_sheet(range, n)`、`measure_audio(range)`(响度/峰值,用 ffmpeg 滤镜 [I]) |
| 输出 | `export`(返回 Task;不支持 Tasks 的客户端走 `job_get/job_cancel` 轮询)、`job_list` |
| 交换 | `import_otio/export_otio`、`import_srt/export_srt` |

`apply_edits` 是核心:一次提交多个 Op,先在内存事务内校验,**要么全部成功要么全不改**;`dry_run` 返回将发生的变化与告警而不落库,帮助 AI 在不确定时先预演。

---

## 8. 分阶段实施计划

**Phase 0 — 风险验证(优先,产出为实测报告,不写产品代码)**
1. 安装 ffmpeg,记录完整构建与最小 LGPL 构建体积;探测 libass、xfade、VideoToolbox 编解码可用性。
2. 合成工程:10^5 个 item / 50 轨 → 测 SQLite 范围查询、批量 ripple、单批 100 个 Op 的耗时与内存。
3. 长 GOP H.264 与 ProRes 代理上的 `render_frame` 延迟;VideoToolbox 解码吞吐。
4. 分段图导出:8 条同时活跃轨道、1000 个 clip,对比"全图单遍"与"分段+拼接";验证段间流拷贝拼接是否可行(B9)。
5. 测 MCP 工具列表(20 个工具)的 token 占用,以及典型 `timeline_query` 返回大小。
6. 补查 GES/MLT 打包体积,决定后端 B 是否值得保留。
退出标准(均为 [H],可调整):范围查询 p95 < 20 ms;100 Op 批量 < 100 ms;代理上单帧 < 1 s;分段导出内存不随 clip 总数增长。

**Phase 1 — 文档核心 + MCP 读写(无渲染)**
`timeline-core`、`store`、`mcp-server`;ffprobe 导入;全部查询/编辑/undo/validate 工具;oplog 往返属性测试;合成大工程回归测试。

**Phase 2 — 预览与验证**
`render_frame/render_preview/contact_sheet`;代理/缩略图/波形作业;基础转场(淡入淡出/xfade)、音频混音/淡入淡出、字幕(SRT/ASS)、变速、缩放/位移/不透明度。

**Phase 3 — 导出与规模化**
分段计划与缓存、`export` + Tasks 与轮询回退、OTIO/SRT 互通、崩溃恢复(作业重启后可续)。

**Phase 4 — 按需优化(只有在 Phase 0–3 实测显示必要时才做)**
进程内 libav + 解码器池、Metal 自定义效果/合成、更丰富的关键帧曲线、实时预览前端、Windows/Linux 构建与硬件加速(ffmpeg 的 VAAPI/CUDA/Vulkan 等,见 FFmpeg 硬件加速 wiki)。

---

## 9. 风险与未调查项

| 风险 | 影响 | 缓解 |
|------|------|------|
| 分段+拼接导出出现音画不同步、段边界瑕疵 | 导出质量 | Phase 0 spike;失败回退时间窗单遍导出 |
| LGPL 构建缺 libx264/libass 等 | 画质/字幕功能 | 启动能力探测;允许自带 ffmpeg;明确告知用户 |
| VideoToolbox 硬件编码画质/码控不如 x264 | 导出质量 | 实测;提供 ProRes/高码率选项 |
| 变速/可变帧率素材时间精度 | 剪辑点漂移 | 导入时检测 VFR,默认生成 CFR 代理并以代理时间轴做精确定位 |
| ffmpeg 版本差异导致滤镜行为不同 | 结果不可复现 | 锁定并记录版本,渲染缓存键含版本 |
| 关键帧/复杂特效超出 filtergraph 表达力 | 功能上限 | 推迟至 Phase 4;备选 MLT 或自定义 Metal 效果 |
| MCP 客户端不支持 Tasks 扩展 | 导出体验 | 轮询工具回退(已设计) |
| AI 误操作 | 数据损坏 | oplog 全量可撤销;写前 `dry_run`;`base_rev` 并发保护 |
| 过度工程 | 进度 | 每项"推迟"明确列出;性能结构(顺序键/树/进程内解码)仅在实测不达标时引入 |

**未调查(请勿当作"已排除")**:现有的视频剪辑类 MCP 服务器、GStreamer GES、OpenShot/libopenshot、Olive、Natron、Remotion、AVFoundation/Metal 细节、各 Rust/Go ffmpeg 绑定库的维护状况、MLT 的 GPL 模块清单与安装体积、SQLite 在本场景的实测上限。

---

## 10. 来源(均于 2026-10-14 抓取)

- MCP 规范总览(版本 2026-07-28、无状态、能力协商、扩展):https://modelcontextprotocol.io/specification/latest
- MCP 工具(outputSchema、structuredContent、annotations、resource_link):https://modelcontextprotocol.io/specification/2026-07-28/server/tools
- MCP Tasks 扩展(状态机、能力协商、轮询):https://modelcontextprotocol.io/extensions/tasks/overview
- MCP 官方 SDK 与分级(TS/Python/C#/Go/Rust/Ruby 为 Tier 1,Java Tier 2,Swift/PHP/Kotlin Tier 3):https://modelcontextprotocol.io/docs/sdk
- OpenTimelineIO README(交换格式、C++ 核心、opentime、Python 绑定、适配器):https://github.com/AcademySoftwareFoundation/OpenTimelineIO
- OTIO 序列化 schema(类清单):https://opentimelineio.readthedocs.io/en/latest/tutorials/otio-serialized-schema.html
- MLT README(LGPL、为视频剪辑设计):https://github.com/mltframework/mlt
- MLT XML 文档(producer/playlist/multitrack/tractor):https://www.mltframework.org/docs/mltxml/
- FFmpeg 硬件加速 wiki(VideoToolbox 编解码列表、回退行为、`-q:v` 限制):https://trac.ffmpeg.org/wiki/HWAccelIntro
- FFmpeg 法律页(LGPL 合规步骤、避免 libx264):https://ffmpeg.org/legal.html
