# Editing-Tools

[![CI](https://github.com/turtlelnc/Editing-Tools/actions/workflows/ci.yml/badge.svg?branch=cpp-rewrite)](https://github.com/turtlelnc/Editing-Tools/actions/workflows/ci.yml)

让 AI 剪视频的 MCP Server + 自带 Agent。达芬奇的 MCP 需要 Studio 版；这个工具用 C++ + FFmpeg 实现，免费、单个二进制、跨平台思路（目前在 macOS 上开发和验证）。

两种用法：把 36 个剪辑工具接到 Claude Code 这类现成 Agent 上（MCP 或 CLI），或者直接用仓库里自带的 `editing-agent`——一句话任务，它自己调用工具、自己看预览图、自己改到满意。

AI 只操作一个 JSON 时间线模型，不碰 FFmpeg 命令：每次修改都会校验，错误会以可读文字返回，方便 AI 自己纠正。可以随时 `preview_frame` 把某一帧作为图片取回来"看"效果。

## 能做什么

| 类别 | 工具 |
|---|---|
| 项目 | `create_project` `open_project` `get_timeline` |
| 素材 | `probe_media` |
| 剪辑 | `add_clip` `trim_clip` `move_clip` `delete_clip` `set_speed`（0.25–4 倍速） |
| 粗剪/分析 | `detect_scenes`（镜头切换）`detect_silence`（静音区间）`auto_rough_cut`（静音/镜头直接变成片段，可顺手建项目） |
| 配乐/音频 | `add_music` `update_music` `delete_music` `set_audio`（音量/淡入淡出/降噪）`auto_duck`（人声时自动压低配乐）`normalize_loudness`（响度标准化） |
| 字幕 | `set_subtitles` `update_subtitle` `delete_subtitle` `style_subtitles` `import_srt` `export_srt` `transcribe`（语音转文字）`auto_subtitles`（转写并自动上字幕） |
| 调色 | `color_adjust` `color_curves` `apply_lut` `apply_style`（一键风格）`reset_color`（可作用于整个项目或单个片段） |
| 预览/输出 | `preview_frame`（单帧）`preview_frames`（联系表）`preview_compare`（调色前后对比）`render_preview` `render_export`（带进度、可 Ctrl-C 取消） |

## 构建与安装

需要：CMake ≥ 3.24、支持 C++20 的编译器、本机装有 `ffmpeg` 和 `ffprobe`。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.editing-tools      # 装 editing-tools 和 editing-agent 到 ~/.editing-tools/bin/
ctest --test-dir build --output-on-failure
```

`editing-agent` 额外依赖系统 `curl`（发模型请求用；请求头走 0600 临时文件，密钥不进 argv）。MCP/CLI 本身不需要 curl。

## 字幕需要带 libass 的 ffmpeg

字幕通过 libass 烧录进画面。Homebrew 的 ffmpeg 默认不带 libass（没有 `ass` 滤镜）。需要一个带 `--enable-libass` 的 ffmpeg，然后用环境变量指过去：

```sh
export EDITING_TOOLS_FFMPEG=/path/to/ffmpeg-with-libass
```

没设置或 ffmpeg 不支持时，渲染带字幕的项目会返回明确的报错；不用字幕的功能不受影响。

macOS 上 libass 读不了 PingFang（系统私有字体），默认字体用 `Hiragino Sans GB`，也可以用 `style_subtitles` 指定其他已安装字体。

## 在 Claude Code 中使用

```sh
claude mcp add --scope user editing-tools \
  -e EDITING_TOOLS_FFMPEG=/path/to/ffmpeg-with-libass \
  -- ~/.editing-tools/bin/editing-tools
```

然后直接对 Claude 说：「把 a.mp4 的 0.5 到 5.5 秒和 b.mp4 拼起来，加上配乐并在说话时压低，加字幕，调成偏暖的色调，导出到桌面」。

## 命令行（给 AI 直接跑）

同一个二进制也是命令行工具，36 个工具都能直接调用，不需要常驻进程，也不用把全部工具的 schema 塞进上下文。项目状态存在一个 JSON 文件里，每条命令读取并写回：

```sh
et=~/.editing-tools/bin/editing-tools
$et tools                         # 列出所有工具
$et help add_clip                 # 某个工具的参数

$et --project p.json create_project --width 1920 --height 1080
$et --project p.json add_clip --path a.mp4 --in 0.5 --out 5
$et --project p.json color_adjust --warmth 0.3 --contrast 1.1
$et --project p.json set_subtitles --cues '[{"start":0.5,"end":3,"text":"你好"}]'
$et --project p.json preview_frame --time 1.0     # 图片存成文件，返回 image_path
$et --project p.json render_export --output_path out.mp4
```

- 参数写成 `--名字 值`，或者一个 JSON：`add_clip '{"path":"a.mp4"}'`；也可以 `--args-file 文件`（`-` 读标准输入）。类型按工具的 schema 转换：字符串参数永远是字符串，布尔参数可以只写 `--box`。
- 也可以用环境变量 `EDITING_TOOLS_PROJECT` 代替 `--project`。
- 结果是 stdout 上的一行 JSON（`--pretty` 缩进）。失败时 stderr 输出 `{"error": "..."}`，退出码 1 表示工具拒绝了这次操作，2 表示命令写错了。
- 不带参数且标准输入是管道时，行为和以前一样（MCP 服务端），所以已有的 MCP 配置不受影响；`editing-tools mcp` 可显式指定。

## 自带 Agent：editing-agent

同一套工具代码（进程内直接调用，不起服务、不把 36 个 schema 塞进外部模型上下文），加上一个自己的 Agent 循环：调模型 → 执行它要的工具 → 把结果（包括预览图）喂回 → 直到它给出不带工具调用的回答。

长任务的可观测性：`render_export` 会把 ffmpeg 的进度（百分比、已渲染时间、速度）实时报到 stderr / JSON 事件里，Ctrl-C 会真的杀掉正在渲染的 ffmpeg（而不是等它跑完）。工具连续失败、或 token 超预算时，循环会停止调用工具并让模型收尾，而不是继续烧钱。

看画面也更省：`preview_frames` 一次把多帧拼成一张联系表，`preview_compare` 把调色前后并排放；评审用的也是联系表，一张图代替六张。

### 三种模型接入

| `--provider` | 怎么用 | 说明 |
|---|---|---|
| `openai` | `OPENAI_API_KEY=…` 或 `--api-key` | 任何 OpenAI 兼容的 `/chat/completions`：OpenAI、DeepSeek、本地 llama.cpp / Ollama、Qwen-VL。**必须显式 `--model`**。 |
| `anthropic` | `ANTHROPIC_API_KEY=…` 或 `--api-key` | Claude Messages API，默认模型 `claude-sonnet-5-5`。 |
| `codex` | 先跑一次 `editing-agent login` | 用 ChatGPT/Codex 订阅登录（设备码），凭证存 `~/.editing-tools/auth.json`，不碰 `~/.codex`。ChatGPT 后端对部分地区返回 `403 Country not supported`；用 `--proxy` 或 `HTTPS_PROXY` 换线路。如果已经在用 Codex.cpp，也可以把 `~/.codex-cpp/auth.json` 复制过来（格式相同），只是两边各自刷新 token 会互相失效，长期建议各自 `login` 一次。 |

要选**能看图**的模型：`preview_frame` 的效果靠模型自己看图判断，纯文字模型只能做不看图的部分。`--base-url` 可以指到本地服务。

### 用法

```sh
et=~/.editing-tools/bin/editing-agent
$et login                                              # 仅 --provider codex 需要
$et --project ~/Movies/p.json "把 a.mp4 的 0.5–5.5 秒和 b.mp4 拼起来，调成偏暖，加字幕，导出到 out.mp4"
$et chat --project ~/Movies/p.json                    # 逐行对话；/exit 离开，Ctrl-C 打断当前这一步
$et sessions                                          # 看历史会话
$et --resume last "继续，把配乐再压低一点"              # 恢复上一次会话（工作区和项目文件一起恢复）
```

常用参数：

- `--workspace DIR`：它只能读写这个目录（默认当前目录）。
- `--allow DIR`：额外允许的目录，可重复，比如外置硬盘上的素材/LUT 目录。
- `--no-export`：只看不做，禁止 `render_export`。
- `--overwrite`：允许覆盖已存在的输出文件（默认拒绝）。
- `--max-steps N`：一轮最多调几次模型（默认 40），到上限会再问一次模型要个收尾。
- `--max-tokens N`：一轮的 token 软预算（输入+输出），超了会停止调工具、直接要收尾；0 表示不限制。
- `--self-review`：导出前跑一次独立评审，发现问题就拒绝导出并把问题交回模型修。
- `--notes FILE`：项目记忆，内容会附在系统提示里。
- `--json`：stdout 输出 JSONL 事件（`start` / `text` / `tool` / `tool_result` / `final` / `error`），给程序调用。
- `--quiet`：不要 stderr 上的进度行。

默认情况下 assistant 的文字直接流到 stdout，进度（调了哪个工具、成功没有）走 stderr，所以 `editing-agent … > result.txt` 只拿到回答。

### 安全边界

- Agent **没有 shell**，只有剪辑工具加上只读的 `list_dir` / `find_media`。
- 所有 `path` / `output_path` / `project_path` 参数必须落在 `--workspace` 或 `--allow` 目录内；越界会被拒绝，并把可用的目录告诉模型。
- 已存在的输出文件默认不覆盖；chat 模式渲染成片前会问一次（`-y` 跳过）。
- 密钥不写日志、不进 argv：请求头通过 0600 临时文件交给 curl。会话里只有对话和工具结果。
- 会话是 JSONL，存在 `~/.editing-tools/sessions/<id>/`，图片单独存文件不内联。

### 长会话

每次工具调用和模型回答都会追加到会话文件。上下文太长时，旧轮次会被模型总结成一段摘要；返回整个时间线的工具结果会被压成紧凑摘要（片段 id、起止、非默认的调色/音频）；旧预览图只保留最近两张。`--resume ID|last` 从上次的地方继续。

### 独立评审

`review_edit` 工具会起一次**没有工具**的独立请求：从时间线上抽几帧，连同时间线 JSON 让模型只报告有证据的问题（调色前后不一致、字幕挡住脸、剪辑点突兀），每条带严重程度、时间点和修法。每轮最多两次，避免它用评审代替干活。

### 验证状态

- **114 个测试**（2 个字幕测试需要带 libass 的 ffmpeg，本机跳过；Ubuntu CI 上会跑），覆盖：三种 provider 的请求格式/SSE 解析、工具循环、路径权限、会话保存恢复、上下文压缩、预算与失败熔断、自检闸门、联系表/对比图、静音与镜头检测、粗剪、变速、风格预设、CLI 参数与退出码。
- 所有模型请求都打在本地模拟 HTTP 服务端上（`tests/mock_http.h`），其中一条完整端到端真的渲染出 mp4 并用 `ffprobe` 校验。
- **GitHub Actions**：Ubuntu + macOS 构建并跑全部测试；另有一个 ASan+UBSan 任务。本机也已经用 ASan+UBSan 跑过全部测试（无报错）。
- **真实联调**（ChatGPT/Codex 订阅 + 代理）跑过三条完整任务：暖色导出、冷色 + 评审、以及「静音粗剪 → warm_cinematic → 1.2 倍速 → 联系表检查 → 导出」；最后一条成片 3.50s，与项目时长一致，日志里能看到实时渲染进度。
- **语音转写也真机验证过**：本机编译 whisper.cpp（仅 whisper-cli）、用 `ggml-small-q5_1` 模型、macOS `say` 合成中文语音，真实跑通 `transcribe → auto_subtitles → export_srt`；7.8 秒音频转写约 2.9 秒，两条字幕时间与音频分段一致。小模型会把个别词听错（例如把"字幕"听成"自动"），这是模型能力问题，不是管线问题——建议中文用 small 及以上的量化模型。
- 没有 OpenAI / Anthropic 的 API Key，这两家只用模拟服务端验证过请求格式；真实联调需要你跑一条冒烟：
  `OPENAI_API_KEY=… editing-agent --provider openai --model <看图模型> --project p.json "把 a.mp4 前 5 秒调暖并加字幕，导出 out.mp4"`

## 说明与限制

- **时间**：字幕的时间是剪辑之后的输出时间线上的秒数。
- **LUT**：支持 `.cube` `.3dl` `.dat` `.m3d` `.csp`，可放在外置硬盘上（路径带空格/中文也行），渲染时硬盘需已连接。LUT 文件不会被复制进项目。
- **配乐**不会自动循环或裁剪到视频长度；比视频长的部分被截掉，短了就提前结束。
- 响度标准化是单遍处理，对很短的片段不够精确。
- 目前只有一条主视频轨（片段首尾相接），没有转场、画中画和多视频轨。
- 没有自动语音转写：字幕文本需要由 AI 或 SRT 文件提供。计划接入 whisper.cpp。
- `editing-agent` 目前是一次性任务 + 逐行对话，没有 TUI；Codex（ChatGPT 订阅）后端协议来自 Codex.cpp 的移植，可能与线上服务有出入，属于已知风险。
- `set_speed` 会改变片段在时间线上的长度，后面所有片段和字幕/预览时间都会跟着移动；变速后要重新检查时间。
- 联系表/对比图是把多帧渲成一张 JPEG，单帧像素比单独 `preview_frame` 小，细节判断仍以单帧为准。

## 代码结构

```
src/mcp/        MCP 协议（stdio 上的 JSON-RPC）
src/model/      时间线数据模型 + 校验 + JSON 序列化
src/compiler/   时间线 -> ffmpeg filter_complex
src/render/     运行 ffmpeg；临时字幕/LUT 文件；单帧预览
src/subtitles/  ASS 生成、SRT 读写
src/util/       base64、UTF-8 安全序列化、进度/取消钩子
src/tools/      36 个工具（MCP、CLI 和 Agent 共用）
src/cli/        命令行前端：每个工具一个子命令，项目状态存在 JSON 文件里
src/agent/      自带 Agent：中立消息模型、curl/SSE HTTP、三种 provider、
                权限层、工具箱、Agent 循环、系统提示、JSONL 会话、CLI
tests/          Catch2 测试（含真实渲染后量颜色/音量/响度，以及模拟模型驱动剪辑）
```
