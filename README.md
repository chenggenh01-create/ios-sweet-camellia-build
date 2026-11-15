# ios-sweet-camellia

Redmi Note 10 5G（**camellia** / MediaTek **MT6833**）上跑 iOS 16.7.16（20H392）用户态的 bring-up 工程。

> **现状（2026-11-15）**：**触摸已打通**（可进桌面、设置、正常滑动）；**SMP 已打通**——
> 8 核（6×A55 + 2×A76）内核全部启动、控制台版完整跑到用户态；
> UI 版稳定核数为 **3 核 @ 60Hz×1200（30fps 上限）**，4 核以上高呈现负载会挂（呈现/定时器并发问题，待修）。
> 主要缺口：**iOS 侧不能充电**（缺 MT6360/MT6359 充电+电量计驱动）、无 GPU（软件渲染）。

## 目标

- **A（主目标）：实验机** —— 尽可能多子系统可用：触控、电源/按键/充电、Wi-Fi、音频、稳定运行。
- **B（条件目标）：日常机** —— 只有 A 稳定后才评估 GPU/Metal、相机、蓝牙、基带通话；
  其中相机/基带目前判断为基本不可达。

路线与验收见 [`docs/33_路线_实验机A_2026-11-14.md`](docs/33_路线_实验机A_2026-11-14.md)。

## 状态

| 里程碑 | 状态 |
|---|---|
| M0 工具链 | ✅ |
| M1 显示 | ✅ |
| M2 XNU | ✅ |
| M3 UFS + iOS root + launchd | ✅ |
| M4 锁屏/桌面 | ✅ |
| M5 触摸 | ✅（2026-11-13 打通，163 基线） |
| A2 电源/充电/按键 | ⛔ 缺 MT6360/MT6359 PMIC 驱动 |
| A3 Wi-Fi / A4 音频 | ⛔ 未开始 |
| A5 性能（SMP/present） | 🟡 SMP 已通（8 核内核可用；UI 稳定 3 核 @30fps 上限）；4 核以上高呈现负载并发问题待修 |

## 仓库内容

```
xnu/patches/        我们改的 XNU 内核补丁（0082–0229）
tools/              打包 / 刷机 / 分析工具
  unpack_super.py     原厂 super.img 拆包（取触摸固件）
  mk_device_boot.py   boot.img 打包（loader + kernel + ADT + cmdline）
  fetch_kernel.py     从 GitHub Actions artifact 拉内核
  gh_commit.py        通过 Git Data API 提交补丁
touch/              触摸相关：驱动快照、NT36672C 固件、厂商参考源码
docs/               构建/刷机/交班文档
  evidence/         上机记录与固件证据
shim/               boot.img 里的 loader 源码
root/               iOS root 构建/调整脚本
```

## 触摸链路（M5，已打通）

- IC：Novatek **NT36672C**（PID 5949，fw 18，18×36，1080×2400）；
- 总线：SPI4 bit-bang（MI=GPIO12、MO=13、CLK=97、CS=98），reset=GPIO15，IRQ=14；
- 事件页 **0x22D00**（trim `0c 00 00 72 66 03` → NT36675 map）；
- 每次开机需下载固件到 SRAM（固件取自原厂 `super.img` 的
  `vendor/firmware/nt36672c_tm_01_ts_fw.bin`，139,264 B）；
- 10ms 轮询 65 字节报文 → 10 指解析 → `sweet_hid_touch()` → SpringBoard。

补丁：`0212`–`0233`；厂商参考源码与固件在 `touch/`。

## SMP 与呈现性能（2026-11-15）

**SMP 已打通**：8 核（6×A55 + 2×A76）内核全部启动，控制台版完整跑到用户态。

关键点：

- 内核实际按 **SWEET 板级配置**编译（`MakeInc.def`：`MACHINE_FLAGS_ARM64_CAMELLIA =
  -DARM64_BOARD_CONFIG_SWEET -DSWEET`），因此 `SweetCPU` 生效；
- CPU 节点必须 `compatible = qcom,kryo470`（匹配 `SweetCPU` 的 `IONameMatch`），
  `cluster-type = 0`（同构）、只有 **cpu0 `state=running`**、其余 `stopped`、`reg = i<<8`；
- ADT 生成器：`tools/mkadt_camellia.py`，`ADT_CPUS=N` 控制 CPU 节点数，
  **多核 ADT 必须带 `--fb1`**（否则 `/sweet-fbcon` 指向错误 buffer，屏幕上没有日志）；
- cmdline 的 `cpus=N` 必须与 ADT 节点数一致。

呈现参数（`SweetFramebuffer`）：

- 全屏帧率上限 ≈ `sweetfb_hz × sweetpresentwindow / 2400`；
- 当前稳定基线：**3 核 + `sweetfb_hz=60` + `sweetpresentwindow=960`**（24fps 上限）；
  1200（30fps）/1600（40fps）手感更好但长时间稳定性不足，按需上调；
- 已知问题：**≥4 核 + 高呈现负载**会卡在苹果标（呈现/定时器并发，待重构修复）。

## 构建链

1. **内核**：上游 XNU + `xnu/patches/` 按文件名字典序应用，GitHub Actions 构建
   `kernel.development.camellia`。
2. **boot.img**：`tools/mk_device_boot.py` 把 kernel 槽（shim loader）、ramdisk 槽
   （SWEETXNU payload：内核+ADT+cmdline）、dtb 槽（原厂 dtb）打包。
3. **iOS root**：`root/mkiosroot.sh` 从 iPhone10,3 16.7.16 IPSW 构建 HFS+ 镜像，
   再用 `root/sweet-root-tweaks.py` 打迁移/Setup/容器补丁。

## 刷机

只写 `boot_a`（iOS root 在 `userdata`）：

```bash
fastboot flash boot boot_iosroot163.img
fastboot reboot
```

## 关键点 / 红线

- 只写 `boot_a`；`boot_b`、dtbo、super、modem、persist 不动；`sweetlog=0`。
- 这台 LK 不支持 `fastboot boot`，必须 `flash boot` 后重启。
- **iOS 侧不能充电**：插充电器电压仍下降；需临时刷原厂 `boot.img` 进
  recovery/Android 充电，充满后刷回 163（详见交班 33）。
- cmdline 关键参数放最前面，长度 < 512；每次构建后必须 `strings` 验证补丁生效
  （`build.sh` 会静默跳过 apply 失败的补丁）。
- 16 GiB userdata root 不适合放 Git；用 Release/网盘单独分发。

## 文档

- [`docs/33_路线_实验机A_2026-11-14.md`](docs/33_路线_实验机A_2026-11-14.md)（路线与验收）
- [`docs/BUILD.md`](docs/BUILD.md)
- [`touch/README_touch.md`](touch/README_touch.md)
- 交接文件（30/31/32/34 等）**只保留在本地**，不随仓库分发。

## 许可 / 来源

内核补丁基于 Apple 开源的 XNU；loader/工具参考
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)。
iOS 用户态（SpringBoard/Settings）是 Apple 闭源二进制，本仓库不分发。
