# 最小 MLT 局部渲染验证

结论与限制见 [报告](../LOCAL_RENDER_REPORT.md)。本目录是固定六轨布局的正确性原型，没有 MCP、通用缓存或任意效果自动规划器。

## 运行全部验证

需要 macOS、clang、Swift/AppKit、Python 3、FFmpeg，以及 Phase 0 的 MLT 7.42.0 runtime。另需 libsox 及其依赖；当前机器已经安装。测试会覆盖本目录专用 fixture、media、results，请不要在这些名称下存放真实素材。

```sh
python3 local_render/run_all.py
```

独立编译和互不依赖的测试并行执行；有依赖的阶段顺序执行。磁盘少于 512 MiB 时拒绝启动，完成后自动清理固定命名的裸帧/PCM 中间文件。不会触碰 Phase 0 历史文件。

检查 `results/SUITE.json`、`verification.json`、`session-probe.json`、`mux-verification.json`、`eos-tail.json`。64 项要求 bit-exact 的断言应通过，故意冷启动/少预热的对照应出现差异。

## 单独渲染区间

```sh
python3 local_render/adapter.py local_render/fixture.json 58 66 /tmp/mlt-range-58-66
```

得到：`video.rgba`（320×180）、`audio.f32le`（48kHz 双声道）、`timestamps.json`、`plan.json`、局部 XML 和日志。区间为半开整数帧 [58,66)。默认 exact 模式从效果依赖起点在全新图中重放，预热音视频但只输出目标区间。

Python API `render(timeline, start, end, directory, policy='exact')` 逐帧返回 bytes；实验用 cold/bounded/fixed 策略可能有意产生错误，不能当成 verified 输出。

- `tmix(N)`：根节点串行历史求和，每个贡献 N−1 帧。
- SoX 反馈混响：从作用域起点新建图重放，没有任意有限历史保证。
- 源依赖计划使用 profile 帧单位；没有强制裁断底层媒体 I/O。
- 尾音需要真实 PCM 静音输入。fixture 的原音频在帧 90 结束，显式 padding 到帧 180。
- 未登记效果、未验证作用域、变速曲线、倒放和已发现的 NTSC 音频时钟偏移区间会被拒绝。

原生字幕插件未验证：fixture 的文字由 AppKit 栅格化，通过 MLT alpha 合成。变速只验证恒定 1.5× 视频。无损 NUT 用于实际容器往返验证；MP4 仅是查看预览。
