# Phase 0 验证程序

结果解释以 `../PHASE0_REPORT.md` 为准。所有输出保存在本目录 `results/`；专用合成媒体位于 `media/`。脚本会覆盖自己的合成输出，不要将真实工程放到这些名称下。

## 复现本机已完成的测试

```sh
python3 phase0/storage_bench.py
python3 phase0/chunk_bench.py
python3 phase0/wal_control.py
python3 phase0/media_bench.py
swiftc phase0/native_probe.swift -o phase0/native_probe
phase0/native_probe
swiftc -parse-as-library phase0/native_decode.swift -o phase0/native_decode
phase0/native_decode phase0/media/raw-first2.mov phase0/results/raw-preview.png
```

原生 RAW 样本是两帧完整压缩包，不是合成普通 ProRes；来源与哈希见报告。`native_hardware.swift` 是未成功的压缩 sample 诊断，不能作为已通过验收的路径。

## 后端运行环境

已重定位的 Homebrew bottles 位于 `runtime/`，当前路径有绝对依赖；搬动目录后需运行 `relocate.py` 并重新编译 probe。新机器需自行获取对应版本/平台的 MLT/GStreamer bottles，所需依赖按 loader 报错安装；不要将本次部分依赖环境当作完整分发包。

```sh
python3 phase0/relocate.py
clang -I phase0/runtime/mlt/7.42.0/include/mlt-7 phase0/mlt_probe.c phase0/runtime/mlt/7.42.0/lib/libmlt-7.7.42.0.dylib -o phase0/mlt_probe
python3 phase0/backend_bench.py
python3 phase0/ges_bench.py
python3 phase0/ges_isolated.py
python3 phase0/verify_outputs.py
```

对比计时请顺序运行、确认没有遗留后端进程，并记录磁盘/内存压力。`ges_isolated.py` 允许 180 秒，MLT 十万条目测试允许 120 秒。超时是筛查结果，不代表所有后端配置不可行。

补索引的 50 轨存储控制需单独运行 `storage_bench.bench(100000,50)`，并将结果保存；原始完整循环的随机种子跨数据集继续，不能直接与分块单数据集实验做配对比较。报告使用了单独控制，与分块实验相同的 seed 与随机抽样。
