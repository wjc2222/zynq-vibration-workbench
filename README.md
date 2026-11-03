# Zynq 三轴振动采集与故障检测

源码仓库：[https://github.com/wjc2222/zynq-vibration-workbench](https://github.com/wjc2222/zynq-vibration-workbench)

基于 AX7Z020 / XC7Z020 的现场振动工作台：采集三路电压与转速、保存 SD 原码、查看曲线、人工标注四种工况，在 ARM 上训练与运行四分类模型。PL 执行采集封包、FIR 抽取、加窗和 FFT；PS 完成原始数据管理、特征组织、训练推理和界面。

2026 AMD FPGA 自主选题赛道，**初级组**。项目原创代码和文档采用 [MIT 许可证](LICENSE)；Linux 驱动、Qt 配套和 rgb2dvi 等组件保留各自许可，见 [许可说明](license_status.md) 和 [第三方来源](third_party_notices.md)。

## 功能

| 部分 | 已有工程功能 |
|---|---|
| 采集 | 5/10/25/50 kHz，三通道与测速，定时或手动停止，分段原码落盘 |
| 显示 | 单绘图区切换时域、频谱、加速度，通道叠加，固定纵轴量程，拖动与缩放 |
| 标注与训练 | 正常、不平衡、不对中、松动；完整记录人工标签，现场 ARM 训练并保存模型 |
| 识别 | 提取69维，生产模型选42维；32树、深度3的原生C++随机森林，不用RPM选择参考或拒绝分类 |
| PL处理 | 对称FIR、抽取至5 kHz、8192点共享FFT，采用PS经AXI-Lite送料的分析支路 |
| Skill | 面向其他FPGA/SoC工程的工作流、六类模板、五个CLI与跨任务验证，见 [skill/README.md](skill/README.md) |

PL案例记录了对称FIR预加、三轴并行、双口BRAM、共享FFT及PS送料开销的取舍，并给出DSP/LUT/FF/BRAM占用、实际Vivado报告和分层A/B结果，见 [PL资源利用与优化案例](skill/examples/project_case/README.md)。新工程可复用方法，需替换自己的数据与硬件参数并重新验证。

## 快速开始

主机环境需要 Linux/WSL、CMake 3.16+、C++17、**Qt5 Core/Widgets**、Python3及X11/Xvfb；Qt6-only不能替代主工作台。构建不接触开发板，输出应放在源码树外且尚不存在。

```bash
python3 -B build-entrypoints/verify_source_manifest.py
bash build-entrypoints/build-host.sh /tmp/zynq_public_host_build
```

构建前会检查 Qt5，构建后会检查主工作台实际生成。ARM 和 PL 入口、依赖与 Linux 全系统重建条件见 [构建说明](docs/build.md)，已完成的检查及验证边界见 [验证状态](docs/validation.md)。

## 项目布局

- `workspace/zynq_vibration_workbench_v7/`：C++/Qt工作台、Python串口PC、测试及固定点参数。
- `fpga-effective/source/`：实际参与当前组合工程的RTL、约束、Tcl与rgb2dvi来源。
- `workspace/zynq_vibration*/` 中的 Linux/驱动部分：依赖当前相对目录，保留结构便于后续移植。
- `build-entrypoints/`：主机、ARM、PL构建与清单检查；不会自动刷机。
- `docs/`：架构、操作、复现、验证与源码发布说明。
- `licenses/` 与原文件头：第三方和派生组件的许可文本。

本次源码发布提供工程代码、构建脚本、测试和文档。原采集工程整包、厂商手册、个人日志、真实机械BIN、已训练模型、SDK、BOOT/rootfs/bitstream和已编译PC程序不在分发范围。Linux 全系统重建与 PC 依赖锁等后续工作见 [发布与复现检查](docs/release_checklist.md)。
