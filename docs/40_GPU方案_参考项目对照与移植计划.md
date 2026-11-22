# 40 · GPU 方案：参考项目对照与移植计划（2026-11-22）

> 目的：确定"让 GPU 真正跑起来且被系统使用"的可行路线。本文是**多轮对照**的结果，
> 所有结论都指到具体仓库/文件/补丁号，不是猜测。

---

## 1. 四个相关项目的对照（全部公开）

| 项目 | SoC | GPU | iOS 用户态 | GPU 方案 | 真实状态 |
|---|---|---|---|---|---|
| **ios-sweet**（我们上游） | SD732G | Adreno 618 | iOS 16.7.16 | 无 | 明确**不用 GPU**，全 CPU |
| **UniXNU-camellia**（我们的 SoC） | MT6833 | **Mali-G57 MC2** | iPadOS 17.7.11 | 无 | GPU 列 incomplete/**禁用** |
| **hakios**（mt6768） | MT6768 | Mali-G52（Bifrost） | iOS 16.5 | Mesa panfrost(GL) + 自写 Metal(AIR→NIR) | **GL 路径干净可用**；Metal 能跑但画面有错（拼条/空控件/半屏暗），作者仍在修 |
| **HelioDarwin**（MT6789） | Helio G99 | **Mali-G57 MP2** | **iOS 16.7.16 20H392** | **MaliMetal**（Metal 实现）+ **arm64 iOS 的 PanVK 移植** + 自定内核 ABI | GPU 上电/Vulkan 设备创建成功；**原生 JM 完成 10 次（含 fragment job）**；像素正确性与性能"尚未验证" |
| UniXNU（Adreno） | SM8550 | Adreno 740 | iOS/iPadOS | metal2vulkan + Turnip + NativeMetal | Metal 部分可用（非 Mali，仅作架构参考） |

### 结论：**HelioDarwin 是第一参考**
理由：
1. **GPU ID 完全相同：`0x90930010`（Mali-G57 / Valhall v9）**
2. **同一套 iOS 16.7.16 (20H392) iPhone X 用户态**（我们也是）
3. 同为 MediaTek、同为 XNU 8796、同为 UFS + HFS+ 根
4. 它把 **Metal→Vulkan→PanVK→内核 ABI** 全链路做出来并公开了源码与 212 个内核补丁

hakios 作为**第二参考**（GL/EGL 路径最成熟，可直接照搬 Mesa 构建与注入方式）。

---

## 2. HelioDarwin 的架构（我们要照搬的那套）

```
iOS 应用/CA (Metal)
   │
   ├─ MaliMetal/  (ObjC++)：Metal 设备/队列/资源/命令缓冲 实现
   │      └─ MaliMetalVulkan.c + arm64 iOS 版 PanVK（Mesa Vulkan 后端）
   │
   ├─ 内核 ABI：IOAcceleratorES 服务，见 MaliMetal/MaliMetalKernelABI.h
   │      8 个 selector：QUERY / BO_ALLOC / BO_IMPORT / BO_RELEASE /
   │                     SUBMIT / WAIT / BO_SYNC / CLOCK
   │      SUBMIT: job_chain(GPU 地址) + handles[] + slot(0=frag,1=vert/tiler/compute)
   │
   └─ 内核：sweetmali 类 user client（补丁 0173/0189/0195）+ Mali JM 提交
```

关键 ABI 文件（已存档）：`evidence/research/heliodarwin/MaliMetal_MaliMetalKernelABI.h`

要点：
- `MMK_SERVICE_NAME = "IOAcceleratorES"` ← iOS 的 Metal 按这个名字找设备
- `MMK_GPU_PAGE_SIZE 4096`，GPU 虚拟地址 64 位
- **一次只允许一个活跃提交**（v1）
- BO 用 `handle` 表管理，`MMK_MAX_BOS 256`
- WAIT 返回 job 状态、`JS_STATUS` 原始异常、AS fault 地址 —— 诊断信息完整

## 3. 他们的内核补丁清单（与 GPU 直接相关的）

| 补丁 | 作用 |
|---|---|
| 0138 | `topaz-empty-ioacceleratores` — 发布空的 IOAcceleratorES（Metal 能找到设备） |
| 0147 | mali GPU 上电 + metal accelerator |
| 0148 | MFG top 电源域 + **Mali-G57 ID** 识别 |
| 0149 | TOPCKGEN GPU 时钟 + VM trace |
| 0173 | 注册 **sweetmali** user client |
| 0176 | 发布 **malimetal properties** |
| 0189 | **malimetal JM GPU user client**（就是我们需要的那个 ABI 服务） |
| 0195 | GPU connection service |
| 0204 | Mali 分配失败诊断 |
| 0208/0209/0210/0212 | 时钟校验 / job 地址空间租约 / 时钟拒绝阶段 / clock-counter 读取 |
| 0196 | arm64 cache 页地址宽度（cache 维护正确性） |

## 4. 我们的移植计划（按顺序）

### 阶段 A：内核侧 ABI（1–2 天）
1. 在我们内核里增加 `IOAcceleratorES` 服务（照 0138/0189 的结构），实现 `MaliMetalKernelABI.h`
   的 8 个 selector，落地到我们**已验证的 v9 核心**：
   - QUERY：返回真实参数（GPU_ID `0x90930010`、MMU/JS features、页大小）
   - BO_ALLOC/BO_IMPORT/RELEASE：kmem 分配 + **我们的 GPU 页表映射**
   - SUBMIT：**我们已跑通的 JS1 job 提交**（含 job 链，v44 已 4/4 验证）
   - WAIT：完成位轮询 + fault 状态回读
   - BO_SYNC：`FlushPoC_DcacheRegion` / invalidate
2. 发布属性：`MetalPluginName` / `MetalPluginClassName`（让 iOS 加载 MaliMetal.bundle）

### 阶段 B：用户态（2–4 天）
3. 把 `MaliMetal/` 整套源码 + 其依赖（AIR 编译器、PanVK）编译成 **iOS arm64 产物**
   - 需要一个 macOS CI（我们已有）用 iPhoneOS SDK；HelioDarwin 自己也在 GitHub Actions 里编
4. 打包成 `/System/Library/Extensions/MaliMetal.bundle`

### 阶段 C：注入与验证（2–3 天）
5. 用他们的 system-services 方式让 backboardd/SpringBoard 加载该 bundle
   （内核侧 exec 注入；hakios 用 `kern.exec_inject`，HelioDarwin 有 tools/ 适配器）
6. 上机：先验证 `metal_props` 能拿到设备 → 再验证 CA 出第一帧 → 再谈正确性

### 阶段 D：正确性/性能（把他们的未完成项做完）
7. 他们的"未验证/未完成"：正确的 UI 像素、性能、完整 DVFS。这正是我们能贡献的部分。

## 5. 现在立刻能做的事

- [x] ABI 头文件、MaliMetal 源码、内核补丁清单、文档全部存档到 `evidence/research/`
- [ ] 从 HelioDarwin 拉取 0138/0189/0195 三个补丁，读我们的内核要改哪几处
- [ ] 用我们已验证的 v9 提交/页表代码，实现 `IOAcceleratorES` 的 8 个 selector
- [ ] 在 CI 里试编 MaliMetal（先只编 `MaliMetalKernelABI.h` 的测试桩）

## 6. 风险与边界（必须记住）

- **他们的 Metal 也没到"像素正确"**：我们不能假设搬过来就完美，要留出调试时间
- 他们的设备是 MT6789，**PMIC/时钟/电源寄存器与 MT6833 不同** —— 电源部分必须用我们
  自己已验证的 MT6833 序列（MFG0–3、mfgcfg、VPROC1/VSRAM_MD），不能照抄
- 授权：两个项目都有 LICENSE，个人移植可用，**不要闭源发布**

---

## 7. 参考文件索引（已下载）

| 文件 | 内容 |
|---|---|
| `evidence/research/heliodarwin/README.md` | 硬件状态表（GPU/MaliMetal 真实进度） |
| `evidence/research/heliodarwin/MaliMetal_MaliMetalKernelABI.h` | **内核↔用户态 ABI（核心）** |
| `evidence/research/heliodarwin/wiki_MaliMetal-Driver.md` | MaliMetal 驱动设计文档（28KB） |
| `evidence/research/heliodarwin/wiki_MT6789-GPU-Power-Sequence.md` | MT6789 GPU 上电序列（对照我们 MT6833） |
| `evidence/research/hakios/docs_metal.md` | hakios 的 Metal 可行性分析（AIR/NIR/IR3） |
| `evidence/research/hakios/docs_metal-x6826b-plan.md` | hakios Metal 收敛计划与 run 记录 |
| `evidence/research/hakios/RedwoodMali.cpp` / `RedwoodGPU.cpp` | hakios 内核 GPU 服务（panfrost ABI） |
| `evidence/research/hakios/panfrost-darwin.patch` | Mesa 的 Darwin panfrost 补丁 |
| `evidence/research/hakios/tools_mk-mesa.sh` | Mesa iOS 构建脚本（可复用） |

---

## 8. 第二轮全网搜索（2026-11-22 晚）——找到"同 GPU 已跑通 Metal"的项目

### 8.1 决定性发现：`Nik300/viva-gpu`

- 仓库：https://github.com/Nik300/viva-gpu
- 目标机：Redmi Note 11 Pro 4G（`viva`，MediaTek **Helio G96 / MT6781**），GPU = **Mali-G57 MC2（与 MT6833 完全相同）**
- 内核侧：`Nik300/ios-viva` 的 `xnu/patches/0100+`（`0100-viva-gpu-mali.patch` 等，共 191 个补丁中的 20+ 个 GPU 相关）
- **README 原话**：*"The driver runs on the phone and renders the whole UI. The home screen, Control Center, Settings, Safari, Sileo and third-party apps all draw on the Mali, at 60 or 120 Hz with direct scanout. The UI gets about 37 fps in the heaviest home-screen animation, where the G57's fill rate is the limit."*

架构（与我们 40 号文档第 2 节的设想完全一致，但它做完了）：

| 层 | 实现 |
|---|---|
| Metal 发现 | 内核发布 `VivaGPUAccelerator`（`IOAcceleratorES` 子类）+ `MetalPluginName=VivaMTLDriver`、`MetalPluginClassName=VMDevice` |
| Metal 前端 | `driver/metal/*.m`：`VMDevice`/`VMCommandBuffer`/`VMRenderEncoder`… 子类化 `_MTLDevice` 等私有基类，报告 **Apple3** 特性档 |
| 后端 | `driver/vmg_*.c`（纯 C，资源/IOSurface 纹理/pipeline/命令重放） |
| 着色器 | ① AIR→NIR→Panfrost（完整路径）；② **自研 AIR→Valhall ISA 编译器**（`driver/isa/`，含 366 条 v9 指令编码表） |
| Mesa | 26.2.6，Gallium Panfrost，**静态链接进 iOS**，6 个小补丁适配 XNU |
| libdrm 替身 | `libdrm-xnu/`：Panfrost 的 ioctl → 内核 selector，同结构体；IOSurface 平面冒充 dma-buf；同步对象快照替代 sync file；**批量提交 + 用共享完成页回答 WAIT_BO（不进内核）** |
| 内核 ABI | `uapi/viva_gpu_uapi.h`：**23 个 selector**，模仿 `DRM_PANFROST_*`，另有 `IMPORT_IOSURFACE`/`SUBMIT_BATCH`/`SYNCOBJ_TRANSFER`/`SURFACE_PROMISE` |
| 内核驱动 | `VivaGPUAccelerator` + `VivaGPUUserClient`，Valhall **JM** 驱动（正是我们需要的形态） |

### 8.2 其他新增来源（第二轮）

| 项目 | 平台/GPU | 价值 |
|---|---|---|
| `Ken-u/MaliMetal` | RK3588 / Mali-G610（CSF） | 完整构建脚本（Mesa/PanVK iOS 交叉编译、darwin-drm、metal2vulkan、插件打包）、**中文开发笔记** `docs/notes.zh-CN.md` |
| `Ken-u/Mali` | 同上 | 内核 IOKit user client + `mali_uapi.h`（多项目共用 ABI） |
| `squeeeezy/HelioDarwin` | MT6789 / G57 JM | `MaliMetal/` + **JM 版内核 ABI**（`MaliMetalKernelABI.h`，8 selector），GPU 上电/Vulkan 设备创建/JM 完成已验证 |
| `oosexgang/hakios-xnu-kernel`（mtk） | MT6768 / G52 | panfrost(GL) 干净 + Metal 有 bug；Mesa `panfrost-darwin.patch` + `redwood-drm` |
| `dvg987/twilight-xnu` | Mi Box Gen3 / Mali-G310 tvOS | 跨平台构建配方（metal2vulkan + PanVK + Ken 插件），符号检查脚本 |
| `MineGraph-bilibili/panvk-JM-kirin` | 麒麟 / Mali v6-v7-v9 | **PanVK JM 后端**（Mesa 27.0-devel，kbase UAPI 11.x） |
| `Dracen-W/Mali-G52` | MT6768/6769 / G52 | 原生 XNU Mali 驱动（GPL-2.0），ABI 形状仿 Adreno740 |
| `Fakani/ios16-on-mt6833` | **MT6833（我们同 SoC）** | 中文真机移植记录：显示/触摸/按键可用，v83 优化流畅度的实验文档 |
| `lib-fluff-so/KLiOS716` | MT6789 | XNU 补丁 + iOS rootfs 工具链 |
| `woaphone/UniXNU-mt6789-common` | MT6789 | 平台公共层（WIP） |

### 8.3 修正后的执行策略

1. **以 `viva-gpu` 为蓝本**：用户态（Metal 插件 + Mesa iOS + libdrm-xnu + UAPI）几乎可整包复用
2. **内核侧**：移植 `ios-viva` 的 GPU 补丁（0100+）到我们的 XNU 8796 树，电源/时钟换成我们已验证的 MT6833 序列
3. **显示**：viva 用 direct scanout；我们需要把它接到现有 SweetFramebuffer/present 路径
4. **验收标准**（viva 已给出量化基准）：最难的主屏动画 ≥ **37fps**（G57 填充率上限），60/120Hz 直扫

> 注意授权：`viva-gpu` 有自己的 LICENSE，`Dracen-W/Mali-G52` 是 GPL-2.0；个人移植可用，**不要闭源分发**。

---

## 9. 第三轮检索（2026-11-22 深夜）——完整参考清单

### 9.1 直接蓝本（同 GPU：Mali-G57 MC2）

| 仓库 | 内容 |
|---|---|
| `Nik300/ios-viva` | XNU 侧移植（MT6781 viva），内核补丁 0100+ 就是 **Valhall JM GPU 驱动**（`0100-viva-gpu-mali`、内存per进程、惰性cache维护、隐式同步、提交批处理/完成页、DVFS…） |
| `tphx37/ios-viva` | 活跃 fork（2026-11-21 更新），README 明确：*"Graphics are hardware accelerated. Metal, and with it CoreAnimation, SpringBoard, Safari and the apps, runs on the phone's Mali-G57 MC2"* |
| `Nik300/viva-gpu` | 用户态 Metal 驱动（G57 MC2）：Metal 插件 + vmg 后端 + AIR→NIR/AIR→ISA + Mesa 26.2.6 静态 iOS 版 + libdrm-xnu + UAPI(23 selectors) |

### 9.2 其他 Mali/XNU 实现（不同 SoC，但同思想）

| 仓库 | SoC/GPU | 状态/价值 |
|---|---|---|
| `Ken-u/MaliMetal` + `Ken-u/Mali` | RK3588 / Mali-G610 (CSF) | 完整构建脚本 + 中文笔记；CSF 路线 |
| `Ken-u/UniXNU-rk3588-common` | RK3588 | UniXNU 平台层 |
| `squeeeezy/HelioDarwin` | MT6789 / G57 (JM) | MaliMetal + JM 内核 ABI(`MaliMetalKernelABI.h`)，GPU 上电/Vulkan/10 次 JM 完成 |
| `oosexgang/hakios-xnu-kernel`(mtk) | MT6768 / G52 | panfrost(GL) 干净 + Metal 有 bug；`panfrost-darwin.patch`、`redwood-drm` |
| `Dracen-W/Mali-G52` | MT6768/69 / G52 | 原生 XNU Mali 驱动（GPL-2.0），`NativeMaliABI.h` |
| `Dracen-W/UniXNU-merlinx` | MT6768 | UniXNU 设备移植 |
| `Luminous418/hakios-a32` | MT6769T / G52 | Panfrost/GL ES 路径 |
| `dvg987/twilight-xnu` | Mi Box Gen3 / Mali-G310 | tvOS 交叉构建配方（metal2vulkan+PanVK+Ken 插件） |
| `MineGraph-bilibili/panvk-JM-kirin` | 麒麟 / Mali v6-v9 | **PanVK JM 后端**（Mesa 27） |
| `leegao/valhall2vulkan` | Valhall (CSF) | 把 (CSF 寄存器/GPU 内存/ISA) 编译成 SPIR-V+管线，用于 libmali_kbase 仿真 |

### 9.3 其它天玑 iOS 移植（GPU 状态）

| 仓库/页面 | SoC | GPU 状态 |
|---|---|---|
| `Fakani/ios16-on-mt6833` | **MT6833（同我们）** | 桌面/显示/触屏/按键可用，中文记录，v83 优化流畅度；GPU 未提 |
| `lizi600jin/hakios-xaga` | MT6895 / G610 | CSF 固件装好、页表建好、MCU 不执行；软件渲染 3–14 fps |
| `sheikhmehraann/X6871-iOS-Port` | MT6895 / G610 | 有 `docs/GPU-PORTING.md`：G610 分析与**七阶段验证法**（我们直接可用） |
| `VibeCoder-sudo/iMediaTek` | MT6785 / G76 | PoC 安装器，含 "Metal on Mali (experimental)" 选项 |
| `lib-fluff-so/KLiOS716` | MT6789 | XNU 补丁 + iOS rootfs 工具 |
| `woaphone/UniXNU-mt6789-common` | MT6789 | 平台公共层（WIP） |
| `lky005/ruby-ios-bringup` | MT6877 | GPU **未实现**（显示/IOSurface 管线已验证，可参考） |
| UniXNU `camellia`/`malachite` | MT6833 / MT6878 | GPU **planned/未实现** |

### 9.4 七阶段验证法（来自 X6871 文档，直接采用）

1. GPU 上电（MTCMOS MFG + VGPU 稳压）
2. 寄存器访问（`GPU_ID` 读到有效 Valhall ID）
3. 内存分配（AS0..AS3 页表映射）
4. 命令提交（job descriptor 写入 job slot）
5. 硬件 job 完成（IRQ + Job Done 位置位）
6. **正确像素**（framebuffer 读回与测试图案一致）
7. **CoreAnimation 呈现**（backboardd/SpringBoard 经 Metal 出画）

我们当前进度：1–5 基本完成（`state=1f`、4/4 链、10+ JM 完成），**6–7 未做**。

### 9.5 最终执行方案（确定）

1. **内核**：以 `Nik300/ios-viva` 的 `xnu/patches/0100+` 为基础移植到我们 XNU 8796 树；
   电源/时钟替换为我们已验证的 MT6833 MFG/SPM/mfgcfg 序列
2. **用户态**：整包复用 `Nik300/viva-gpu`（Metal 插件 + Mesa iOS 构建 + libdrm-xnu + UAPI）
3. **显示**：把 viva 的 direct scanout 接到我们 SweetFramebuffer/present
4. **验收**：按七阶段推进到 6、7；性能基准参考 viva：**最难主屏动画 ≥37fps**、UI 直扫 60/120Hz
