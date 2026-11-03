# 许可证与源码范围

`zynq-vibration-workbench` 的项目原创代码和文档采用根目录 [MIT 许可证](license.txt)。未另作声明的项目贡献适用 MIT；已有文件级声明和第三方许可证优先，本许可证不重新授予第三方内容的权利。

| 范围 | 适用许可与来源 |
|---|---|
| 项目原创算法、Qt 工作台、Python PC、测试、RTL、脚本与文档 | MIT；已有其他明确声明的文件除外 |
| Linux 采集、分析、显示驱动 | 保留逐文件 GPL-2.0-only 等声明；文本见 `licenses/linux/` 和显示驱动目录 |
| 采集、分析 UAPI | 保留逐文件 MIT 等声明 |
| Qt ax7zfb 与 ax7zfb-direct 配套 | 保留 LGPL-3.0-only；从 Qt 源生成的 integration 文件保留 Qt 原头和修改来源，文本见 `licenses/qt5base/` |
| Digilent rgb2dvi | BSD-3-Clause，保留 `src/fpga-effective/source/display/vendor/rgb2dvi/LICENSE.txt` 和原版权头 |
| DTS、DTSI 与 U-Boot 板级头 | 保留现有 GPL-2.0-or-later 等文件级声明 |

## 采集逻辑来源

`src/fpga-effective/source/combined/rtl/` 的 `ad7606_controller.v`、`capture_stream.v`、`capture_ip.v`、`speed_meter.v` 沿用或修改自项目收到的原采集工程；其中 `speed_meter.v` 与交接原件相同。这些文件的来源单独记录，不将整个平台描述为由同一作者独立原创。原工程整包和其附带文档不随本次源码发布。

## 构建元数据

两份 `linux/br2-external/package/vibration-offline/vibration-offline.mk` 将项目应用标记为 MIT。其 `VIBRATION_OFFLINE_REDISTRIBUTE = NO` 用于关闭 Buildroot 默认的源码归档：该归档会绕过此工程的 rsync 白名单并复制工程外延内容。此设置不撤销 MIT 的再分发权限。配套 `archive-app-source.py` 按 CMake 与软件源码范围准备归档；扩大归档范围前应先检查实际内容。

## 分发范围

本次提供源码、配置、构建入口、合成测试和文档。Linux、Qt、Buildroot、U-Boot、AMD 工具/IP和工具链按构建说明从相应上游取得，原组件的许可继续适用。固件、rootfs、SDK、PC 可执行程序、原始机械数据和训练模型不随本次分发；若以后提供二进制，应随实际组件补充对应源码、notice 和构建材料。

组件说明见 [第三方来源](third_party_notices.md)，依赖版本和取得方式见 [docs/dependencies.md](docs/dependencies.md)。
