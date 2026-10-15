# Editing Tools — MLT 局部渲染验证

一个最小的渲染正确性原型：将时间线区间的依赖展开为 MLT 图，重放必要历史后输出 RGBA 与 PCM。没有 MCP、通用缓存或编辑器界面。

## 已验证的核心结论

- 同一构建内，新建图、保留绝对时间坐标并重放依赖区间，可使本测试集的局部输出与完整渲染逐帧、逐采样、逐时间戳一致。
- `tmix(5)` 需要前 4 帧；串联 `tmix(5)+tmix(3)` 需要 6 帧，少一帧会失败。
- SoX 反馈混响按无已知有限精确历史处理；必须新建图并从效果起点重放。对已使用的图 seek 回 0 仍不能恢复一致音频。
- clip 结束后的空白轨道不等于 PCM 零输入；本原型用显式静音片段推进尾音。
- 当前布局固定为六轨、320×180；不支持的布局/效果/作用域会被拒绝。它不是任意 XML 或任意工程的通用适配器。

完整机制、实验和限制见 [局部渲染报告](LOCAL_RENDER_REPORT.md)；早期规模与后端比较见 [Phase 0 报告](PHASE0_REPORT.md)。[evidence/macos](evidence/macos) 保存本机实测证据。Linux 执行路径已准备，尚未完成 Linux 正确性验证；不能将 Mac JSON 当成 Linux 成功。

## 测试时间线

6 秒，30 fps，48 kHz 双声道，包含 A/B 交叉溶解、恒定 1.5× 视频变速、透明叠加与不透明度动画、两个字幕 cue、根节点 tmix 和真实 SoX 混响。音频 clip 在第 3 秒结束，之后显式补 PCM 静音；另有无 padding 反例。

字幕是预栅格化字形的带 alpha 素材，并非原生 SRT/ASS 插件验证。未验证变速曲线、倒放、音频保调、自动无限尾音排空、全局分析滤镜或状态快照恢复。长 NTSC 的 MLT 浮点采样边界偏差会被适配器检测并拒绝。

## 在本地运行

依赖：Python 3、C 编译器、pkg-config、FFmpeg、MLT 7.42.0（core/avformat/xml/sox）及其开发头文件、libsox。Linux 和已有系统 MLT 的 macOS 可运行：

```sh
MLT_SYSTEM=1 python3 local_render/run_all.py
```

需配置正确的 `PKG_CONFIG_PATH` 和动态链接路径。两个字幕 PNG 已随源码保留，系统模式无需 AppKit。原实验机器可用保留在本机但未提交的 `phase0/runtime`，运行不带 `MLT_SYSTEM` 的同一命令；该路径会使用 Swift/AppKit 重建字幕。

测试将重建本目录专用合成媒体和结果。独立编译与独立测试并行，状态复用测试等待参考文件完成。磁盘少于 512 MiB 时拒绝启动；运行末尾自动清理固定命名的裸帧/PCM 临时文件。不要将真实媒体存放到这些专用名称下。

## 从源码准备 Linux MLT

`vendor/mlt-v7.42.0.tar.gz` 是 MLT 官方 v7.42.0 源码归档，许可证文件保留于归档内。它用于避免构建时访问 GitHub；本仓库不附带安装好的 MLT/FFmpeg 运行时。

Debian/Ubuntu 开发依赖：

```text
build-essential cmake pkg-config python3 ffmpeg
libavcodec-dev libavdevice-dev libavfilter-dev libavformat-dev libavutil-dev
libswscale-dev libswresample-dev libxml2-dev libsox-dev libsamplerate0-dev
```

在具有依赖安装权限的环境准备好这些包，然后运行：

```sh
bash local_render/cloud_setup.sh
```

脚本只编译 core/avformat/xml/sox，默认安装到 `/tmp/mlt-7.42.0`；不会安装系统包。`MLT_BUILD_JOBS` 控制编译并行度。此脚本也能在普通 Linux 本地机器使用，不要求云端服务。

## 单区间输出

```sh
MLT_SYSTEM=1 python3 local_render/adapter.py local_render/fixture.json 58 66 /tmp/mlt-range-58-66
```

区间是半开整数帧 `[58,66)`。输出裸 RGBA、48 kHz 双声道 f32 PCM、全局有理数时间戳、局部 XML 和依赖计划。默认 exact 策略使用新图；实验性的 cold/bounded/fixed 策略可能故意产生差异。

## 证据边界

本机 Mac 实验包括 118 次差分比较，64 项需要精确一致的断言通过；其余含预期不一致的负对照，不能写成“118 项全部一致”。6 种效果均有移除对照；实际 NUT/FFV1/PCM 容器也进行了无损往返与 PTS 检查。

有状态效果从起点重放的成本随历史增长。本次先证明正确性，没有证明任意大工程的实时预览性能，也没有证明与其他 NLE 的色彩/显影等价性。
