# Editing-Tools

让 AI 剪视频的 MCP Server + 自带 Agent。达芬奇的 MCP 需要 Studio 版；这个工具用 C++ + FFmpeg 实现，免费、单个二进制、跨平台思路（目前在 macOS 上开发和验证）。

两种用法：把 27 个剪辑工具接到 Claude Code 这类现成 Agent 上（MCP 或 CLI），或者直接用仓库里自带的 `editing-agent`——一句话任务，它自己调用工具、自己看预览图、自己改到满意。

AI 只操作一个 JSON 时间线模型，不碰 FFmpeg 命令：每次修改都会校验，错误会以可读文字返回，方便 AI 自己纠正。可以随时 `preview_frame` 把某一帧作为图片取回来"看"效果。

## 能做什么

| 类别 | 工具 |
|---|---|
| 项目 | `create_project` `open_project` `get_timeline` |
| 素材 | `probe_media` |
| 剪辑 | `add_clip` `trim_clip` `move_clip` `delete_clip` |
| 配乐/音频 | `add_music` `update_music` `delete_music` `set_audio`（音量/淡入淡出/降噪）`auto_duck`（人声时自动压低配乐）`normalize_loudness`（响度标准化） |
| 字幕 | `set_subtitles` `update_subtitle` `delete_subtitle` `style_subtitles` `import_srt` `export_srt` |
| 调色 | `color_adjust` `color_curves` `apply_lut` `reset_color`（可作用于整个项目或单个片段） |
| 预览/输出 | `preview_frame` `render_preview` `render_export` |

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

同一个二进制也是命令行工具，27 个工具都能直接调用，不需要常驻进程，也不用把全部工具的 schema 塞进上下文。项目状态存在一个 JSON 文件里，每条命令读取并写回：

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

同一套工具代码（进程内直接调用，不起服务、不把 27 个 schema 塞进外部模型上下文），加上一个自己的 Agent 循环：调模型 → 执行它要的工具 → 把结果（包括预览图）喂回 → 直到它给出不带工具调用的回答。

### 三种模型接入

| `--provider` | 怎么用 | 说明 |
|---|---|---|
| `openai` | `OPENAI_API_KEY=…` 或 `--api-key` | 任何 OpenAI 兼容的 `/chat/completions`：OpenAI、DeepSeek、本地 llama.cpp / Ollama、Qwen-VL。**必须显式 `--model`**。 |
| `anthropic` | `ANTHROPIC_API_KEY=…` 或 `--api-key` | Claude Messages API，默认模型 `claude-sonnet-5-5`。 |
| `codex` | 先跑一次 `editing-agent login` | 用 ChatGPT/Codex 订阅登录（设备码），凭证存 `~/.editing-tools/auth.json`，不碰 `~/.codex`。ChatGPT 后端对部分地区返回 `403 Country not supported`；需要可访问的线路，curl 会自动使用 `https_proxy` / `HTTPS_PROXY` 环境变量。 |

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

- 一共 77 个测试，其中 agent 相关 23 个；模型请求全部打到本地模拟 HTTP 服务端（`tests/mock_http.h`），覆盖：三种 provider 的请求格式和 SSE 解析、工具循环、路径权限、覆盖保护、会话保存/恢复、上下文压缩、评审、CLI 参数和退出码。
- 其中一条是完整端到端：脚本模型依次调用 `create_project` → `add_clip` → `color_adjust` → `render_export`，真的渲染出一个 mp4，再用 `ffprobe` 校验时长和保存下来的时间线。
- 没有真实 API Key，所以真实联调需要你跑一条冒烟：
  `OPENAI_API_KEY=… editing-agent --provider openai --model <看图模型> --project p.json "把 a.mp4 前 5 秒调暖并加字幕，导出 out.mp4"`
- `login` 的设备码流程已经用模拟授权服务端验证；真实 `auth.openai.com` 在本机所在地区返回 403（见上表），需要换线路才能真连。

## 演示脚本

`examples/demo.py` 像 AI 客户端一样通过 MCP 协议走完整条流程（造几秒的小素材 → 剪辑 → 配乐/闪避/响度 → 字幕 → 调色+LUT → 预览 → 导出），最后用 ffprobe 校验结果：

```sh
python3 examples/demo.py --out /tmp/et_demo
```

## 说明与限制

- **时间**：字幕的时间是剪辑之后的输出时间线上的秒数。
- **LUT**：支持 `.cube` `.3dl` `.dat` `.m3d` `.csp`，可放在外置硬盘上（路径带空格/中文也行），渲染时硬盘需已连接。LUT 文件不会被复制进项目。
- **配乐**不会自动循环或裁剪到视频长度；比视频长的部分被截掉，短了就提前结束。
- 响度标准化是单遍处理，对很短的片段不够精确。
- 目前只有一条主视频轨（片段首尾相接），没有转场、画中画和多视频轨。
- 没有自动语音转写：字幕文本需要由 AI 或 SRT 文件提供。计划接入 whisper.cpp。
- `editing-agent` 目前是一次性任务 + 逐行对话，没有 TUI；Codex（ChatGPT 订阅）后端协议来自 Codex.cpp 的移植，可能与线上服务有出入，属于已知风险。

## 代码结构

```
src/mcp/        MCP 协议（stdio 上的 JSON-RPC）
src/model/      时间线数据模型 + 校验 + JSON 序列化
src/compiler/   时间线 -> ffmpeg filter_complex
src/render/     运行 ffmpeg；临时字幕/LUT 文件；单帧预览
src/subtitles/  ASS 生成、SRT 读写
src/tools/      27 个工具（MCP、CLI 和 Agent 共用）
src/cli/        命令行前端：每个工具一个子命令，项目状态存在 JSON 文件里
src/agent/      自带 Agent：中立消息模型、curl/SSE HTTP、三种 provider、
                权限层、工具箱、Agent 循环、系统提示、JSONL 会话、CLI
tests/          Catch2 测试（含真实渲染后量颜色/音量/响度，以及模拟模型驱动剪辑）
```
