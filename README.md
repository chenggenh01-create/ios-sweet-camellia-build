# ios-sweet-camellia

Redmi Note 10 5G（**camellia** / MediaTek **MT6833**）上跑 iOS 16.7.16（20H392）用户态的 bring-up 工程。

> **现状（2026-11-22 晚）**：**假 GPU 服务已在真机打通**——内核向 iOS 发布了一个
> `IOAcceleratorES` 服务（`MetalPluginName = SweetMTLDriver`，`gpu_id = 0x90930010`），
> iOS 的 Metal 已能枚举到这块 Mali-G57；但 **Metal 插件尚未实现**，画面仍由 CPU 绘制，
> **系统流畅度目前没有任何提升**。触摸已打通；SMP 已打通（8 核内核可用，UI 稳定 3 核）。
> 主要缺口：**iOS 侧不能充电**（缺 MT6360/MT6359 驱动）、**GPU 驱动插件（下一步）**。

## 目标

- **A（主目标）：实验机** —— 尽可能多子系统可用：触控、电源/按键/充电、Wi-Fi、音频、稳定运行。
- **B（条件目标）：日常机 / GPU** —— 让界面真正由 Mali-G57 渲染。路线与参考实现在
  [`docs/40_GPU方案_参考项目对照与移植计划.md`](docs/40_GPU方案_参考项目对照与移植计划.md)。

## 状态

| 里程碑 | 状态 |
|---|---|
| M0 工具链 | ✅ |
| M1 显示 | ✅ |
| M2 XNU | ✅ |
| M3 UFS + iOS root + launchd | ✅ |
| M4 锁屏/桌面 | ✅ |
| M5 触摸 | ✅ |
| M6 GPU 上电 / JM 提交 | ✅（GC 链 4/4、`state=1f`、后台 job 232/232 成功） |
| **M7 假 GPU 服务发布** | ✅（2026-11-22，`SWEET GPU: service state=1 gpu_id=90930010`） |
| M8 Metal 插件 / 界面 GPU 化 | ⬜ 未开始（见 `docs/41_交接`） |
| A2 电源/充电/按键 | ⛔ 缺 MT6360/MT6359 PMIC 驱动 |
| A3 Wi-Fi / A4 音频 | ⛔ 未开始 |
| A5 性能（SMP/present） | 🟡 SMP 已通；UI 稳定 3 核 @30fps 上限；4 核以上高呈现负载并发问题待修 |

## 仓库内容

```
xnu/patches/        我们改的 XNU 内核补丁（0082–0270）
tools/              打包 / 刷机 / 分析 / 生码工具
  unpack_super.py        原厂 super.img 拆包（取触摸固件）
  mk_device_boot.py      boot.img 打包（loader + kernel + ADT + cmdline）
  fetch_kernel.py        从 GitHub Actions artifact 拉内核
  gh_commit.py           通过 Git Data API 提交（多文件一次提交）
  autopack_watch.py      等构建 → 下内核 → 打启动镜像（一条命令）
  check_patch.py         推 CI 前的补丁体检（hunk 行数/BOM/括号/悬挂类型）
  replay_patches.py      在本地树上重放补丁（校验补丁序列）
  extract_new_files.py   从 unified diff 还原新增文件
  vidscan.py             视频顺序解码抽帧（seek 会重复出帧，别用 vidframe）
  mk_0176_gpu_service.py 生成 GPU 假服务代码块（配合 tools/drafts/gpu_stub_block.txt）
  mk_v5x_*.py            逐版本生成 bsd/dev/sweet_touch_mtk.c 并出补丁
touch/              触摸相关：驱动快照、NT36672C 固件、厂商参考源码
docs/               构建/刷机/交班/GPU 方案文档
  USERDATA_方案.md  iOS root 放 userdata 的构建/刷写/恢复/日志方案（破坏性操作边界）
  evidence/         上机记录与固件证据
  source/           生成的驱动源码快照（参考用，真正来源是 xnu/patches）
shim/               boot.img 里的 loader 源码
root/               iOS root 构建/调整脚本
```

## GPU：这一步是怎么做的（2026-11-22）

iOS 找 GPU 的唯一方式是 `IOServiceMatching("IOAcceleratorES")`，然后读服务上的
`MetalPluginName`（去 `/System/Library/Extensions/<name>.bundle` 找插件）和
`MetalPluginClassName`（插件里必须是 `_MTLDevice` 子类）。所以"冒充"分两步：

1. **内核发布服务**（已完成，补丁 0176 / 0268 / 0269 / 0270）：
   - 在本树里定义 `IOAcceleratorES : IOService`（类名就是 iOS 匹配的关键）；
   - 它的 `start()` 里 `setProperty("MetalPluginName","SweetMTLDriver")`、
     `setProperty("MetalPluginClassName","VMDevice")`，然后 `registerService()`；
   - 挂一个真用户客户端 `SweetGPUUserClient`，实现 `GET_VERSION` / `GET_PARAM`，
     值直接读 Mali 寄存器（`bsd/dev/sweet_touch_mtk.c` 导出 `sweet_gpu_reg_read()`）；
   - 由 `sweet_mali_power()` 在上电成功后主动调用 `sweet_gpu_start()` 发布；
   - 稳定输出块每 10 秒打印 `SWEET GPU: service state=%d gpu_id=%08x`，
     **任何时刻拍照都能看到状态**（一次性日志 0.5 秒就滚走，不要再追）。
2. **用户态插件**（未完成）：`SweetMTLDriver.bundle` + Mesa(Panfrost/PanVK) +
   libdrm 替身，参考同款 G57 已跑通的 `Nik300/viva-gpu` 与 `Nik300/ios-viva`。

真机证据：

```
SWEET GPU: service state=1 gpu_id=90930010
SWEET MALI: BG batches=29 jobs=232 ok=29 bad=0
```

当前镜像：**361 = UI 版 + 常驻 GPU 状态**（`C5B073162A7B36E1`）；
控制台版 **360**（`9f4354370c4a25b9`）。

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
   `kernel.development.camellia`（只有 `xnu/patches/**` 或工作流本身变化才触发构建）。
2. **boot.img**：`tools/mk_device_boot.py` 把 kernel 槽（shim loader）、ramdisk 槽
   （SWEETXNU payload：内核+ADT+cmdline）、dtb 槽（原厂 dtb）打包。
3. **iOS root**：`root/mkiosroot.sh` 从 iPhone10,3 16.7.16 IPSW 构建 HFS+ 镜像，
   再用 `root/sweet-root-tweaks.py` 打迁移/Setup/容器补丁。

## 刷机

只写 `boot_a`（iOS root 在 `userdata`）：

```bash
fastboot flash boot payload/out/n2/boot_iosroot361.img
fastboot reboot
```

## 关键点 / 红线

- 只写 `boot_a`；`boot_b`、dtbo、super、modem、persist 不动；`sweetlog=0`。
- 这台 LK 不支持 `fastboot boot`，必须 `flash boot` 后重启。
- **iOS 侧不能充电**：插充电器电压仍下降；需临时刷原厂 `boot.img` 进
  recovery/Android 充电，充满后刷回（详见交班 33）。
- cmdline 关键参数放最前面，长度 < 512；每次构建后必须 `strings` 验证补丁生效
  （`build.sh` 会静默跳过 apply 失败的补丁）。
- 16 GiB userdata root 不适合放 Git；用 Release/网盘单独分发。
- **推 CI 前先跑 `tools/check_patch.py`**；**任何状态日志默认做成常驻**（每 10 秒重复）。

## 文档

- [`docs/41_交接_2026-11-22_GPU假服务打通与下一步.md`](docs/41_交接_2026-11-22_GPU假服务打通与下一步.md)（**最新**：GPU 假服务成果 + 下一步 4 个部件 + 踩坑）
- [`docs/USERDATA_方案.md`](docs/USERDATA_方案.md)（iOS root 放 userdata：构建/刷写/恢复/日志）
- [`docs/40_GPU方案_参考项目对照与移植计划.md`](docs/40_GPU方案_参考项目对照与移植计划.md)（参考项目对照：viva / io-s-viva / hakios / HelioDarwin / Ken-u）
- [`docs/33_路线_实验机A_2026-11-14.md`](docs/33_路线_实验机A_2026-11-14.md)（路线与验收）
- [`docs/BUILD.md`](docs/BUILD.md)
- [`touch/README_touch.md`](touch/README_touch.md)

## 许可 / 来源

内核补丁基于 Apple 开源的 XNU；loader/工具参考
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)。
iOS 用户态（SpringBoard/Settings）是 Apple 闭源二进制，本仓库不分发。
GPU 方案参考（**仅为链接，未随仓库分发**）：
[Nik300/viva-gpu](https://github.com/Nik300/viva-gpu)、
[Nik300/ios-viva](https://github.com/Nik300/ios-viva)、
[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)、
[squeeeezy/HelioDarwin](https://github.com/squeeeezy/HelioDarwin)、
[Ken-u/MaliMetal](https://github.com/Ken-u/MaliMetal)。
