# 从源码构建

先在仓库根目录运行 `python3 -B build/scripts/verify_source_manifest.py`。使用短 ASCII 源码路径，构建目录必须全新且位于源码树外。构建入口只生成本地输出。

## 主机与 ARM 应用

Linux/WSL 主机须有 CMake 3.16 以上、C++17 编译器、pkg-config、Qt5 Core/Gui/Widgets 开发组件、Python 3.10+ 与 NumPy。完整主机测试还需要 `xvfb-run`、`Xvfb` 和 `xauth`。主工作台 CMake 会在缺少 Qt5 时跳过，因此主机入口包含依赖预检和最终目标检查。

```bash
bash build/scripts/build-host.sh /tmp/zynq_host_new
bash build/scripts/build-arm.sh /tmp/zynq_arm_new /path/to/buildroot_sdk/host
```

入口从 `src/workspace/zynq_vibration_workbench_v7/` 构建。主机入口使用同一 Python3 解释器构建并运行 CTest，配置 `WORKBENCH_TEST_PLATFORM=xcb`，在 `QT_QPA_PLATFORM=xcb` 与 1920×1080×24 的 Xvfb 屏幕内运行；原生坐标、拖放和布局探针依赖此环境，不能用 offscreen 运行结果替代。ARM 入口交叉构建应用，检查工作台目标，再用 `file` 列出可执行文件身份。ARM 需要与目标 Linux/rootfs 一致的 Buildroot hard-float SDK 和 Qt5；仓库不携带工具链二进制。交叉构建后还须核验 ELF 架构、解释器、共享库与目标 rootfs 匹配。主机测试不替代 ARM、触屏或机械验证。

## FPGA

使用 Vivado 2025.2，器件 `xc7z020clg400-2`。从有效平台源码重建 XFFT 9.1 和平台：

```powershell
pwsh -File build/scripts/build-pl.ps1 -OutputDirectory E:/zynq_pl_new -VivadoBin D:/tools/Vivado/2025.2/bin
```

路径为示例，须替换工具位置。入口将 `src/fpga-effective/source/` 复制到包外构建目录，调用 `src/workspace/zynq_vibration_workbench_v7/hardware/analysis_v6/scripts/generate_fft.tcl` 生成 XFFT，随后创建并实现含 SG 的组合平台。必要系数和第三方 VHDL 保留在源码内；IP 由使用者本机 Vivado 生成，源码仓库不分发 DCP/XSA/网表/bitstream。须审核资源、setup/hold、CDC 和 DRC，并保留警告及其处理依据。

## 数值与 RTL 仿真

`sim/` 保存公开仿真证据和来源映射，`data/` 提供明确标注的合成输入；这些证据的生成时间与来源以各目录说明和清单为准。

隔离 FIR/结果总线回归的现有源码入口如下。需要 Python3、NumPy 和 Vivado 2025.2，显式指定全新的包外工作目录：

```powershell
pwsh -File src/workspace/zynq_vibration_workbench_v7/hardware/analysis_v6/tests/run_isolated_units.ps1 -WorkDirectory E:/zynq_units_new -VivadoBin D:/tools/Vivado/2025.2/bin
```

`-BusOnly` 仅核验结果读口；默认模式还核验 28 例 FIR 输入。该测试使用 FFT stub，不能作为实际 FFT 数值验收。真实 XFFT 仿真的准备入口为 `hardware/analysis_v6/scripts/prepare_replay_batch.tcl`，需要先生成 FFT IP 和 `tests/make_vectors.py` 的 NumPy 输入。运行完成后应核验全部四档 PASS 与独立 C-model 对照；现有 `tests/compare_fft_model.py` 的 Windows C-model ZIP 路径固定为 `D:/vivado/2025.2/Vivado/...`，换机前须审核并适配实际安装位置。仅仿真器退出不表示数值验收通过。

## Linux 全系统重建边界

仓库包含驱动、Buildroot external 和 Qt 插件等 Linux 移植输入，当前可直接使用的是上面的 V7 应用交叉构建入口。历史 Linux 脚本仍有以下接入边界：

- `src/workspace/zynq_vibration/linux/scripts/inject-display.py` 按 `display-platform.lock.json` 校验 9 份历史硬件导出文件；公开源码未携带这些 XSA、bitstream 及其导出伴随文件。不能删除断言或填入假哈希。应从新平台生成所需导出，审核地址映射、PS 初始化和内核配置后建立对应锁表。
- 两个历史 `vibration-offline.mk` 的 local-site 指向各自旧项目根，并只收集 `CMakeLists.txt` 与 `software/**`。公开整理保留的旧项目目录没有这两项；当前应用位于独立的 `zynq_vibration_workbench_v7`。完成全系统集成前，须审核重定向 recipe，并将 V7 所需的 `hardware/combined/include` 和 `hardware/analysis_v6/data/analysis_coefficients.hpp` 等依赖纳入源清单及 legal-info 归档。仅复制旧 recipe 不能构建当前应用。
- SG 内核脚本还依赖明确的历史基础内核和 SDK 输出目录。须先建立匹配当前平台、驱动、设备树、rootfs 和存储布局的构建输入，再验证完整启动链。

尚未完成全新机器上的全系统重建。这些边界不影响独立应用与 PL 源码入口的用途。`vibration-offline.mk` 的应用许可为 MIT；`REDISTRIBUTE=NO` 关闭范围过宽的 Buildroot 自动归档，项目源码的再分发权限见[许可说明](../license_status.md)。

## 串口 PC

Python 源码位于 `src/workspace/zynq_vibration_workbench_v7/software/serial_pc/`，运行依赖 PySide6 与 pyserial。历史封装的版本 pin 尚未验证为当前版本的独立 lock，仓库提供 Python 源码。运行入口和导出协议按该目录源码核对；发布 PC 可执行程序前须建立实际环境 lock 和依赖许可清单。
