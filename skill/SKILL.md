---
name: fpga-soc-engineering
description: 为 FPGA/Zynq 等 SoC 数据处理工程完成需求到 PL/PS 划分、资源与吞吐预算、定点加速验证、PS 分组建模、响应式显示及可复现交付。用于新建、移植、优化和跨层排障；通过参数化契约、模板和标准库脚本复用，不依赖特定 ADC、通道数、分类标签或板卡地址。
license: MIT
---

# FPGA/SoC 数据处理工程

把用户任务转成可以实现、比较和复验的工程。按当前问题选择路线，不要求每次读完所有参考。振动工作台是经验来源和实例，不是新工程默认规格。

## 接收与适配

先确定交付对象：架构、隔离代码改动、仿真、优化、应用、移植或发布。沿用用户明确的范围和授权，记录影响当前决策的数据速率、帧布局、器件/时钟、数值精度、通信/存储、OS/SDK和验收指标。使用[工程适配](references/adaptation.md)和[契约模板](templates/project-contract.json)。未知项保留null，只追问阻止当前步骤的缺项，继续其他独立工作。

明确应保留的原码、接口和算法，以及允许改变的模块。输出与保护基线分开。不要把上一工程的设备节点、MMIO地址、分区、标签、模型或容差代入新任务。

## 按问题选择执行路线

| 任务 | 读取与使用 | 产出 |
|---|---|---|
| 新架构、PL/PS划分 | [适配](references/adaptation.md)、[PL](references/pl.md) | 数据流、职责、接口、吞吐/存储/运算预算与对照实验 |
| 采集、保存、预览缺口 | [采集链](references/acquisition.md) | 原码与预览契约、连续性、背压与停止语义、最小复现 |
| PL资源或性能优化 | [PL](references/pl.md)、[优化模板](templates/optimization-review.md) | 位宽与端口预算、黄金参考、资源取舍、数值/时序/A/B结果 |
| 特征、模型或精度 | [PS](references/ps.md)、[实验模板](templates/experiment-plan.json) | 独立分组、训练/验证隔离、schema、业务指标 |
| 绘图、卡顿、触屏 | [UI](references/ui.md) | 有界队列、物理量/量程、异步计算、真实交互验收 |
| 换板、重建、部署 | [部署](references/deployment.md)、[部署模板](templates/deployment-plan.json) | 目标身份、ABI/存储/版本兼容性、保留项与恢复计划 |
| 跨层异常 | [排障](references/troubleshooting.md) | 观察→假设→最小实验→修复→同条件复验 |
| 成果和复用结论 | [证据](references/evidence.md)、[验收模板](templates/acceptance-plan.md) | 指标口径、输入/源身份、证据等级、未覆盖范围 |

## 先预算再实现

同步采样按真实记录格式计算 `bytes_per_record = channels × bytes_per_sample + overhead`，再乘记录速率。UART扣除起止位和协议效率；存储/网络使用明确声明或测量值，不套理论峰值。执行 `scripts/engineering_budget.py --profile PROFILE.json`，输入契约见[README](README.md)。预算失败时改数据分流、分块或接口，不能抽稀后仍声称保存完整原码。

PL划分依据是吞吐、确定性、并行性、状态容量和传输成本。先写运算次数、位宽、BRAM端口、流水线间隔和每窗调度成本；资源占用率本身不是目标。

## 实现并验证PL加速

冻结输入、系数、定点/溢出规则、索引、握手与完成语义。先独立黄金参考，再单元、真实IP集成、布局布线，最后按任务所需做板级比较。逐码目标使用零容差；浮点容差在试验前按误差预算规定，使用 `scripts/compare_numeric.py`。

优先研究对称预加、核共享、多通道并行、双口BRAM、流水线、批处理与搬运开销；用实际报告确认映射。一次乘法不一定等于一个DSP，FIR局部周期也不等于整机延迟。`scripts/summarize_vivado.py`摘录资源和设计级时序，解析成功不代表CDC、DRC和全部约束合格。

本项目的具体DSP/LUT/FF/BRAM利用、FIR实现与分层效果见[PL案例](examples/project_case/README.md)。新任务替换通道、核、系数和接口后重新验收。

## PS与显示闭环

监督学习先按独立试验/设备/对象分组，之后切分；只在训练折拟合特征选择、标准化和模型。用 `scripts/dataset_audit.py --input DATASET.json`核查清单泄漏与覆盖；该工具不训练模型、不证明准确率。类别、特征与算法由任务决定，不固定为四类或随机森林。

原始保存、分析和绘图使用独立任务及有界队列。物理量单位、纵轴锁定、手势、停止状态与配置重载须实际操作；过载时先满足原码保存承诺，再丢弃允许丢的预览。

## 收尾

同输入、相同配置和身份，先核结果再比较性能；写明计时段、预热、样本数与统计。资源、时序、应用与bitstream须可追溯到对应构建。用 `scripts/project_audit.py`核入口及选定文件SHA；新文件不能沿用旧清单。

交付契约、替换参数、实际入口、依赖、验证命令、失败处理与证据。方法与离线脚本可跨任务复用；驱动、overlay、DMA、引脚/时钟、模型和UI后端需要目标适配。没有跨板PYNQ运行证据时，不宣称通用PYNQ部署通过。

## 调用示例

```text
使用 fpga-soc-engineering。我要把双通道声学FIR从PS移到PL。
采样48kHz、16位，器件与时钟见project-contract.json。
先计算吞吐与存储，再给软硬件划分、定点契约和A/B验收。
在我给出的源码根与隔离输出中实现，保留现有采集格式。
```
