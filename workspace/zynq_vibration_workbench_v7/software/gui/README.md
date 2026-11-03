# Qt 多页分析工作区

此界面只读打开已保存的 CAPDDR2v2 BIN 文件，不控制采集。当前没有经验证的故障分类模型，始终保留“未分类”边界。合成文件全程显示“合成演示”，不能在参数中改成真实机械来源；无效 sidecar 也会锁定来源确认。名义灵敏度 500 mV/g、增益 -1 尚未标定，存储 CH1/CH2/CH3 对应 ADC1/3/5，物理 XYZ 待确认。

工作区按任务分为五页，使用 QStackedWidget 和固定文字导航。初始 1024×600，支持 1280×720；主操作与参数控件高度至少 44px。字体优先 Noto Sans CJK SC / Microsoft YaHei，白与浅灰配单一蓝灰强调色，三通道颜色全程一致。

- 文件：选择目录、单层 BIN 列表（最多 500 项）、刷新、校验元信息、本次会话最近打开、演示数据入口。无卡或空目录保持空态，不自动挂载、格式化或写入 SD。
- 波形：一张大图、独立通道开关、全长或指定时间区间。区间从当前已校验 Capture 的原始 records 重新保留三通道极值；小区间可恢复每个采样。单击读取最近原始样本，不从旧概览插值。区间不改变全记录统计、FFT 或诊断。
- 频谱：分段平均频谱、通道开关、最近真实 FFT 频点游标、各通道主峰。有效且稳定测速时显示已有 1X/2X/3X 固定转频参考；不称阶次跟踪，不做角域重采样。
- 转速：独立文件记录曲线、平均/最小/最大、有效率与 PPR。未知 PPR 时 RPM 为 —，曲线使用脉冲 Hz。NaN 缺测保持断线。
- 诊断：未分类、时域统计、转频频带与来源/校验/算法边界详情。JSON 和真实摘要 CSV 通过 QSaveFile 原子保存，拒绝原 BIN、sidecar 及其符号链接目标；CSV 正确转义引号、逗号和换行。

数据读取、CRC 与分析在 QThread 后台运行。只缓存当前一份 Capture，参数变更或加载新文件立即清理旧结果；错误不会显示旧曲线或允许导出。关闭界面等待有限文件分析结束。保留全部 core 边界，不更改协议或算法。

依赖 C++17、vibration_core、Qt 5.15/6 Widgets，不依赖 Qt Charts、WebEngine、OpenGL。

```sh
./vibration_gui /path/CAP00003.BIN --ppr 1 --fullscreen
./vibration_gui /path/CAP00003.BIN --ppr 1 --page waveform
./vibration_gui --size 1280x720 --page files
./vibration_gui --directory /media/sd/CAPTURE --fullscreen
QT_QPA_PLATFORM=offscreen ./vibration_gui sample.BIN --ppr 1 --page spectrum --screenshot /tmp/spectrum.png
```

`--page` 接受 files/waveform/spectrum/speed/diagnosis；`--size` 接受 1024×600 至 7680×4320 的 `WxH`。`--ppr` 为显式启动覆盖，须有限且在 `[1e-6,1e9]`，与非零文件头冲突时拒绝；导出保留 explicit_user_override 来源。演示入口优先查找程序旁的 `samples/synthetic_10s.BIN`、`../../samples/synthetic_10s.BIN` 或工作目录 `artifacts/samples/synthetic_10s.BIN`；不存在时提供文件选择，不静默生成或伪造数据。

`--directory <path>` 只预选文件页目录并列出其中可读的 BIN（不递归），不会自动打开、分析或修改文件。目录不存在、不可读或路径不是目录时显示中文空态，界面与演示按钮仍可使用；之后仍可选择目录或手动打开文件。同时提供位置参数 BIN 时，该文件仍按显式打开请求处理，并显示其所在目录。

启用 BUILD_TESTING 后，ctest 包括 core 70 项协议/信号测试、独立 raw 预览数学测试，以及 5 种异步 GUI 状态。GUI 测试覆盖目录预选与只列文件、空/缺失/非目录、导航、通道、raw/FFT 游标、区间不改全记录分析、原子 JSON/CSV、原文件/sidecar/符号链接保护、关闭 worker、44px 控件、来源锁与启动 PPR 边界。主机离屏截图不证明 ARM 部署、HDMI/触摸或实机故障识别性能。

工作流参考：Dewesoft X 的文件/分析分离与通道/区间选择，OROS NVGate 的任务分区与游标。采用原创轻量界面，不复刻品牌 UI，不暗示实现其高级算法。
