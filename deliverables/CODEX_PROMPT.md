请在当前实际的 Codex 远程/Linux 执行环境中继续这个项目。不要创建新聊天、重新迁移任务或访问我的 Mac。若没有远程 shell，明确说明，不能把聊天创建当成执行成功。

附件是完整源码交接包。先解压并进入包含 `local_render/` 的目录，读取 README_START_HERE.md、LOCAL_RENDER_REPORT.md，运行 `python3 verify_bundle.py`。本机证据在 `reference/macos/`，只作待复核对照，不代表云端已通过。

任务：完成最小 MLT 局部渲染适配器的云端正确性验证。时间线包含交叉转场、恒定变速、透明叠加、定时字幕、音频混响、时域滤镜。比较完整渲染与随机局部区间的原始帧、逐音频采样和时间戳；验证冷启动、有限预热、从作用域起点重放及脏图 seek 的差别。不要开发 MCP、通用缓存或产品 UI。

执行要求：
- 独立编译与独立测试并行；有依赖的阶段顺序执行，不能覆盖同一输出目录。先记录 OS、架构、MLT/FFmpeg/SoX 版本与能力。
- 优先固定 MLT 7.42.0。源码已包含在 vendor/mlt-v7.42.0.tar.gz，无需从 GitHub 下载依赖源码；OS 开发包清单在 README。安装当前云端环境允许的依赖，运行 `bash local_render/cloud_setup.sh`。Linux 路径使用 MLT_SYSTEM=1 和 pkg-config，不需要 Swift/AppKit，两个字幕 PNG 已附带。
- 有既有可用 MLT 时可设 MLT_SYSTEM=1 运行 `python3 local_render/run_all.py`；版本若非 7.42.0，必须明确区分该实验，不冒充同版本复验。网络、安装权限或依赖问题不能误判成渲染算法不可行。
- 全部效果要做激活/移除对照。tmix(5) 的 4 帧历史、tmix(5)+tmix(3) 的 6 帧历史及少一帧的突变均须检查。混响要比较新图重放与复用脏图；音频 clip 真正结束后，空白轨道与显式 PCM 静音 padding 必须做对照。
- 验证真实 NUT/FFV1/PCM 容器的解码输出与 PTS。用独立有理数检查 NTSC 音频边界，不能只与使用同一错误时钟的 full 比较。本机观察到 float fps 计算在帧 13454 首次偏一采样；云端须实际复核，不预设结果。
- 逐平台以内的 full/local 精确一致是主要判据，不要求 Mac/Linux 不同构建的原始哈希相同。保留实际失败和无法处理的效果类型；不要通过放宽阈值、删测试或伪造通过来完成任务。
- 生成 CLOUD_RENDER_REPORT.md，记录执行命令、版本、真实测试数量、精确/近似恢复条件、失败、未支持类型、与 Mac 对照的差异。默认清理可重建的 RGBA/PCM，保留轻量 JSON 与必要的无损样例。没有证明有限历史时，不要宣称任意固定预热足够。

GitHub 交付已获得用户明确授权：完成验证后，将源码、测试和报告提交并推送至 https://github.com/turtlelnc/Editing-Tools.git 。先检查远程内容及 AGENTS.md；可能为空仓库，按实际情况处理。在 `mlt-local-render-validation` 分支提交，不 force-push、不重写历史、不擅自覆盖已有文件。不要提交安装好的运行时、依赖构建目录、裸帧、PCM 或密钥；Mac 对照应保留独立标记。返回实际 commit SHA、分支与链接，检查对应 commit 的 CI（若无 workflow 则明确说明）。如缺少写权限，保留可审阅的代码/补丁并报告 push 未完成，不读取或打印密钥、不交互登录。

不要调用付费模型 API 或另外创建付费计算资源；使用当前 Codex 执行环境即可。请直接实现、运行和修复，不停留在方案。沟通简洁中文，区分已执行证据与推断。
