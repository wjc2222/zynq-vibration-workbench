# 工程契约与适配

未知项保持未知，先做不依赖它的只读检查。不要照搬其他项目的板卡、串口、地址、分区、特征或阈值。

| 契约 | 需要填写 |
|---|---|
| source_root / output_root / protected_baseline | 自己的源码、隔离输出、受保护输入与版本 |
| part / board / tools / sdk_host | 完整器件型号、时钟、Vivado版本、目标OS/Qt/交叉SDK与ABI |
| channels / sample_rates / raw_format | ADC映射、方向、单位、帧字节序、CRC/index/tick/session、停止语义 |
| pl_contract | 寄存器ABI、握手、窗口与FIR历史、定点位宽、舍入/饱和、FFT索引与缩放 |
| ps_contract / labels / sessions | 特征语义、mask、归一化、模型schema、类别、独立会话定义 |
| ui_contract | 模式、单位、分辨率、空数据入口、可写持久配置根 |
| board_identity / storage / authorization | 目标板与介质身份、实际分区/挂载、保留清单、授权范围 |
| acceptance | 参考来源、数值误差预算、性能范围/次数、UI与物理验收项 |

```text
使用本目录的zynq-vibration-workflow。
目标=[功能或故障]；源码根=[自己的路径]；输出根=[新的路径]。
保护基线=[自己的版本]；器件/工具/协议=[已确认值]。
允许操作=[只读/隔离修改/构建/指定目标板具体操作]。
验收=[参考、容差及理由、性能计时范围、UI和物理项]。
先列契约缺项，复用可用方法，输出实际通过/失败及未验证项。
```

例如双通道声学任务先定义通道单位、采样和滤波器，再独立生成黄金参考和候选JSON；比较器不限制通道数。换温度分类或PYNQ时可复用分组验证和身份追溯，但须重新开发合适驱动/overlay/特征，不意味着本包在该平台已上板验证。
