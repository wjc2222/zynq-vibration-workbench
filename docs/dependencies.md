# zynq-vibration-workbench 依赖与复现范围

这是初级组项目的源码发布，包含原生应用、PL 有效源、Linux 配套、测试与 Skill。项目原创部分采用 MIT，已有第三方和派生组件保留原许可。本仓库提供源码与构建输入，完整系统从零重建仍受下文所列工具链和硬件依赖约束。

## 应用、测试与PC端

| 部分 | 实际依赖 | 仓库内证据与边界 |
|---|---|---|
| 原生特征与RF42 | C++17、CMake≥3.16、Threads、项目accelerator接口 | `workspace/zynq_vibration_workbench_v7/CMakeLists.txt`与`software/core/CMakeLists.txt`。生产69维提取/42维分类由C++实现，不依赖Python分类器 |
| 板端工作台 | Qt5 Core/Gui/Widgets | `workspace/zynq_vibration_workbench_v7/software/workbench/CMakeLists.txt`明确查找Qt5。仅安装Qt6不能等同于构建出完整工作台 |
| 其他GUI与采集/流服务 | 部分目标支持Qt5/Qt6；UNIX stream_bridge仍要求Qt5 Core | 对应`software/gui/`、`software/acquisition/`、`software/streaming/`、`software/stream_bridge/`的CMake。配置成功不保证所有可选目标都已生成，应检查所需目标 |
| Python串口PC端 | Python、PySide6、pyserial | `workspace/zynq_vibration_workbench_v7/software/serial_pc/app.py`、`serial_device.py`真实导入这些库。当前版本尚无经过PC重建验证的完整锁文件，仓库不打包wheel或exe |
| 旧模型训练/互操作工具 | Python3.10+、NumPy | `workspace/zynq_vibration_workbench_v7/tools/model_training/train_model.py`为历史36维candidate模型工具，不能当作当前RF42训练入口；按需使用，不是板端生产依赖 |
| 软件测试与Skill校验 | Python、目标的Qt环境；部分GUI测试需要X11/Xvfb | 检查各目标CMake与`skill/README.md`。Skill通用CLI仅用Python标准库。软件测试不代替实板、触摸或机械识别实验 |

此前研究用的SciPy、scikit-learn和PyWavelets探索脚本没有进入本仓库源码闭包，不列为当前板端运行必需项。需要重做该探索时，应另行选择、锁定与核对依赖，不能用研究库的结果替代原生C++模型。

## PL与系统配套

| 部分 | 版本/来源 | 取得方式与限制 |
|---|---|---|
| PL构建 | Vivado2025.2、目标XC7Z020、XFFT9.1 | `build-entrypoints/build-pl.ps1`使用`fpga-effective/source/`和项目FFT生成Tcl。工具/IP由使用者从AMD官方取得；仓库不提供工具授权、网表或预编译bitstream |
| Digilent HDMI源 | 本地rgb2dvi文件保留2014 Digilent BSD3条款 | `fpga-effective/source/display/vendor/rgb2dvi/LICENSE.txt`。具体复制revision仍需补来源记录，不以Digilent仓库根许可替换文件原条款 |
| ARM交叉构建 | 匹配目标ABI/Qt的Buildroot ARM hard-float SDK | `build-entrypoints/build-arm.sh`接收外部SDK路径。仓库不带预编译SDK；版本、运行库与对应材料应一并核实 |
| Linux/Buildroot | 既有平台为Linux6.6.70、Buildroot2025.02.19；另依赖Xilinx U-Boot启动链 | 仓库保留配套源码/recipe，上游归档另行取得。官方来源见`upstream_sources.json`；旧外部树和注入脚本仍有环境/硬件依赖，不是完整可移植系统构建入口 |
| Qt帧缓冲插件 | Qt5.15.18；既有Qtbase固定commit见来源JSON | `workspace/zynq_vibration/linux/qt-ax7zfb/prepare.py`校验Qt LinuxFB源哈希，代码含LGPL-3.0-only头。`qt-ax7zfb-direct`是实验变体，不能据目录存在断言其正在运行 |

旧系统清单将Bootlin2024.05-1工具链许可记为`unknown`，本仓库不分发其工具链或运行库。若后续提供rootfs、SDK或PC二进制，需按实际组件补齐来源、源码/修改和许可材料。Qt、Linux与Buildroot保留各自适用许可。

## 构建前的范围检查

从`build-entrypoints/`选入口，使用全新包外构建目录，显式提供工具/SDK。不要把历史`prepare_build.py`、内核注入或部署脚本当作任意机器/板卡的默认命令。文件身份使用本仓库的`SOURCE_MANIFEST.json`校验。

Linux配套仍有历史硬件lock与基础输出依赖待补齐，不能删除哈希断言或用假文件让脚本“通过”。硬件部署按目标板和存储布局单独验证。仓库不附真实机械样本或已训模型，识别精度和板端运行须在实际环境评估。

许可范围见[第三方说明](../third_party_notices.md)和[许可说明](../license_status.md)。官方URL、版本范围和本仓库相对证据路径集中在[upstream_sources.json](upstream_sources.json)。
