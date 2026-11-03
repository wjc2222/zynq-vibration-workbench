# 隔离单元的证据范围

`result_port_unit_tb.sv` 直接预填内部 sample/power/stat/band RAM，强制状态为 DONE，验证全部结果地址的 `INDEX -> completed READY_INDEX -> LO/HI` 协议。包括24576个sample、12291个单边power、54个频带、24个统计字段及400次重复/远跳，共37345次64位核验。此单元不计算FIR或FFT，不能用来证明频谱数值或硬件性能。

`fir_unit_tb.sv` 对四档采样各输入满量程、DC、阶跃、逐点交替、脉冲、确定性噪声和按系数符号对齐的满幅饱和应激，共28例。最后一类利用系数L1增益大于2，确实触发signed24-Q7钳位，避免只验证永远为零的饱和计数。独立Python int64卷积参考使用冻结Q23系数、ties-to-even右移16位和signed24钳位。每个case完整输出8192点三轴，逐值读取最终RAM并核验min/max/sum/sumsq/全部raw端点/饱和计数。FIR完成到MEAN后，testbench强制DONE，仅用于读取冻结FIR结果；`fir_unit_fft_stub.sv` 明确不提供FFT输出。不能将此结果归入FFT数值验收。

```powershell
pwsh -File hardware/analysis_v6/tests/run_isolated_units.ps1
# 仅快速结果读口核验
pwsh -File hardware/analysis_v6/tests/run_isolated_units.ps1 -BusOnly
# V7四档direct旧求值器的饱和应激与周期对照（4例）
pwsh -File hardware/analysis_v6/tests/run_isolated_units.ps1 -DirectFir -WorkDirectory E:\zynq_workbench_v7_build\fir_unit_direct
```

默认工作目录 `E:\zynq_workbench_v7_build\fir_unit` 与主厂商FFT仿真的工作目录独立。生成的mem、编译快照和日志都在该构建目录内；任务证据只需保留明确范围的PASS日志、manifest及参与验证的RTL哈希。生成器需要NumPy。最终bitstream中的FFT仍须另跑厂商实际核与独立数值/板端验证。

V7增加了 `FIR_UNIT_CYCLES`：testbench逐拍计数FIR状态，断言direct为`(taps+2)*8192`、对称预加为`((taps+1)/2+3)*8192`，再报告相同AXI输入方法下START至MEAN周期。这是仿真逻辑周期，不是PS驱动运行时间、CPU使用量或整机响应速度。`summarize_fir_performance.py SYMMETRIC_WORK DIRECT_WORK EVIDENCE`验证所有要求的PASS、四档周期、冻结系数对称性，以及参与编译的RTL/测试副本与最终源文件哈希一致后生成JSON和小型日志证据。

完整真实FFT仿真可先用 `scripts/prepare_replay_batch.tcl BUILD SOURCE` 编译/展开，随后在 `BUILD/sim_project_batch/sim_v7.sim/sim_1/behav/xsim` 运行 `xsim tb_detection_accel_behav -runall -log actual-fft.log`。必须确认`RTL_PASS profiles=4 sample_checks=32768`并调用`compare_fft_model.py`；仅仿真器退出不构成验收。该batch路径保留实际AMD XFFT模型，省去GUI波形记录。
