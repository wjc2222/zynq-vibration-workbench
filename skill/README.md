# FPGA/SoC 工程 Skill

本包将采集、PL加速、PS分析、交互与交付经验转为工作流、模板、只读校验工具及排障案例。入口[SKILL.md](SKILL.md)，名称 `fpga-soc-engineering`。适用于多通道采集、声学/振动处理、遥测等数据链，不要求特定ADC、通道数、故障标签或模型。

## 陌生队伍开始使用

1. 复制整个skill目录，或克隆仓库后让支持SKILL.md的智能体读取。仅运行离线工具需Python3.9+，不需板卡、Vivado或第三方Python库。
2. 复制[工程契约](templates/project-contract.json)，填写自己的数据、器件、时钟、接口和验收。null表示未知，不是可执行默认值；工具profile与人工契约分开。
3. 选下表路线，使用对应提示词和模板，再用自己的输入、参考与报告验证。
4. 离线成功之后，完成目标工程自己的构建、板级和业务效果验收。

## 各项范围、用法与失效条件

| 能力 | 场景、输入和产出 | 失效条件及已验证范围 |
|---|---|---|
| [PL/PS划分与适配](references/adaptation.md) | 新任务/换器件；填写契约，预算吞吐/内存/截止期，产出架构与接口 | 缺协议、实测带宽或精度预算时给条件性方案；预算计算不证明硬件达到指标 |
| [采集/保存/预览](references/acquisition.md) | 连续采集/缺口/错误波形；独立核原码、预览、时间索引、背压 | 无原码/协议时不能由截图恢复信号；使用本协议的完整性机制，不强加CRC字段 |
| [PL资源与加速](references/pl.md) | FIR/FFT/统计等核；位宽/端口、参考、综合/时序和同输入A/B | stub不代表真实IP；WNS不覆盖CDC/约束；局部周期不推为整机速度；见[实际案例](examples/project_case/README.md) |
| [PS特征与建模](references/ps.md) | 分类、异常检测或回归；按独立试验分组，训练折拟合，保存模型契约 | 类别缺失、会话泄漏、分布改变时不能报泛化；清单审计不证明模型精度 |
| [显示与交互](references/ui.md) | GUI卡顿/触屏/量程；有界队列、异步计算、原始/绘图分流、实际验收 | 高刷新率不代表低触摸延迟；显示抽样不可进入原码或算法；新后端需重测 |
| [移植与部署](references/deployment.md) | 换板/OS/持久启动；目标身份、ABI、存储、版本与恢复计划 | 旧板分区/路径不能套用；软件重启不证明掉电启动；依赖未匹配不算重建 |
| [跨层排障](references/troubleshooting.md) | 定位硬件/传输/文件/绘图/模型问题；最小实验闭环 | 缺证据时保留条件性结论，不把猜测写成根因 |
| [证据与优化报告](references/evidence.md) | 资源、速度与复用效果；记录口径、输入/源身份及验证等级 | 合成案例/训练回放/报告解析不等于机械或跨板效果 |

## 五个可执行工具

工具只读输入，向stdout输出UTF-8 JSON。`0`表示工具范围内成功，`1`表示有效输入未通过检查，`2`表示输入/格式错误；Vivado摘要的0仅表示解析成功。`--help`给出参数。

| 工具 | 输入→输出 | 边界 |
|---|---|---|
| [engineering_budget.py](scripts/engineering_budget.py) | profile→原码吞吐、UART载荷、存储、FFT窗长/bin间隔与可选资源预算 | 带宽是声明值；不测速，不验证抗混叠滤波器 |
| [dataset_audit.py](scripts/dataset_audit.py) | 标签/分组/切分清单→泄漏、重复与类别覆盖 | 不读波形、不训练；分组独立性由实际试验决定 |
| [compare_numeric.py](scripts/compare_numeric.py) | 两个有限数值JSON树→绝对/相对容差比较 | 容差先规定；拒绝shape/重复键/NaN/Inf/bool等，不生成黄金参考 |
| [summarize_vivado.py](scripts/summarize_vivado.py) | 同构建资源/设计级时序→严格摘要 | 支持明确列序/器件别名，未知格式拒绝，不审CDC/DRC |
| [project_audit.py](scripts/project_audit.py) | 源码根、入口或选定成员→诊断/SHA清单/核验 | 拒绝越界/链接/重解析点，只保证列出文件 |

在skill目录运行，两个跨任务profile和数据清单是明确标记的合成教学输入：

```bash
python3 -B scripts/engineering_budget.py --profile examples/portable/audio_profile.json
python3 -B scripts/engineering_budget.py --profile examples/portable/telemetry_profile.json
python3 -B scripts/dataset_audit.py --input examples/portable/dataset_good.json
python3 -B scripts/compare_numeric.py --actual actual.json --expected expected.json --atol 0 --rtol 0 --source-kind synthetic
python3 -B scripts/summarize_vivado.py --utilization examples/project_case/evidence/utilization.rpt --timing examples/project_case/evidence/timing_summary.rpt --source-kind actual-report
python3 -B scripts/project_audit.py diagnose --root /path/to/source --require CMakeLists.txt
python3 -B scripts/project_audit.py manifest --root /path/to/source --file src/core.cpp > /new/output/manifest.json
python3 -B scripts/project_audit.py verify --root /path/to/source --manifest /new/output/manifest.json
```

Windows可将python3替换为 `python -X utf8`。预算失败按设计返回1，先读JSON原因，不视为崩溃。完整输入字段和公式见[工具契约](references/tool-contracts.md)。

## 可替换模板

| 模板 | 使用时机与产出 |
|---|---|
| [project-contract.json](templates/project-contract.json) | 新工程/移植前：数据、数值、软硬件与验收契约 |
| [acceptance-plan.md](templates/acceptance-plan.md) | 实现前：功能、输入、期望、证据和失败动作 |
| [experiment-plan.json](templates/experiment-plan.json) | PS模型前：独立试验、标签、切分与指标，不预填准确率 |
| [optimization-review.md](templates/optimization-review.md) | 优化前后：资源选择、数值一致性、局部/完整耗时A/B |
| [vivado-interface-checks.tcl](templates/vivado-interface-checks.tcl) | 在自己的已打开设计中只读盘点接口/时钟，不自动加约束 |
| [deployment-plan.json](templates/deployment-plan.json) | 确定目标后：身份、存储、版本、保留项和恢复计划 |

模板是设计/复核输入，不直接视为脚本配置或可烧写工程。更换ADC、寄存器、DMA、Linux/PYNQ后端和模型时须实现相应适配。

## 实际验证与复验

- [原三个CLI回归](examples/validation/run_checks.py)与[记录](examples/validation/results-public.json)：27项全部合成，覆盖误差、格式、报表和SHA/路径边界。
- [通用工具回归](examples/portable/run_checks.py)与[记录](examples/portable/results-public.json)：跨声学/遥测预算、分组审计及失败路径，数量以结果文件为准。
- [PL工程案例](examples/project_case/README.md)：具体资源映射、对称FIR、真实布局布线和分层性能；公开报告副本只做声明的身份字段脱敏，数值保持原样。
- [Vivado模板试验](examples/project_case/evidence/template_smoke.json)：真实Vivado2025.2中对最小教学设计完成正确配置、未知目标拒绝、时钟不符拒绝3项检查；不代表实际平台重新布局布线或上板。

```bash
python3 -B examples/validation/run_checks.py --output /new/output/cli_checks.json
python3 -B examples/portable/run_checks.py --output /new/output/portable_checks.json
```

使用新输出文件，测试临时夹具生成后自动清理。跨任务离线复用不等于跨板物理验收，也不直接证明赛题的通用PYNQ加分；后者需要实际overlay/驱动适配与跨板运行证据。
