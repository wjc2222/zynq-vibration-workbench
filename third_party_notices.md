# 第三方来源与许可说明

项目原创部分采用 [MIT](LICENSE)。以下第三方和派生组件保留自己的版权、许可条件及免责条款；根目录许可证不覆盖这些声明。

## 随源码提供的组件

| 组件/文件 | 许可与处理 |
|---|---|
| Digilent rgb2dvi | 2014 Digilent BSD-3-Clause，保留 `fpga-effective/source/display/vendor/rgb2dvi/LICENSE.txt` 及源码原头；上游仓库根 MIT 不替换这个 IP 的 BSD 声明 |
| Linux 采集、分析、显示驱动 | `.c` 文件按 GPL-2.0-only 等原头；相关 UAPI 另按 MIT 等逐文件声明。文本见 `licenses/linux/` 和 `workspace/zynq_vibration/hardware/display/driver/LICENSE` |
| Qt ax7zfb 配套 | `workspace/zynq_vibration/linux/qt-ax7zfb/`、`qt-ax7zfb-direct/` 保留 LGPL-3.0-only；准备脚本从匹配 Qt 源生成 integration 派生文件并保留原头，许可文本见 `licenses/qt5base/` |
| DTS/DTSI、U-Boot 板级头及补丁 | 保留文件已有 GPL-2.0-or-later 等声明及被修改上游的适用许可 |
| Buildroot external 配置与 recipe | 项目原创配置、脚本采用 MIT，修改上游部分按原许可；Buildroot 本体文本见 `licenses/buildroot/copying.txt` |

原采集工程对有效采集 RTL 的贡献见 [采集逻辑来源](license_status.md#采集逻辑来源)。原整包不随此仓库提供，也未将其来源改写为项目独立原创。

## 外部取得的依赖

- 板端采用 Qt5，已审计源版本为 5.15.18。主许可文本保留于 `licenses/qt5base/`；实际取得的 Qt 源及模块中的独立第三方条款仍须保留。
- PC 源码依赖 PySide6/shiboken6 和 pyserial。这些 wheel 未 vendor；其 LGPL/GPL/商业许可选择及第三方 notice 以实际安装组件为准。pyserial 3.5 使用 BSD-3-Clause。
- Linux、Buildroot、U-Boot 和 ARM 工具链按 [构建说明](docs/build.md) 取得，仓库提供项目配置与修改。上游源码及运行库保持自己的许可。
- AMD Vivado/Vitis 与 FFT 等 IP 从官方取得，受其实际协议约束；本仓库不附工具安装、IP 模型库、固件或网表。
- PyInstaller 是外部封装工具；若以后发布 PC 可执行程序，需保留所用应用依赖的许可与 notice。历史 Python 研究依赖也应按实际使用版本单独管理。

本仓库没有分发 Qt/SDK/rootfs/PC 二进制。此处记录源码所需依赖，不是未来任意二进制组合的完整第三方清单。官方来源和版本适用范围见 [docs/upstream_sources.json](docs/upstream_sources.json)。
