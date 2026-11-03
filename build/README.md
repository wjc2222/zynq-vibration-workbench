# 构建与实现报告

`scripts/`提供应用主机构建、ARM交叉构建、PL生成/实现和源码清单核验。完整命令、依赖与实际边界见[构建说明](../docs/build.md)。输出放到新建的包外ASCII目录，避免覆盖证据或源文件。

`reports/`保留真实Vivado资源、时钟、时序、CDC/DRC与既有数值/性能结果。来源、脱敏字段、原件/公开副本哈希见其README及provenance清单；合成与实板输入分别标明。

主入口：

```bash
python3 -B build/scripts/verify_source_manifest.py
bash build/scripts/build-host.sh /tmp/zynq_host_new
bash build/scripts/build-arm.sh /tmp/zynq_arm_new /path/to/matching_sdk/host
```

```powershell
pwsh -File build/scripts/build-pl.ps1 -OutputDirectory E:/zynq_pl_new -VivadoBin D:/tools/Vivado/2025.2/bin
```

模板路径不是本机默认安装位置。部署与物理采集不是这些脚本的隐式动作；平台/SDK不匹配时先完成适配，不运行历史板卡地址。
