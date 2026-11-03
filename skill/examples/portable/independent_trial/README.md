# 陌生队伍独立试用

独立代理只获得本Skill和另一项声学采集/分类需求，自行建立输入并执行工具。本目录是合成需求与声明清单，不含原振动项目配置、硬件测量或模型训练。

需求：8通道，20kHz/通道、3字节/样点、每同步记录12字节开销，UART115200且总载荷效率0.8，存储2MB/s，60秒；抽取4、FFT1024，要保留6kHz信号。

实用结果：原码720000B/s，UART11520B/s，存储预算通过但全原码串口预算失败；抽取后Nyquist2500Hz，频带失败。改为不抽取后频带通过，串口瓶颈仍存在。因此原码走足够快的本地保存链路，串口保留控制/低速摘要；不能为串口带宽而悄悄抽稀原始数据。PL是否承担FFT应在PS基线与调度预算之后决定。

数据计划：正常/故障两类均覆盖所有split，但run-a同时用于train和validation，工具正确指出泄漏；将该试验全部归train后清单检查通过。后者不代表样本物理独立或模型准确。

从skill根运行（前3项期望exit1，最后一项期望exit0）：

```bash
python3 -B scripts/engineering_budget.py --profile examples/portable/independent_trial/requested-profile.json
python3 -B scripts/engineering_budget.py --profile examples/portable/independent_trial/candidate-no-decimation-profile.json
python3 -B scripts/dataset_audit.py --input examples/portable/independent_trial/requested-dataset.json
python3 -B scripts/dataset_audit.py --input examples/portable/independent_trial/candidate-grouped-dataset.json
```

输入与对应完整JSON输出均在本目录，[独立复用记录](review.json)标明范围。首次编写过程中发现的缺失模板已补齐并经独立复查；最终使用以现有文件为准，未用措辞替换实际执行。
