# Editing-Tools

让 AI 剪视频的 MCP Server。达芬奇的 MCP 需要 Studio 版；这个工具用 C++ + FFmpeg 实现，免费、单个二进制、跨平台思路（目前在 macOS 上开发和验证）。

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
cmake --install build --prefix ~/.editing-tools      # 装到 ~/.editing-tools/bin/editing-tools
ctest --test-dir build --output-on-failure
```

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

## 代码结构

```
src/mcp/        MCP 协议（stdio 上的 JSON-RPC）
src/model/      时间线数据模型 + 校验 + JSON 序列化
src/compiler/   时间线 -> ffmpeg filter_complex
src/render/     运行 ffmpeg；临时字幕/LUT 文件；单帧预览
src/subtitles/  ASS 生成、SRT 读写
src/tools/      各 MCP 工具
tests/          Catch2 测试（含真实渲染后量颜色/音量/响度）
```
