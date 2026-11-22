# USERDATA 方案：iOS root 放在 userdata 上（构建 / 刷写 / 恢复 / 日志）

> 本机（Redmi Note 10 5G / camellia / MT6833）**不新建分区**：iOS 的根文件系统是一个
> **16 GiB 的 HFS+ 镜像**，整块写进 Android 的 `userdata` 分区；GPT 不动，`boot_a` 只负责
> 把内核指向它。本文说明这条方案的全部环节，以及破坏性操作的安全边界。
> （2026-11-22 整理，事实来自 `tools/prepare_iosroot.ps1`、`tools/flash_iosroot.ps1`、
> `root/mkiosroot.sh` 与 `15_N2b` / `37_交接全量` 的记录。）

---

## 1. 布局与原则

| 项 | 值 |
|---|---|
| iOS root 容器 | **16 GiB HFS+ 镜像** `iosroot-20H392.img`（来自 iPhone X 16.7.16 / 20H392） |
| 落地位置 | `userdata` 分区（UFS **LUN 2**），整盘覆盖写入 |
| 内核如何找到它 | cmdline：`rd=ufsroot0 ufslun=2 ufsrw=1 rootrw=1` |
| 启动镜像 | `mk_device_boot.py --root ufs --allow-userdata-write ...` |
| **不动的分区** | GPT、`boot_b`、dtbo、super、modem、persist、nvram |
| 会清空的数据 | **Android 的 userdata**（一次性、不可逆） |

原则：
- **只写 `boot_a`** 是日常刷机；写 `userdata` 只有一次（首次部署或重建 root），且必须显式确认。
- 16 GiB 镜像**不进 Git**，用网盘/Release 分发。
- 保留原厂 ROM 作为唯一可靠恢复手段（见 §5）。

---

## 2. 构建 iOS root 镜像（prepare_iosroot.ps1）

`tools/prepare_iosroot.ps1`（Windows + WSL2，全部幂等）：

1. 从 IPSW 里提取 `OS.dmg` / `SystemOS.dmg` / `AppOS.dmg`（`tools/extract_ipsw_dmg.py`，可断点续）；
2. 检查 WSL 发行版有 `apfs-fuse` + `hfsprogs` + 支持 hfsplus 的内核
   （`tools/wsl_setup_ubuntu.ps1`、`tools/wsl_kernel_hfsplus.ps1`）；
3. 把镜像和 `root/mkiosroot.sh` 拷进 WSL 文件系统；
4. 以 root 运行 `mkiosroot.sh`：挂载 APFS → `rsync` 出系统 → 写成 **HFS+** 镜像 → `fsck`；
5. 把 raw 镜像拷回 Windows，打印大小与 `sha256`。

产物：`iosroot-20H392.img`（16 GiB）。**这一步完全不碰手机。**

---

## 3. 刷写（flash_iosroot.ps1，破坏性）

`tools/flash_iosroot.ps1 -YesWipeUserdata`：

```
fastboot flash userdata iosroot-20H392.img   # 大文件按 fastboot -S 分段
fastboot flash boot_a   boot_iosroot.img     # rd=ufsroot0 ufslun=2 ufsrw=1 rootrw=1
fastboot reboot
```

安全设计：
- 不带 `-YesWipeUserdata` 时**直接拒绝执行**；
- 只写 `userdata` 与 `boot_a` 两个分区，**不碰 GPT**；
- 证据（命令输出、哈希）写入 `evidence/`。

---

## 4. 运行期写日志（不动分区）

- boot cmdline 加 `sweetlog=1` → 内核把日志写进 **userdata 尾部**（不新建分区、不动布局）；
- 取出：`root/sweet-log-pull.sh`（WSL/adb），或 recovery 里拉取；
- 目的：UI 模式下没有串口/控制台时，仍能把"一次性启动日志"带回分析
  （**注意**：能常驻输出的状态就做成常驻，别依赖事后捞日志）。

---

## 5. 恢复 / 回退

| 场景 | 做法 |
|---|---|
| 只想回 Android 开机 | `tools/recover_stock.ps1`（刷回原厂 `boot_a`） |
| 彻底恢复（含清 Android 数据） | 刷原厂 ROM：`flash_all_except_data_storage.bat`，再 `fastboot format userdata` |
| 重建 iOS root | 重跑 §2 → §3 |
| iOS 内部崩溃/黑屏 | 长按电源 10–15 s 强制重启；仍黑则音量减+电源进 fastboot |

---

## 6. 已知限制与红线

- **iOS 侧不能充电**：插充电器电压仍下降 → 需要时刷原厂 boot 回 Android 充电；
- 16 GiB userdata 对 iOS 偏小，装大 App 会紧张（可用 `df` 在 iOS 里确认）；
- 写 `userdata` 会清掉 Android 数据；**不要在没备份的情况下做**；
- 任何"改分区/扩分区"的做法都**没有**在本机验证过，不要尝试；
- `boot_b` 永远不写（保留一条恢复路径）。

---

## 7. 相关文件

| 文件 | 作用 |
|---|---|
| `tools/prepare_iosroot.ps1` | 从 IPSW 构建 16 GiB HFS+ iOS root（WSL2） |
| `tools/flash_iosroot.ps1` | 破坏性刷写 userdata + boot_a（需显式确认） |
| `tools/recover_stock.ps1` | 只回退 boot_a 到原厂 |
| `root/mkiosroot.sh` | APFS→HFS+ 镜像构建核心脚本 |
| `root/sweet-root-tweaks.py` | 迁移 / Setup / 容器补丁 |
| `root/sweet-log-pull.sh` | 从 userdata 尾部取日志 |
| `tools/mk_device_boot.py` | 打包启动镜像（`--root ufs --allow-userdata-write`） |
