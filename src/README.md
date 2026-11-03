# 源码与接口

- `workspace/zynq_vibration_workbench_v7/`：当前C++/Qt工作台、Python串口程序、分析、标注/训练/推理及测试；该目录的CMakeLists.txt为PS应用构建入口。
- `fpga-effective/source/`：当前组合平台真正使用的采集、显示和分析RTL/Tcl/约束；保持内部相对布局。
- `workspace/zynq_vibration/`与`workspace/zynq_vibration_continuous_v4/`：Linux驱动、Qt显示配套、Buildroot external和历史集成脚本。

当前有效RTL与V7软件分开保存以保留工程来源。仿真入口会将软件树中的scripts/tests/data和effective RTL合并到新的包外工作目录；不修改源中的算法字节。完整启动系统重建还需按build说明准备对应SDK、平台导出和Linux集成输入。

不含HLS是实际实现选择；未将外部厂商工具、安装包、巨大SDK或原始资料包当作自有源码发布。协议、驱动ABI与数据布局参阅源码及设计报告。
