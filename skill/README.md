# 可复用 FPGA / Zynq 工作流与离线校验工具

这是用于准备公开的脱敏候选，尚未发布。按[SKILL.md](SKILL.md)使用从采集、PL数值处理、PS监督检测、UI到交付的工作流。脚本只依赖Python3.9+标准库，不需要板卡、私人数据或原项目路径。新的器件、ADC、协议、采样率、PL ABI、模型schema和OS均需先填[工程契约](references/adaptation.md)。不提供通用ADC驱动、即装固件或默认刷机操作。

## 新队伍使用

1. 把源码、输出与保护基线设为自己的路径；新输出放源包之外。
2. 运行只读诊断，选择[采集](references/acquisition.md)、[PL](references/pl.md)、[PS](references/ps.md)、[UI](references/ui.md)或[部署](references/deployment.md)步骤。
3. 保存自己的输入/参考/失败记录与构建身份，按[证据边界](references/evidence.md)报告验证范围；未知信息保持未知。

三个CLI只读输入，将UTF-8 JSON写到stdout。返回0表示比较/核验通过或报告解析成功，1表示比较/核验不通过，2表示输入/格式错误。`--help`说明参数，容差必须由工程误差预算决定。

```powershell
# 在skill目录运行；带空格的路径加引号。python替换为自己的Python3解释器。
python -X utf8 -B scripts/compare_numeric.py --actual actual.json --expected expected.json --atol 1e-6 --rtol 1e-3 --source-kind synthetic
python -X utf8 -B scripts/summarize_vivado.py --utilization utilization.rpt --timing timing_summary.rpt --source-kind actual-report
python -X utf8 -B scripts/project_audit.py diagnose --root "path/to/source" --require build.ps1
python -X utf8 -B scripts/project_audit.py manifest --root "path/to/source" --file src/core.cpp > new-manifest.json
python -X utf8 -B scripts/project_audit.py verify --root "path/to/source" --manifest new-manifest.json
```

| 工具 | 输入与用途 | 拒绝/限制 |
|---|---|---|
| compare_numeric.py | 有限数值叶子的JSON树；abs(a-e) ≤ atol+rtol×abs(e) | shape/键不匹配、NaN/Inf、bool、空树、重复键、运算溢出等失败；不生成算法黄金参考 |
| summarize_vivado.py | 同构建资源与设计级时序报告 | 仅支持明确标准列序；器件配对、完整表头和端点数关系检查；未知格式失败。exit0仅解析成功，不是资源/时序合格门，不涵盖CDC/DRC/约束完整性或源绑定 |
| project_audit.py | 显式root与选定文件清单SHA | 拒绝路径越界/链接/重解析点、缺文件、改动、清单错误。只检查列出文件，不是ZIP安全解压器或可信来源签名 |

## 可复现合成回归

```powershell
python -X utf8 -B examples/validation/run_checks.py --output my-validation.json
```

输出必须为新文件。夹具在明确临时目录生成并自动清理；公开结果中的临时绝对路径替换为`<temporary-test-root>`，不改变退出码和断言。当前结果见[results-public.json](examples/validation/results-public.json)，记录本候选脚本SHA；这些SHA不代表私人回执、原始报告或脱敏前文档的字节身份。

本版本有27个**全部合成**案例：保留私人版27项中的26个合成断言，移除依赖私人实际报告的一项，替换为新造的12列设计级时序成功案例。相同数量不表示相同覆盖；没有重新测试私人真实Vivado报告。覆盖绝对/相对容差、shape、NaN/溢出/bool/重复键、资源/时序缺失与未知布局、器件配对、资源表头/列次序、setup/hold/pulse端点关系以及SHA/越界/入口缺失。CLI没有启动Vivado、连接板卡或训练。

## 方法来源与脱敏边界

工作流来源于既有工程实践，包括预览流与连续原码混用、局部周期收益冒充完整API收益、弱频带覆盖、会话泄漏、配置持久化、存储布局差异与字节行尾等问题。[方法案例](references/evidence.md)明确保留可迁移的做法和证据层次；[公开索引](references/evidence-index.json)仅索引本候选资料。

原始验收证据保存在私人提交包，本候选未复制部署回执、介质标识、原始采集会话、模型身份、私人路径映射或旧验证结果。这里不是原始证据的匿名化等价副本；既有历史测试的数字不能作为本候选或新队伍的硬件验收结论。新的部署、性能、物理采集和独立检测指标须由使用者自己的实验证明。故障处理见[故障定位](references/troubleshooting.md)。
