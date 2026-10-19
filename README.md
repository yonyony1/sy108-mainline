# sy108-mainline

面向 Soyealink SY108 5G通信壳（紫光展锐 UDX710 SoC）的 OpenWrt 平台集成与用户态服务仓库，提供主系统、独立 RAM Recovery、原生 CP/NV 基带服务、蜂窝网络接入及 AT/短信管理支持。

项目由三个仓库组成：
- `sy108-mainline`：用户态服务、OpenWrt target/feed 集成与构建脚本
- `linux-udx710`：UDX710/SY108 内核驱动、设备树与配置
- `sy108-uboot-spl`：SPL 与 U-Boot 启动器源码及构建环境

OpenWrt 源码由构建脚本从官方固定提交拉取并应用板级补丁。

## 构建

依赖环境：Git、Python 3.12+、Docker（需支持运行 `linux/arm64` 容器）。可通过 `SY108_PYTHON` 指定 Python 解释器，通过 `SY108_DOCKER_CONTEXT` 指定 Docker context。

```sh
export SY108_PYTHON=/path/to/python
tools/python tools/bootstrap.py
tools/python tools/build/firmware.py main
tools/python tools/build/firmware.py recovery
```

内核源码优先自动使用同级目录 `../linux-udx710`（或通过 `SY108_LINUX` 指定路径）；若本地不存在，则由 `bootstrap.py` 自动从 `manifests/kernel.json` 中的仓库地址按锁定提交浅拉取至 `out/linux` 并生成源码归档与校验和。

默认构建输出位于 `out/`，主系统与 Recovery 共用 `out/openwrt-build` 构建树（需串行执行）。可通过环境变量调整路径与容器资源：
- `SY108_OUT`：构建输出目录
- `SY108_DOWNLOADS`：OpenWrt 源码包下载缓存目录
- `SY108_JOBS` / `SY108_MEMORY`：构建容器 CPU 核数与内存上限（默认 6 核、10 GiB）

构建产物位于 `out/images/main` 与 `out/images/recovery`：
- `out/images/main/sysupgrade.bin`：主系统 `sysupgrade` 升级镜像
- `out/images/recovery/boot.bin`：独立 Recovery Capsule 镜像（对应 `recovery` UBI 卷）


U-Boot 与 SPL 在 `sy108-uboot-spl` 仓库中独立构建，详见 [构建维护](docs/build.md#独立启动器)。

## 功能概述

- **Web 管理（LuCI）**：UDX710 菜单提供设备状态监控、蜂窝网络管理（运营商扫描与选网、NR/LTE 频段/频点/小区锁定、APN 模板、IMS/VoLTE）、短信收发、硬件与 CPU/DDR 调频管理、AT 命令终端与 IMEI 管理，以及主系统/Recovery 启动目标切换。
- **蜂窝数据平面**：基于原生 `netifd` 协议扩展与官方 DHCPv6 客户端管理 IPv4/IPv6 WAN 接入。
- **USB 网络管理**：主系统与 Recovery 均在 USB1（`0x29100000`）上提供 CDC-NCM 管理网口（默认管理地址 `192.168.66.1/24`，初始登录凭据 `root` / `admin`）。主系统默认启用 PAM U3 硬件加速，Recovery 使用软件 NCM。

## 现状、已知限制与待办

- **USB 与协议支持**：
  - 粗略优化了 USB DWC3 的 PAM U3 硬件卸载加速。
  - USB 3.0 SuperSpeed 链路偶发不稳，目前尚不确定是测试线材/转接头问题还是 PHY/链路参数仍需微调。
  - USB 网络目前仅做了 CDC-NCM 支持；RNDIS 与 CDC-ECM 因个人无使用需求暂未适配。
  - USB Host 模式暂未测试。
- **蜂窝与基带功能**：
  - 目前仅使用中国联通 SIM 卡完成基础测试，其余运营商尚未实测。
  - 基带独立软复位与 SIM 卡热插拔重识别目前尚存在问题，待后续排查解决（前端基带复位按钮暂置灰保留）。
  - 运营商扫描/手动选网、NR/LTE 频段与频点/小区锁定、IMS/VoLTE 及 IMEI 修改等基带高级功能仅完成代码接入与基础验证，完整实网测试尚待进行。
  - 受限于测试环境信号较弱，尚未开展极限蜂窝吞吐性能测试。
  - 语音 DSP（AGDSP / 语音通路）仍有问题，待后续调试。
- **Web 管理前端（LuCI）**：
  - 当前前端虽已切换至原生 LuCI 表单框架，但各页面的功能划分、状态联动与交互逻辑仍较为杂乱，待后续重新梳理与优化。
- **外设与多机型支持**：
  - 由于 Soyealink SY108 5G通信壳未引出 eMMC、SDIO 与 PCIe 外设，UDX710 的相关控制器驱动未做实机测试。
  - 目前仅基于手头这一台 SY108 硬件完成适配与验证，同款其他批次硬件及其他 UDX710 机型仍有待进一步测试与支持。
- **启动回退与刷机工具文档**：
  - 为方便日常内核与用户态调试，U-Boot、内核与用户态之间实现了一套基于 `misc` 分区状态与 RTC 寄存器的试启动/看门狗超时自动回退 Recovery 机制，感兴趣可自行结合源码分析，详细文档后续补全。
  - 初次刷入本固件需刷入配套修改后的 U-Boot（`sy108-uboot-spl`）并对原厂 UBI 卷分区做微调，具体引导逻辑与卷结构均可自行结合源码分析；配套的刷写工具与详细刷机文档待后续整理补全。

## 文档索引

- [架构与接口](docs/architecture.md)
- [构建维护](docs/build.md)
- [USB、MCU 与 CC/PD 硬件参考](docs/usb-mcu-cc-findings.md)

## 致谢

- [1orz/project-cpe](https://github.com/1orz/project-cpe) — UDX710 5G CPE 后台管理与调制解调器控制研究（AT 指令、NR/LTE 频段锁定与网络状态接口参考）
- [LeoChen-CoreMind/UDX710-TOOLS](https://github.com/LeoChen-CoreMind/UDX710-TOOLS) — 紫光展锐 UDX710 平台 Web 管理界面与设备功能控制参考
- [TheGammaSqueeze/UnisocBypass](https://github.com/TheGammaSqueeze/UnisocBypass) & [LeoChen-CoreMind/spd_flasher](https://github.com/leochen-coremind/spd_flasher) — 紫光展锐安全启动绕过、DHTB 签名结构分析与 BootROM 自动刷机恢复工具
- [sprd-linux/ud710_bsp](https://github.com/sprd-linux/ud710_bsp) — 紫光展锐 UDX710 开源 BSP 参考基线
- [OpenWrt](https://openwrt.org/)、[The Linux Kernel](https://www.kernel.org/) 与 [Das U-Boot](https://docs.u-boot.org/) 开源社区

---

*Powered by GPT 6 Astra & Gemini 4 Argon*

