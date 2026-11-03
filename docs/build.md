# 从源码构建

先运行根README中的清单校验。使用短ASCII源码路径，构建目录必须全新且位于源码树外。所有构建入口只生成本地输出，没有自动烧写动作。

## 主机与ARM

Linux/WSL主机须有CMake、C++17、Qt5开发组件和Python3。界面探针用X11/Xvfb；Python模型工具另需NumPy。主工作台CMake会在缺少Qt5时跳过，所以构建入口包含显式前置检查和最终目标检查。

```bash
bash build-entrypoints/build-host.sh /tmp/zynq_host_new
bash build-entrypoints/build-arm.sh /tmp/zynq_arm_new /path/to/buildroot_sdk/host
```

ARM需要与目标Linux/rootfs一致的Buildroot hard-float SDK及Qt5；源码仓库不提供工具链二进制。交叉构建后核ELF架构、解释器、共享库与目标rootfs匹配。不要把主机测试结果当作ARM、触屏或机械验证。

## FPGA

Vivado 2025.2，器件`xc7z020clg400-2`。从effective源码重建XFFT9.1与平台：

```powershell
pwsh -File build-entrypoints/build-pl.ps1 -OutputDirectory E:/zynq_pl_new -VivadoBin D:/tools/Vivado/2025.2/bin
```

路径为示例，自行替换工具位置。此入口保留必要系数与第三方VHDL，用本机Vivado生成IP，源码仓库不分发DCP/XSA/网表/bitstream。检查资源、setup/hold、CDC和DRC，保留警告及其处理依据。

## Linux全系统重建边界

驱动、Buildroot external和Qt插件源码已整理，但原`inject-display.py`依赖的9份历史硬件锁定文件未随源码包提供。不能删掉哈希断言或补假哈希让脚本通过。应在后续构建中从新平台生成并审核锁文件，同时核SDK、内核配置、设备树与存储布局，再验证完整启动链。

当前材料提供应用/PL入口及Linux移植输入，尚未完成全新机器上的全系统重建。`vibration-offline.mk`的应用许可为MIT；`REDISTRIBUTE=NO`仅关闭范围过宽的Buildroot自动归档，项目源码的再分发权限见[许可说明](../license_status.md)。

## 串口PC

当前Python源码在`workspace/zynq_vibration_workbench_v7/software/serial_pc/`，运行依赖PySide6与pyserial。历史封装的版本pin尚未验证为当前版本的独立lock，仓库提供Python源码。运行入口和导出协议按该目录源码核对；发布PC可执行程序前须建立实际环境lock及依赖许可清单。
