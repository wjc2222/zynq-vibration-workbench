# 通用工具输入契约

以下两个工具只依赖Python标准库；全部示例是合成的声明配置，不是硬件测速。规划模板的null未知项须先替换为确切输入，不能直接运行。

Both entry points are read-only and emit JSON to stdout, except human-readable `--help`. Exit 0 means the declared checks pass, 1 means a valid input fails one or more checks, and 2 means invalid JSON, schema or command arguments. Input `-` reads stdin. Unknown keys, duplicate JSON keys at any depth, NaN/Infinity, null numeric values and bool-as-number are rejected. An I/O error produces a JSON error without echoing local filesystem paths.

`engineering_budget.py` requires a JSON object containing `channels`, `sampling_hz`, `bytes_per_sample`, `per_record_overhead_bytes`, `uart_baud`, `uart_framing_efficiency`, `storage_MBps`, `duration_seconds`, `analysis_decimation` and `fft_size`. Channels, sample bytes, overhead, decimation and FFT size are integers; all inputs are positive finite numbers except overhead, which can be zero. Framing efficiency is a payload-bit fraction in (0,1]. One synchronized record contains a sample from every channel plus the declared record overhead. The tool calculates raw throughput, total recording bytes, UART full-raw feasibility, declared storage margin, FFT observation duration and bin spacing. Storage MB means 1,000,000 bytes. It does not infer available bandwidth, benchmark storage or account for omitted packet overhead.

Optional `resources` maps arbitrary caller-selected names to `{ "used": 80, "capacity": 200, "limit_fraction": 0.8 }`; used can be zero, capacity must be positive, limit defaults to 1 and must be in (0,1]. Optional `signal_max_hz` checks strictly below the post-decimation Nyquist frequency. A pass does not prove that a suitable anti-alias filter exists, and FFT bin spacing is not a promise of resolving nearby spectral components. No signal-band check is invented when this input is absent.

`dataset_audit.py` requires `{ "rows": [...] }`, with nonempty rows containing `label`, `split`, and exactly one of `group` or `session`. Splits are `train`, `validation` or `test`. Group/session share one namespace. Optional per-row `filepath` is an identity string and optional `hash` is 64 hexadecimal SHA256 digits; the tool neither opens samples nor recomputes their hashes. It checks group leakage between splits, duplicate filepath/hash identities even within one split, split population and label coverage. Filepath comparison normalizes backslashes and redundant dot segments, preserving case. A missing identity cannot be used to establish content uniqueness.

Optional `required_splits` selects a unique nonempty subset, defaulting to all three. Optional `expected_labels` is a unique nonempty list; by default it is the observed label union. Every expected label must occur in each required split; unexpected labels fail when an explicit expected set is supplied. Metadata labels/groups must be sanitized before sharing the report. No model fitting, predictions, scores or accuracy are produced. The planning templates with unknown `null` values are not direct CLI inputs: create a concrete profile/inventory using the documented schema.

The portable runner currently passes **62/62** contract checks. It exercises distinct audio and slow multichannel telemetry profiles, analytical expected values, UART/storage capacity boundaries, short FFT windows, resource ceilings, near/at/above post-decimation Nyquist, absent signal-band inputs, malformed types/nonfinite/duplicate JSON keys, and dataset coverage/leakage/duplicate identities. Tests use synthetic declared values and do not require hardware or sample files. The sanitized committed report is `skill/examples/portable/results-public.json`. The runner prints JSON by default; `--output FILE` explicitly writes the same report to a chosen destination.


## 预算公式与解释

- 同步记录字节数 = channels × bytes_per_sample + per_record_overhead_bytes。
- 原码字节率 = 同步记录字节数 × sampling_hz；记录容量 = 字节率 × duration_seconds。
- UART有效字节率 = uart_baud × uart_framing_efficiency / 8。8N1仅计起止位时效率为0.8；若还有协议开销，应进一步扣除，勿重复或遗漏计算。
- storage_MBps采用十进制MB，存储余量以声明的持续写入能力减原码速率计算；未声明的文件系统尾延迟不在预算内。
- FFT输入采样率 = sampling_hz / analysis_decimation；窗长 = fft_size / FFT输入采样率；bin间隔是其倒数。抽取前应另设计并验证抗混叠滤波器。
- resources只比较used/capacity与指定limit_fraction；不测量器件实际布局、端口、路由或时序。

工程预算、数据清单、人工实验计划是不同schema，不混用。示例与命令见[README](../README.md)。
