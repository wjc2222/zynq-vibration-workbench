# 检测模型与实测训练

本目录提供可部署的四类线性模型训练程序，输出可由板端 `vibration_detect` 和 HDMI 检测页直接加载的 `candidate.model`。初始检测使用“实测正常基线 + 工程先验”，正常、不平衡、不对中、松动都能产生候选分数，证据不够时输出未知。它不是已经用你们台架四状态数据训练好的分类器。

## 明天先完成的验证

1. 固定三轴传感器安装位置和方向，配置实际灵敏度，确保测速每转 1 个有效脉冲。速度采用实际测速，不能把调速旋钮百分比当作真实 rpm。
2. 先采一份明确正常、转速稳定的 10 秒以上记录。打开该记录，在检测的模型设置中确认机械输入和正常状态，再建立正常基线。至少要有 5 个互不重叠且通过质量检查的窗口。
3. 保存基线后，分别采正常、不平衡、不对中和松动。保持安装、采样率、换算参数、转速工况一致，记录实际机械状态。查看候选、依据和拒识原因，保留 BIN、配套 JSON 与实验记录。
4. 换安装位置、传感器、换算参数或采样档后重新建立基线或选择匹配模型；不要把“配置不符”当作软件故障绕过。转速超出基线范围会拒绝判别。

没有转速的信号发生器数据仍可验证三轴原码、FIR、FFT、幅值和绘图；不会用于训练机械故障分类，也不会自动变成“正常”基线。

## 导出与标注

板端命令示例（也可通过界面导出特征）：

```sh
vibration_detect features --input /path/data.BIN --output /new/features.jsonl --setup rig1_mount1 --confirm-mechanical --backend hardware
vibration_detect baseline --input /path/normal.BIN --output /new/normal.model --setup rig1_mount1 --confirm-mechanical --confirm-normal --backend hardware
vibration_detect infer --input /path/test.BIN --output /new/result.json --setup rig1_mount1 --confirm-mechanical --model /new/normal.model --backend hardware
```

CLI 默认导出的特征采用电压单位。界面使用当前选定的电压/加速度换算配置，两者模型不可混用。硬件不可用时会明确失败；只有主动指定 `--backend reference` 才在 ARM/电脑参考后端计算。

导出的 JSONL 每行对应一个完整窗口。根据实验记录填写 `label`、`run_id`、`group_id`，其余计算字段保持原样：

| 字段 | 取值与作用 |
|---|---|
| `label` | `normal`、`imbalance`、`misalignment`、`looseness`，不能由信号外观猜标签 |
| `run_id` | 一次实际采集的标识；同一采集不能改名伪装成新试验 |
| `group_id` | 一次独立机械状态设置/复装试验；同设置的连续片段、速度档、重复窗口属于同组 |
| `setup_id` | 整体台架与传感器安装配置，须与推理时相同 |
| `capture_sha256` / `raw_window_sha256` | 程序从原始字节计算，用于排除重复内容 |
| `source_session_id` | 原始采集会话，跨 SD 分段仍保持同源 |

第一轮小规模采集主要验证流程和候选规律。训练工具要求每类至少 3 个真正独立的组，且拆分后每类至少 2 个训练组；不能仅把一份 10 秒文件切成多个片段满足这个条件。

## 电脑训练

依赖 Python 3.10+、NumPy。以下命令不修改原始数据或旧模型，输出目录必须是新目录：

```powershell
python train_model.py labeled.features.jsonl --output models\rig_linear_v1 --id rig_linear_v1
```

训练固定使用 36 个特征和四类线性 softmax 判别器。标准化仅从训练组估计，优化按类别和组平衡权重；独立留出组不参与拟合、阈值选择或调参。拒绝无标签、合成、质量不合格、配置混用、原始窗口重复或同会话跨训练/测试组的数据。

输出包括模型和训练报告。报告同时给出逐窗口及等组权重指标、混淆矩阵、未知/拒识比例、逐组表现、来源哈希和拆分清单。只有“被接受样本的准确率”高而拒识很多，不代表整体性能好；要同时看覆盖率和包含拒识的召回率/F1。

模型始终标记 `provisional`。工程先验阈值、分数和训练后的 softmax 输出都不是校准概率，也不等同于已经验证的机械故障准确率。最终需要另一天、重新设置机械状态的独立实测验收，不能反复根据留出集改阈值再将它称为独立测试。

## 特征与数值边界

每轴依次为：交流 RMS、峰值因子、Pearson 峭度、1X RMS、0.5X/1X/1.5X/2X/2.5X/3X 功率比、4–10X 合并功率比、非谐波功率比。三轴合计 36 维；RMS 使用当前电压或加速度单位，其余项无量纲。功率比例参考带为 2–500 Hz。

每个计算窗包含 FIR 所需完整历史和 8192 个 5 kHz 输出点。原始 50/25/10/5 kHz 采集与 SD 格式不变。质量检查包括原码连续、削顶、数值平线、测速有效性、转速稳定性和倍频带是否可分。数值平线门只防止除零/不稳定比值，不代表经过实测标定的仪器噪声底。

```powershell
python -m unittest -v test_train_model
```

测试使用明确标为软件夹具的构造特征，仅证明训练拆分、模型格式和拒识处理可运行，不证明四状态实测准确率。
