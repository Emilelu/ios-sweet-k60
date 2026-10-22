# HackiOS on Redmi K60 (mondrian, SM8475) — 移植状态

> 生成时间：2026-10-21。目标机：Redmi K60 `mondrian` / 型号 23013RK75C / 骁龙 8+ Gen 1 (SM8475, "cape"/taro 家族)
> 序列号 `ffec9d48`，UFS 3.1，2K 1440×3200 AMOLED，Android 16 官改/移植 ROM，bootloader 已解锁。

## 一、结论先说

| 阶段 | 状态 |
|---|---|
| bootloader 解锁 + `fastboot boot`（不写分区起镜像） | ✅ 已验证 |
| shim 在真机执行（裸机、MMU 关、IMEM/PSCI 可用） | ✅ 已验证（IMEM 信标铁证） |
| 硬件图提取（GIC/UFS/面板/触摸/RAM/IMEM） | ✅ 完成 |
| shim 本机编译（无 macOS） | ✅ NDK clang + ld.lld |
| 屏幕输出 | ❌ **硬阻塞**：ABL 交接时关掉面板 |
| XNU 内核 | ⛔ 未开始：必须在 macOS 工具链编（GitHub Actions macOS runner） |

**一句话**：K60 上"能起我们自己的裸机代码"这件事已经打通，但"能在屏幕上看到东西"必须先写显示驱动；
而 iOS 那条链（XNU）本地编不了，需要 macOS CI。

## 二、黑屏的定性过程（不是猜的）

1. shim 第一版（45s 停留 + 全屏刷红）：用户观察 = 黑屏。
2. IMEM 信标实验：shim 写 `0x77665500` 到 `0x146aa65c` 再 PSCI 复位 → 手机停在 fastboot。
   ⇒ 证明 ABL 确实跳进了我们的 Image，代码在跑。
3. `/chosen` 节点 **没有** cont-splash 交接属性；Android dmesg **没有** "Continuous splash enabled"，且 DSI 在 2.7s 才从头初始化。
4. 最终实验（不碰帧缓冲、只停 25 秒）：用户观察序列 = logo → fastboot → **黑屏** → logo → 开机动画 → 安卓。
   ⇒ **ABL 跳进内核那一刻就把面板关了**，与我们的缓冲区地址无关。

**推论**：
- 刷 `boot_a` 走"正常启动"也不会保留面板（第 3 条证明正常启动同样关闭）→ 省掉这次写分区风险。
- `splash` 分区 32MB 全零，开机 logo 不在分区里 → "改 logo 分区显示 XTOM" 也不可行。
- 想要屏幕只能自己点 DSI/DPU；且面板是 **DSC command-mode AMOLED**（`mdss_dsi_m11a_42_02_0a_dsc_cmd`），是最难的一档。

## 三、已提取的硬件常量（来源：设备 live FDT + /proc/iomem + GPT）

| 项 | 值 |
|---|---|
| compatible | `qcom,cape-mtp` / `qcom,cape`；model "Mondrian based on ... SM8475" |
| GICD / GICR | `0x17100000` / `0x17180000`（stride `0x40000`，GICv3） |
| timer | 19.2 MHz，虚拟定时器 GIC_PPI 11 → **INTID 27**；物理 PPI 14 → 30 |
| UFS host | `0x1d84000`（`ufs_mem`，另 `ufs_ice 0x1d88000`、`0x1d90000`）；逻辑块 **4096**；LUN0 = `sda` |
| splash 保留区 | `0xb8000000`，43MB（`cont_splash_region`）—— 实测不扫描输出 |
| ramoops | `0xa7000000`，4MB（console 2MB + pmsg 2MB）；`/dev/pmsg0` 读写报 EINVAL，不可用 |
| IMEM restart_reason | `0x146aa65c`（`qcom,msm-imem@146aa000 + 0x65c`） |
| PSHOLD | **不存在**（SM8475 走 PMIC PON/SPMI），复位只能用 PSCI_SYSTEM_RESET |
| CPU | 8 核 `cpu@0..700`（MPIDR 0x000–0x700），X2 + 3×A710 + 4×A510 |
| RAM 高窗 | `0x840000000` 起连续 9GB 可用（`0x840000000–0xa7fffffff`）；低窗 `0x80000000+1.66GB`、`0xf4600000+186MB` |
| 面板 | `qcom,mdss_dsi_m11a_42_02_0a_dsc_cmd`，1440×3200（0x5a0 × 0xc80），DSC cmd mode |
| 触摸 | `xiaomi_touch@0` on **SPI** `spi@990000`，compatible `xiaomi,m11a-spi`，Goodix m11a 固件；IRQ GPIO 0x15、reset 0x14、AVDD 0x47 + `vdd-supply` |
| 调试 UART | `qcom,qup_uart@99c000`（cmdline `console=ttyMSM0,115200n8`），物理未引出 |

## 四、产物与位置

| 路径 | 内容 |
|---|---|
| `~/hackios/ios-sweet/` | 上游仓库（本地 clone）+ 本次 K60 改动（commit `155a981`） |
| `~/hackios/ios-sweet/shim/` | 改好的 shim：板级常量、XTOM 横幅、klibc、UFS 写入/日志、bring-up 探针 |
| `~/hackios/dump/live.dtb` / `live.dts` | 真机 live FDT 及其反编译（1.7MB，全部地址来源） |
| `~/hackios/dump/iomem.txt` | `/proc/iomem` 全量 |
| `~/hackios/dump/boot_a.img` | **boot_a 原始镜像备份**（192MB，md5 `9d20ec5ed698263d1a8ff35efebcf795`）——回滚用 |
| `~/hackios/dump/gpt.bin` | 前 34 个 LBA（非整盘 GPT，仅供调试） |
| `~/hackios/dtc/` | 从源码编的 dtc |
| `~/hackios/run-fast.sh` | 快循环：shim 复位直回 fastboot，只测时长 |
| `~/hackios/.cache/hackios/` | 上游文档/网页抓取（how-it-works、兼容性表、分片 fork 清单等） |

## 五、shim 的编译要点（踩过的坑，别重复）

1. **没有 aarch64-linux-gnu-gcc**：用 NDK `25.2.9519653` 的 clang + `ld.lld`，
   `--target=aarch64-linux-gnu`。
2. **`-fno-jump-tables` 必加**：clang 会把 panic 的 `switch` 编成字符串指针跳转表，
   产生 3 个 ABS64 重定位，破坏"位置无关"守门检查（`ABS64 == 0`）。
3. **缺 libc**：clang 不内联 `__builtin_strcmp`，需要自带 `klibc.c`
   （`strcmp`/`memset`/`memcpy`，用 `-fno-builtin` 单独编）。
4. `mkbootimg` 用 `--header_version 2` 可用；DTB 用真机 `live.dtb`。
5. `build.sh` 支持 `CFLAGS_EXTRA`、`DTB`、`HDRVER`、`CMDLINE` 覆盖。

## 六、UFS 日志通道（进行中）

- 现状：**读通路已验证**（GPT 按名字查 `opcust`、读回 Android 埋的 `HOSTPROBE` 字符串都对）；
  **写通路不落盘**（WRITE(10) 按上游 XNU 驱动的编码：UPIU flags `0x20` + UTRD DD=1，
  四种组合都试过；写后发 SYNCHRONIZE CACHE(10)；介质本身可从 Android 正常写）。
- 已加：`REG_IE=0`、清 `REG_IS`、清 ABL 遗留的 UTRLDBR 门铃（对齐 XNU 驱动的接管序列）。
- 待查：写命令的 SCSI 状态码（`scsi()` 返回 1=超时 / 2=OCS / 3=响应异常 / 4=SCSI status），
  用"停留时长编码状态码"的快循环取，无需启动 Android。
- 备选通道：EDL(9008) 读物理内存（需要 SM8475 的 firehose loader）；
  `/dev/mem`、pmsg、cache 分区均不可用（`CONFIG_DEVMEM` 未开、A/B 无 cache）。

## 七、下一步（按性价比排序）

1. **把 UFS 写通路修好**（读已通，差最后一步）→ 从此每步实验都有真实日志，
   而不是靠时序编码。`scsi()` 已返回分阶段状态码，快循环一轮 ~15s。
2. **XNU 内核**：必须在 GitHub Actions 的 macOS runner 上编
   （`xnu.yml` + `xnu/patches/`，见上游 README）。需要：GitHub 账号 + 一个空仓库 + `gh` 登录。
   注意：K60 需要新的板级补丁（cape 的 PMIC/SPMI 总线、UFS 参数、ADT 常量），
   sweet 的 80 个补丁只能作模板。
3. **显示驱动**（决定"能不能真的看到 iOS"）：
   最小可行路径 = 抄 bootloader/LK 的做法，让 DSI 控制器进 command mode 直推帧，
   绕过整个 DPU/SDE：
   - 使能 disp_cc/GCC 相关时钟 → 初始化 DSI PHY PLL（锁相要读回校验）
   - 用 DTS 里的 `qcom,mdss-dsi-on-command` 序列初始化面板
   - 处理 DSC：面板若支持非压缩模式可先关 DSC；否则要软件 DSC 或配 DPU 的 DSC 编码器
   - 阶段性验收可以用现有的时序信标（PHY 锁定/命令 ACK）来判定，不需要屏幕先亮。
4. 之后才是 ADT 生成（`tools/mkadt.py`，用本文件第三节的常量）→ XNU 起来 →
   存根 ramdisk root（不需要动 userdata）→ 最后才考虑 iOS 根文件系统（那一步会清 userdata）。

## 八、红线（别踩）

- `persist` / `modemst1,2` / `fsg` / `fsc` / `devinfo` / `cdt` / `ddr` 这些分区**绝对不能写**。
- 装 iOS 根会覆盖 `userdata`（= 清空 Android 数据）→ 必须用户明确同意后另行进行。
- 写 `boot_a` 前必须先确认 `~/hackios/dump/boot_a.img` 完好（md5 见上）。
- `opcust` 尾部被我们当日志盘用（400MB 全零、未挂载）；别把日志写到它的头部，
  也别让 LBA 计算出现 0（曾有一次 GPT 查不到就直接算到 0 的风险，已加保护）。

## 九、面板（m11a）参数 —— 显示驱动的输入

已从 live FDT 抽出完整面板节点：`dump/panel-m11a.dts`（377 行，原 DTS 第 31814–32190 行，phandle 0x6c4，
触摸节点的 `panel = <0x6c4 0x6b8>` 指向它）。

| 项 | 值 | 备注 |
|---|---|---|
| 模式 | `qcom,mdss-dsi-panel-type = "dsi_cmd_mode"` | **command mode**，不是 video mode |
| 分辨率 | 1440 × 3200（0x5a0 × 0xc80） | |
| bpp | 24（`dsi-bpp = 0x18`） | 面板侧 RGB888 |
| DSC | `bit-per-pixel = 0x08`（8bpp）、`slice-per-pkt = 0x02`、`slice-width = 0x2d0`(720)、`slice-height = 0x20`(32) | 1440/720 = 2 条 slice |
| bit clock | `panel-clockrate = 0x47868c00` = **1.2 GHz** | DSI PHY PLL 目标频率 |
| 帧率 | `panel-framerate = 0x3c`（60Hz） | 另有 timing@1/@2（高刷档） |
| 时序 | h: front 8 / back 8 / pulse 12；v: front 16 / back 6 / pulse 14 | |
| PHY timings | `[00 27 0a 0a 1b 25 0a 0b 0b 0a 02 04 20 0f]` | 14 字节，直接喂 DSI PHY 时序寄存器 |
| traffic | `"burst_mode"`，`mdp-trigger = 0x1`、`dma-trigger = 0x1` | |
| reset | `platform-reset-gpio = <0x3c 0x00 0x00>`（tlmm 0xf000000，gpio 0） | |
| 初始化序列 | `qcom,mdss-dsi-on-command`（3 组，对应 3 个 timing） | 逐条 DSI 短包/长包 |

最小可行显示路径（抄 LK/bootloader）：**不碰 DPU**，让 DSI 控制器进 command mode，
面板初始化后用 DSI DMA 直推帧；但 8bpp DSC 意味着要么软件做 DSC 编码，
要么先确认面板支持非压缩（改 PPS/关 DSC），要么最终还是得配 DPU 的 DSC 编码器。

## 十、macOS CI（XNU 编译）

- 账号：`XTOM0706`（用户授权，token 存 `~/.config/gh-token`，600，权限 `repo`+`workflow`，用完可撤）
- 仓库：https://github.com/XTOM0706/ios-sweet-k60 （public，含本仓库 shim 改动 + `xnu/cape/` 草稿）
- 首次 run：`workflow_dispatch`，`macos-14`，`xnu-8796` + `MACHINE_CONFIG=SWEET`/`DEVELOPMENT`，超时 120 分钟
- 本地仓库 `~/hackios/k60-repo`（1 commit 快照，push 用）与 `~/hackios/ios-sweet`（带历史的开发树）
- ⚠️ 上游 `dxb` 构建脚本**不支持 ccache**，每次都是冷编（这就是慢的原因，不是网络问题）
- 推代码注意：全局 gitconfig 有 `gh-proxy` 的 insteadOf 改写，push 时要
  `GIT_CONFIG_GLOBAL=~/.cache/git-noproxy.cfg` 绕开，token 不能进 `.git/config`

## 十一、显示驱动（进行中）：已拿到的寄存器地基

上游 Linux 驱动已拉到 `clkref/{gcc,dispcc}-sm8450.c`（公开可信来源），配合 live FDT 得到：

| 块 | 基址（物理） | 说明 |
|---|---|---|
| MDSS/MDP | `0xae00000` | `qcom,mdss_mdp`，SDE/DPU 主体 |
| DSI0 ctrl | `0xae94000` | `qcom,mdss_dsi_ctrl0` |
| DSI0 PHY | `0xae94400` / `0xae94900` | `qcom,mdss_dsi_phy0` |
| disp_cc | `0xaf00000` | 显示专用时钟控制器 |
| TLMM | `0xf000000` | 面板 reset GPIO 0 所在 pinctrl |

**disp_cc 关键寄存器（来自 `dispcc-sm8450.c`）**

| 名称 | 偏移 | 用途 |
|---|---|---|
| `mdss_gdsc` | `0x9000` | MDSS 主电源域（GDSCR，必须先开才能碰 MDSS） |
| `mdss_int2_gdsc` | `0xb000` | MDSS 第二电源域 |
| `disp_cc_mdss_mdp_clk` | `0x800c` | MDP 时钟使能（bit0） |
| `disp_cc_mdss_byte0_clk` | `0x8028` | DSI0 byte clock 使能 |
| `disp_cc_mdss_byte0_intf_clk` | `0x802c` | DSI0 byte intf |
| `disp_cc_mdss_mdp_lut_clk` | `0x801c` | MDP LUT |
| `disp_cc_mdss_mdp1_clk` | `0xa004` | 第二 MDP |

**分阶段计划（每阶段都用二进制信道验收，不需要屏幕）**

0. 读 GDSCR 状态（`0xaf09000`）判断 ABL 交接后 MDSS 电源域是否已关 → 决定后续顺序。**只读，安全**。
1. 打开 `mdss_gdsc`，轮询 GDSCR 的 power-on 位 → 二进制验收。
2. 使能 disp_cc 的 AHB/MDP 时钟分支并检查 halt 位（`enable_reg+0x4` 通常是 halt）→ 验收。
3. 读 MDSS `HW_VERSION`（`0xae00000 + 0x0`）确认能访问（若前两步没做，这一步可能挂总线，所以排在这里）。
4. DSI PHY：按 PHY timing 14 字节 + `panel-clockrate = 1.2GHz` 配 PLL，读 lock 位。
5. 面板初始化：用 `dump/panel-m11a.dts` 里的 `qcom,mdss-dsi-on-command`（3 组 timing，60Hz 用 timing@0）。
6. 首帧：DSI command mode 直推（绕过 DPU）；DSC 8bpp 的处理（软件 DSC / 关 DSC / 用 DPU 编码器）在第 5 步之后再定。

⚠️ 顺序不能乱：没开 GDSC/时钟就读 MDSS 寄存器有挂死总线的风险（设备会看门狗重启）。

## 十二、CI 首次结果（已验证）

- run `39617876767`：**9 分钟**跑完（14:33:44 → 14:42:41），`conclusion = success`
- 产物 `kernel.development.sweet`：17 MB，`Mach-O 64-bit arm64`，`MH_MAGIC_64`、`cputype 16777228` —— xnu-8796 + 全部补丁
- 本地产物：`artifacts/kernel.development.sweet`、`artifacts/kernel.zip`、另可下载 `xnu-buildtree`（23 MB，含生成头文件，供单文件编译复用）
- 结论：**不需要 Mac 虚拟机**，秒级触发的 9 分钟 CI 就是最快的迭代方式
- 缓存优化版工作流已推到分支 `cache-ci`（未合并）：加了 `actions/cache` + 每次重置补丁树，可把重复构建压到分钟级；但 9 分钟已经够快，暂不动主线

## 十三、暂停点（2026-10-21 晚，明天继续）

### 显示驱动进度：stage 0 ✅ / stage 1 ✅

| stage | 内容 | 结果 |
|---|---|---|
| 0 | 读 `disp_cc + 0x9000`（mdss_gdsc）的 GDSCR | ✅ 读通，**PWR_ON=0**（ABL 交接后 MDSS 电源域是关的） |
| 1 | 清 `SW_COLLAPSE` + 设 EN/CLK_DIS wait，再轮询 `PWR_ON` | ✅ **PWR_ON=1**，域被我们自己点亮 |
| 2 | 使能 disp_cc 时钟分支（`mdss_mdp_clk` @0x800c、`byte0_clk` @0x8028、`byte0_intf` @0x802c）并查 halt 位 | ⏭ 下一步 |
| 3 | 读 MDSS `HW_VERSION`（`0xae00000+0x0`）确认可访问 | ⏭ 依赖 stage 2 |
| 4 | DSI PHY PLL（1.2GHz）锁定 | ⏭ |
| 5 | 面板初始化（`panel-m11a.dts` 的 on-command，60Hz 用 timing@0） | ⏭ |
| 6 | 首帧（DSI command mode 直推；DSC 8bpp 方案待定） | ⏭ |

**验收方式**：二进制信道 —— shim 里 `IMEM_RESTART_REASON = 成功?0:RESTART_BOOTLOADER`，
复位后落 Android 还是 fastboot 一目了然；shim 源码里 `K60_DISP0` / `K60_DISP1` 两个宏就是这两步。

### 明天开工命令（照抄即可）

```bash
cd ~/hackios/ios-sweet/shim
CFLAGS_EXTRA="-DK60_DISP2 -DK60_NOFB" ./build.sh          # 写好后编译 stage 2
cd ~/hackios && ./run-fast.sh ios-sweet/shim/out/shim-boot.img 1
# 设备侧辅助
adb -s ffec9d48 reboot bootloader && timeout 10 fastboot devices
```

### 设备当前状态

- 已回到 Android（`ffec9d48`，adb 正常），未改任何分区，`boot_a` 备份完好
  （`~/hackios/dump/boot_a.img`，md5 `9d20ec5ed698263d1a8ff35efebcf795`）
- `opcust` 分区里被我们写过探测数据（块 0、16），该分区未挂载、无价值，无需清理
- 累积的 shim 变体镜像：`~/hackios/{w,r,disp0,disp1}.img`

### CI 侧

- 仓库 https://github.com/XTOM0706/ios-sweet-k60 （main = `e2c29b5`）
- 首次 run 9 分钟成功，产物 `artifacts/kernel.development.sweet`（17MB，arm64 Mach-O）
- 刚推的缓存版工作流已触发 run `39619638414`（在跑/或已完成，`~/hackios/ci.log` 记录）
- token 在 `~/.config/gh-token`（600），随时可撤

### 待决策（明天）

1. 继续 stage 2→6 的显示驱动（这是能看到 iOS 的唯一路径，周期最长）
2. 还是先做 `mkadt.py` 的 cape 版 + 把 sweet kernel 塞进 shim 试着起（盲跑，只能靠二进制/时序判读）
3. 顺带修复项：UFS 写不落地（XNU 要 `ufsrw=1` 可写根时会是问题）

## 十四、【关键修正】shim 从未执行 → 必须用 boot.img header v4（2026-10-22）

**根因**：K60 是 GKI 时代设备，ABL 只接受 **boot.img header v4**。我们用上游 sweet 的
`--header_version 2` 构建的镜像，ABL 收到后会 **静默忽略并回退启动 Android**，而
`fastboot boot` 仍返回 `Sending OKAY / Booting OKAY`（那只是协议应答）。

**证据链**：
1. v2 + "停 60 秒"探针 → Android 仍在自己 ~41 秒的节奏回来（停留完全没发生）
2. v2 + "关机探针"（`PSCI_SYSTEM_OFF`）→ 设备照常回到 Android（没执行）
3. **v4 + 关机探针 → 设备离线 100 秒且再未回来 = 真的执行了关机** ✅

**因此**：本次会话里之前所有基于 v2 镜像的"结果"（IMEM 信标、GDSCR `PWR_ON`、
AHB 时钟被读通、UFS 读写探针…）**全部无效**，都是 Android 正常启动的噪音。
真实状态需要全部用 v4 重测。

**已改**：`shim/build.sh` 的 `HDRVER` 默认改为 **4**（v3/v4 下 `--dtb` 会被忽略，
DTB 由 ABL 通过 x0 传入，shim 本来就只读那个指针）。

**下一步（v4 重测清单）**：
1. 关机探针 → 已验证执行 ✅
2. "停 60 秒"探针 → 验证 `delay_ms`/时序信道（注意：`CNTFRQ_EL0` 读到 0 的坑已加 19.2MHz 回退）
3. UFS 写入 → **读回前在 Android 侧 `echo 3 > /proc/sys/vm/drop_caches`**（此前"写不落地"可能是块设备页缓存假象）
4. 显示 stage 0/1/2 → 全部重测（disp_cc 读到 0 也可能是无效镜像导致的）

## 十五、2026-10-22 早：v4 生效后的真实基线

### 已经确认可用

| 项 | 状态 | 证据 |
|---|---|---|
| `fastboot boot` + **header v4** | ✅ 执行 | v4 关机探针 → 设备离线 100 秒不回来 |
| IMEM restart_reason | ✅ 生效 | shim 要求 fastboot 就停在 fastboot；写 0 就回 Android |
| `delay_ms` | ✅ 生效 | clear 镜像 0.5 s 停留 + Android 21 s 返回 |
| 两条信道 | ✅ | ①reason 二元（fastboot/Android，~15 s 一轮）②停留时长多比特 |

### 已经确认不可用（需重做）

| 项 | 状态 | 说明 |
|---|---|---|
| header v2 镜像 | ❌ 静默忽略 | K60 ABL 只认 v4；v2 时 `fastboot boot` 仍报 OKAY，然后回退启动 Android |
| UFS 主机接管 | ❌ FAIL | `ufs_take_over()`（HCE/HCS/UTRLRDY + UTRLBA 接管）不成立 → GPT 查不到、读不了、写不了 |
| 因此 UFS 日志通道 | ❌ | 需要自己做 UFSHCI 初始化（HCE、link start、UIC 命令、任务队列），不能靠"继承 ABL 状态" |

### 结论修正

- 本次会话前半段所有基于 v2 镜像的实验结论**全部作废**（IMEM 信标、GDSCR 读到 0、
  AHB 时钟读通、UFS 读写"不落地"、shim 执行证据…）
- 显示相关的 stage 0/1/2 需要**用 v4 全部重测**后才能下结论
- 教训：**"代码在跑"这件事必须用不可误读的信号证明**（关机探针是最便宜的一种）

### 下一步优先级

1. **UFSHCI 初始化**（唯一能拿到日志的路）：读 CAP/HCS/HCE 与 UIC 状态 → 自己使能 HCE、
   发 DME_LINKSTARTUP、配置 UTRL/UTRD 队列 → 再做 SCSI READ/WRITE。
   一旦通了，日志、iOS 根可写、后续所有调试都有了地基。
2. 用 v4 重测显示链路（GDSCR/AHB/MDSS 可读性），确认能否自点 DSI。
3. 再谈 XNU（kernel 已有，`artifacts/kernel.development.sweet`）。

### 工具与镜像清单（v4）

- 通用构建：`HDRVER=4 CFLAGS_EXTRA="-DK60_XXX -DK60_NOFB" ./build.sh`
- `v4-clear.img` 清 reason 回 Android；`v4-ufschain.img` 链路结果用停留时长编码
- `q1..q4.img` 二元问答（Q1 takeover / Q2 GPT / Q3 写 / Q4 读回）
- 信道工具：`./boot-once.sh <img>`（自动等交接、测返回时长）

### 二元问答实测（v4，2026-10-22 早）

| 问题 | 结果 |
|---|---|
| Q1 UFS 主机接管成功？ | ❌ FAIL |
| Q5 disp_cc 时钟块寄存器可读（非零）？ | ❌ FAIL（读到 0/0xffffffff） |
| Q2/Q3/Q4/Q6 | 受限未完成（发送失败/未跑） |

**Q5 的含义**：显示时钟控制器整块不可访问（需要 VDD_MM/VDD_MXA 供电轨 + GCC iface 时钟），
所以「自己点亮屏幕」必须先做 RPMh 供电 + GCC 时钟，再谈 disp_cc/DSI。

**Q1 的含义**：UFS 控制器不能沿用 ABL 的现场，需要自己跑 UFSHCI 初始化
（HCE、UIC DME_LINKSTARTUP、UTRL/UTRD 队列）——这是拿到任何日志的前置条件。

## 十六、2026-10-22：UFS 主机初始化实测（A 项）

用二元信道逐级二分（`-DK60_UFSGT -DK60_GT=<k>`，PASS→fastboot / FAIL→Android）：

| 阶段 | 内容 | 结果 |
|---|---|---|
| 1 | 等 `HCS.UCRDY` | ✅ 过 |
| 2 | 置 `HCE` | ✅ 过 |
| 3 | UIC `DME_LINKSTARTUP` 完成位 | ✅ 过 |
| 4 | UIC 结果码 == 0 | ✅ 过 |
| 5 | `HCS.DP`（UniPro 链路 DP 起来） | ✅ 过 |
| 6 | 写 `UTRLRSR=1` 后 run 位读回 1 | ✅ 过 |
| 7 | 写 `UTRLBA/BAU` 后读回等于我们的地址 | ❌ **卡在这里** |

**结论**：`ufs_host_init()` 的链路层完全打通了（HCE + UniPro link up），
只差最后一步——控制器不接受我们写的请求队列基址。可能原因：
64 位寻址模式、UTRL 基址必须在 4 GiB 以下/特定对齐域、或该 UFSHCI 版本要求先写
`UTRLBA` 再置 `UTRLRSR`（顺序已试）、或 0x50 偏移在本版本被重映射。
**下一步**：读回 `CAP`（含 `64AS` 位）、`VER`、`UTRLBA` 实际值，用二元信道对比猜测值。

代码状态：`shim/ufs.c` 新增 `ufs_host_init()`（UCRDY→HCE→UIC link start→UTRL），
`ufs_take_over()` 已改为调用它，`-DK60_UFSGT/-DK60_GT=<k>` 与 `-DK60_BIN/-DK60_Q=<n>`
为二分探针模板。所有镜像必须 `HDRVER=4`。

## 十七、B 项（显示供电链）—— 未开始

已确认 `disp_cc` 整块不可访问（Q5 FAIL），需要先在 RPMh/AOSS 上把 `vdd_mm` / `vdd_mxa`
供起来、再打开 GCC 的 `iface` 时钟，disp_cc 才可能读通。这是下一步 B 的前置。

## 十八、2026-10-22 续：UTRLBA 诊断（未闭环）

| 探针 | 结果 | 含义 |
|---|---|---|
| Q30 `UTRLBA` 读回非零？ | FAIL（读回 0） | 我们写的队列基址**没有被锁存** |
| Q31 `CAP.64AS`（64 位寻址） | FAIL | 控制器**不支持 64 位 DMA 寻址** |
| Q32 `CAP.NUTRS>=32` | 未判读 | 同上批被设备状态干扰 |
| Q33 `UTRLBA == 我们的地址` | 未判读 | 同上 |
| Q34/Q35 镜像在 4 GiB 以上/以下 | 两者都 FAIL（互相矛盾） | **探针判定不可信**：说明某些轮次镜像根本没被 boot 成功 |

**当前判断**：
- 链路层（HCE + UniPro DP + UIC 命令）确实是通的
- 唯一硬点：`UTRLBA` 写不进去；结合 `CAP.64AS=0`，优先怀疑
  ① 描述符/镜像被 ABL 放在 4 GiB 以上（控制器寻址不到）
  ② 或该 UFSHCI 版本要求不同的队列配置序列

**工具缺陷（必须先修）**：二元问答脚本没有校验「镜像真的被 hand-off」，
发送失败/未交接会被误判成 FAIL。修法：判定前先确认 fastboot 设备**消失过一次**
（`boot-once.sh` 里有这个逻辑，`ask()` 里没有）。在修好这个之前，
所有 PASS/FAIL 都要打折看。

## 十九、下次开工建议顺序

1. 修 `ask()`：加 hand-off 校验（消失→再出现才算数），并打印失败原因
2. 用修好的信道重测 Q32/Q33/Q34/Q35（4 轮，约 2 分钟）
3. 若确认「镜像在 4 GiB 以上」→ 需要把 UFS 描述符放到低地址（例如用
   `cont_splash_region 0xb8000000` 这块 45 MiB 保留区当 DMA 缓冲，它肯定在 4 GiB 以下）
4. 通了之后：SCSI READ → GPT → 日志通道；再回头做 B（显示供电链）

## 二十、2026-10-22：低地址 DMA 重试 + 探针修正

**工具修正**：新增 `ask.sh`（带 hand-off 校验：发送 OK → 设备必须先从 fastboot 消失 →
再判 PASS/FAIL/UNKNOWN）。旧 `ask()` 的判定不可信，已弃用。

**改动**：把 UFS 全部 DMA 缓冲搬到 `cont_splash_region 0xb8000000`
（UTRL@+0x0、UCD@+0x1000、bounce@+0x2000、GPT entries@+0x4000），
用宏保持原代码写法不变。这正是 Qualcomm 平台"低地址 DMA"的标准做法。

**实测（可信判定）**：

| 探针 | 结果 |
|---|---|
| Q1d 全链路（初始化+GPT 查 opcust） | ❌ FAIL |
| `stage<=5`（链路 DP 为止） | ❌ FAIL |
| `stage<=6`（队列 run 位） | ❌ FAIL |

→ 仍是 **stage 7：`UTRLBA` 写后读回不等于我们的地址**，低地址**没有解决**。

**下一步的四个候选（按可能性排序）**

1. **`CAP` 读数可能不可信**：Q31 的 `CAP.64AS=0` 是在**未做 HCE/链路**时读的；
   应在 `ufs_host_init()` 之后再读 CAP/VER（加探针 Q40/Q41）
2. 我加的 `ufs_wr(REG_UTRLCLR, 0xffffffff)` 可能惹事 → 删掉重试
3. `VER` 若 >= 0x400（UFSHCI 4.0），legacy 的 0x50 队列寄存器可能被重映射/需要 legacy 使能位
4. 参考其它 fork（`ios-raphael`/`ios-apollo`/`ipados-nabu`）的 shim 与 XNU UFS 补丁：
   SM8250/SM8150 与 cape 同代，它们的队列初始化序列可直接对照

## 二十一、2026-10-22：A/B 两条路收敛到同一个前置（电源+时钟）

**探针结果（全部经 `ask.sh` 的 hand-off 校验，可信）**

| 探针 | 结果 | 含义 |
|---|---|---|
| Q40 CAP 可读（HCE 之后） | ❌ | CAP 读到 0 |
| Q42 VER >= 0x300 | ❌ | 同上 |
| Q43 `HCS` 非零（寄存器块活着？） | ❌ | **UFS 寄存器块整体读不到** |
| Q44 HCE 已由 ABL 置位 | ❌ | 一致：控制器没上电就没法有状态 |
| Q46 GCC GDSCR 可读 | ❌ | GCC 侧 GDSCR 也读不到 |
| Q1 全链路 | ❌ | 自然失败 |

**结论（重要）**：ABL 交接后，**UFS 控制器和显示时钟块都是断电/断时钟的**，
寄存器读到 0。之前"UFS 链路能起来"的判断是在寄存器不可读的情况下由
`wait_set()` 的边界行为造成的误判——需要作废。

**因此 A（日志）和 B（显示）收敛为同一个前置工作**：
1. **GCC 时钟**：`gcc_ufs_phy_*`、`gcc_disp_*`、以及各 GDSC（GCC 通常常开，需先确认基址与可读性）
2. **RPMh 供电**：`vdd_ufs`、`vdd_mm`、`vdd_mxa`（PMIC 侧，经 AOSS/RSC）
3. 之后才是 UFSHCI 初始化 / disp_cc+GDSC+DSI

**下一步（下次开工）**
1. 先确认 GCC 基址与可读性：设备树 `clock-controller@100000` 的 reg、以及一个确定非零的 GCC 寄存器
   （例如 `gcc_ufs_phy_axi_clk` 的 halt 寄存器），把 Q46 的失败原因分清是"地址错"还是"被门控"
2. 读 `ufs_phy_gdsc`（GCC 0x87004）的 GDSCR 原始位，判断域状态与是否可软开
3. 用 GCC 使能 UFS 的 AXI/AHB/ICE 时钟 → 再试 UFSHCI 初始化（Q43 变 PASS 即成功）
4. 显示方向同理：GCC 的 disp 相关时钟 + RPMh 的 vdd_mm/vdd_mxa

## 二十二、2026-10-22：固件静态分析尝试（abl_a / imagefv_a）

用户的思路：把分区提出来分析，直接看 ABL 对显示/UFS 做了什么。

**提取结果**

| 分区 | 大小 | 结构 |
|---|---|---|
| `abl_a` | 8 MB | ELF32 ARM，**实际内容仅前 ~0x3E000**，其余全零；段1 = `0x30000 @ vaddr 0x9fa00000` |
| `imagefv_a` | 48 MB | ELF32 ARM，头部熵 **7.57 bit/byte**（压缩/加密特征），全文件可读字符串仅 79 条 |
| `devcfg_a` | 256 KB | 未分析 |

**结论**：ABL 的字符串里**没有任何 splash/display/dsi/panel/ufs/gdsc 痕迹**；
`imagefv` 同样没有。两者载荷都是高熵（压缩或加密），`strings` 路线走不通。

**后续可选（按性价比）**
1. 用 Ghidra/radare2 反汇编 `0x9fa00000` 段（ARM32）——工作量大，但能看到 ABL 对
   `0x1d84000`（UFS）/`0xae00000`（MDSS）的写序列，直接告诉我们它关了什么
2. 更划算：**运行时探测**（已在做）——用 shim 自己读 GCC/GDSCR/UFS 寄存器，
   用二元信道问出状态；这比静态逆向快得多
3. 取 `xbl_a`（SBL，早期 DDR/时钟/供电初始化）看是否也加密；若未加密，可拿到
   RPMh 供电序列的寄存器写法

**当前主结论不变**：UFS 与显示在 ABL 交接后都断电/断时钟，
必须先做 GCC 时钟 + RPMh 供电，才谈得上 UFSHCI 初始化或点屏。

## 二十三、2026-10-22：Android debugfs 插桩 —— 拿到"标准答案"（重大）

`debugfs` 在这个 ROM 里默认没挂载，但 root 可以挂（`mount -t debugfs none /sys/kernel/debug`）。
挂上后直接读出 Android 驱动真实配置的时钟树与供电状态，**这就是我们自己做 bring-up 要照抄的目标**。

已存盘：`dump/clk_summary.txt`（486 行）、`dump/regulator_summary.txt`（254 行）。

**显示侧（Android 运行时的真实值）**

| 时钟 | 速率 | 说明 |
|---|---|---|
| `dsi0_phy_pll_out_dsiclk` | 200 MHz | DSI PHY PLL 输出（不是 1.2GHz，PLL 有分频） |
| `disp_cc_mdss_pclk0_clk_src` / `pclk0_clk` | 200 MHz | 像素时钟 |
| `dsi0_phy_pll_out_byteclk` | 150 MHz | byte clock |
| `disp_cc_mdss_byte0_clk_src` / `byte0_clk` | 150 MHz | |
| `disp_cc_mdss_byte0_div_clk_src` / `byte0_intf_clk` | 75 MHz | |
| `gcc_disp_xo_clk` / `sf_axi` / `hf_axi` / `ahb` | 使能 | GCC 侧显示基础设施 |

**UFS 侧**

| 项 | 状态 |
|---|---|
| `gcc_ufs_phy_gdsc` | **已使能**（regulator 列表里 count=1, normal） |
| `gcc_ufs_phy_ahb_clk`、`gcc_ufs_0_clkref_en` | 使能 |
| `ufs_phy_tx/rx_symbol_*_clk` | 使能 |
| `ufshc` 供电 | `vdd-hba`、`vcc`、`vccq`、`vddp-ref-clk`、`vdda-phy` 全部在 |

**意义**：不用猜、不用逆 ABL——直接照着这份总结在 shim 里把同样的时钟/供电打开即可。
`debugfs` 还可继续挖：`/sys/kernel/debug/clk/<name>/`（每个时钟的父/速率/寄存器）、
`regulator_summary` 的电压、以及可能的 `ufs`/`drm` 调试节点。

**本轮其它探针（结论有限）**
- Q52/Q53：想用「ABL 传入的 FDT magic」当"内存读正常"的对照，但 **v4 镜像下 x0 不一定给 DTB**
  （v3+ 的 DTB 在 vendor_boot 里），所以该对照**无效**，不能据此判断 MMIO 读是否正常
- Q49/Q50：TLMM GPIO0 配置读数为 0 —— 也可能只是该引脚未配置，同样不能下结论

**下一步**
1. 用 debugfs 的 `clk/<name>/` 节点把每个必需时钟的 **enable 寄存器偏移/位** 读出来（clk 子系统 debugfs 通常有 `reg`、`parent`、`rate`）
2. 按这份清单在 shim 里实现 GCC 侧使能（disp/ufs 各一组），再用二元信道验证"块可读"
3. UFS 通了之后接 UFSHCI 初始化；显示方向接 disp_cc/DSI

## 二十四、2026-10-22：判定链失效（Q65 自检失败）—— 必须先修工具

**最后一道自检**：`K60_BIN` 里加 `Q=65`（不做任何事、`pass=1`、直接写 reason 回 fastboot）。
**结果 FAIL（进了 Android）**，而这正是我们之前用来证明"信道可用"的同一条代码路径。

已核实：镜像里确实包含 `0x77665500` 与 IMEM 地址常量（反汇编 2/2 命中），
`_end=0x2a000` 与 Image 大小 172032 一致，`.bss` 86 KB 正常。

**因此**：本轮 Q40–Q64 的所有 FAIL **不能作为结论**（可能是代码没跑到，也可能是 reason 未生效，
两者在 FAIL 分支上不可区分）。唯一可信的判据是 **PASS**（因为只有真的写到 reason 才会进 fastboot）。

**下次开工必须先做的三件事（工具优先）**
1. 让 `Q65` 重新 PASS：对比 `v4-write.img`（同代码路径、已知能进 fastboot）与本轮镜像的差异
   （最可能：Image 从 221184 缩到 172032 后，某处布局/偏移假设变了；或 ask.sh 的 30 s 等待窗口太短）
2. `ask.sh` 增加"UNKNOWN 区分"与更长的 fastboot 枚举等待（把 30 s 提到 90 s）
3. 只有在 Q65 PASS 之后，才重新跑 Q43（UFS 块可读）/Q62（GICD 对照）/Q60（GCC UFS 上电）

**本轮唯一确定的新资产**：`dump/clk_summary.txt` / `dump/regulator_summary.txt`
（Android 真实时钟树与供电清单）以及 `ufs.c` 里 `gcc_ufs_bring_up()` 的寄存器偏移表
（gdsc 0x87004、ahb 0x87020、axi 0x87018、ice 0x8706c、unipro 0x87064、phy_aux 0x870a4、
tx/rx symbol 0x87024/0x87028/0x870c0、clkref 0x9c000）——这些不依赖信道，可直接用。

## 二十五、2026-10-22：信道重新定性（重要）

### 发现 1：`misc` 里有残留的 `bootonce-bootloader`
`adb reboot bootloader` 会在 `misc` 的第 0 字段写 `bootonce-bootloader`。
它和 IMEM 的 restart_reason 抢优先级，是之前 PASS/FAIL 混乱的一个来源。
已备份前 4KB（`dump/misc_head_backup.bin`）并清空该 32 字节命令字段。

### 发现 2：`fastboot boot` 之后，shim 复位会回到 **fastboot**
用 60 秒停留镜像（`-DK60_SLEEP`，delay 已加 19.2MHz 回退）实测：
`fastboot boot` → 设备离开 fastboot → **约 60 秒后重新出现在 fastboot**。
这同时证明了两件事：
1. **shim 确实执行了 60 秒**（delay/停留机制可用）
2. **快迭代通道成立**：一轮 = 停留时长 + ABL 回 fastboot（~5 s），不需要等 Android

因此「时长信道」是当前最可信的判据，判读方式：
`./holdtime.sh <img>`（等 hand-off → 计时 → fastboot 重现），
基线为 0 秒停留镜像的返回值，差值即停留秒数。

### 发现 3：`fastboot boot` 偶发 BOOT_FAILED
设备在 fastboot 里但 `fastboot boot` 返回失败（疑似上次会话残留状态）。
规避：先 `fastboot reboot bootloader` 重置一次再发；`holdtime.sh` 需要加这一步重试。

### 本轮结论
- 之前基于 reason 的 PASS/FAIL 结论**不可全信**（PASS 可能来自 misc 残留命令或 reset→fastboot 的默认行为）
- **可信判据**：`holdtime.sh` 测出的停留时长（≥10 s 差值才算数）
- 已具备的能力：v4 镜像执行、60 秒级停留、复位回 fastboot（快迭代）

### 下次开工
1. 给 `holdtime.sh` 加重试（BOOT_FAILED → `fastboot reboot bootloader` 后再发，最多 3 次）
2. 用时长信道重测三条地基：UFS 块是否可读（Q43）、GCC UFS 上电（Q61）、GICD 对照（Q62）
   —— 每条都用「hold = 2s + 5s*code」编码，比二元信道信息量大且不依赖 reason
3. 通过后再继续 UFSHCI 初始化 / disp_cc

## 二十六、2026-10-22：GCC UFS 上电成功 —— UFS 与 GICD 都活了（重大）

**脚本统一**：新增 `probe.sh` —— 唯一入口，自己处理三种设备状态
（ABL fastboot / fastbootd / Android），先确保 ABL fastboot、发镜像、等 hand-off、
再报落地结果。不再手动切模式。基线实测：0 停留镜像 = `ANDROID 16s`。

**多比特时长探针 `timing.img`（hold = 2s + 3s*code）**

| 项 | 结果 |
|---|---|
| 总耗时 | **63 s**（基线 16 s → 停留 ≈47 s） |
| 解出 code | **15 = 0b1111** |
| bit0 UFS `HCS` 可读 | ✅ **通** |
| bit1 UFS PHY GDSC 上电 | ✅ **通**（`gcc_ufs_bring_up()` 生效） |
| bit2 GICD_TYPER 可读 | ✅ **通**（说明 shim 的 MMIO 读本身没问题） |
| bit3 恒 1（有效性） | ✅ |

**意义（推翻了上一轮的悲观结论）**
- 之前的 `CAP=0 / HCS=0 / GICD=0` 全是**信道失效**造成的假象，**不是硬件被彻底关死**
- 只要先按 `gcc_ufs_bring_up()` 把 `ufs_phy_gdsc` + `clkref/ahb/axi/unipro/phy_aux/ice/tx_symbol`
  打开，**UFS 控制器寄存器块就变得可读**
- GICD 可读 → shim 的物理地址访问、MMU 状态都正常

**下次开局动作**
1. 把 `gcc_ufs_bring_up()` 接进 `ufs_take_over()`：先上电再走 UFSHCI 初始化
   （UCRDY → HCE → DME_LINKSTARTUP → UTRL 队列 → READ(10) → GPT 查 `opcust`）
2. 结果用时长信道编码（stage 1..7，`hold = 2 + 3*stage`），一轮 ~20-60 s
3. 通了就写日志文件；之后转显示（用同样的 `gcc_disp_*` + `disp_cc` 思路，
   目标速率照抄 `clk_summary.txt`：dsiclk 200MHz / byteclk 150MHz / pclk0 200MHz）

## 二十七、2026-10-22：信道纠错 + UFS 读到 sense key（第 10 轮）

### 信道三处修正（都是我的工具 bug）

1. **`probe.sh` 的 Android 检测写错了**：只匹配 Google VID `18d1:4ee7`，
   而这台机器 Android 侧是 **Xiaomi VID `2717:ff48`** → 之前大量"device not on USB"/FAIL 都是误报。
   已改成用 `adb devices` 判断。
2. **IMEM reason 信道确认无效**：用修好的 harness 复测，`reason=1`（要求 fastboot）仍然进 Android。
   → 二元信道不存在，别再指望它。
3. **时长为编码被内部耗时污染**（重试/等就绪会加几秒到几十秒）→ 已实现**自校准**：
   `report_at(ms)` 以 shim 入口时刻为基准等到目标时间，内部工作耗时不再影响测量。

### UFS 进展

| 步骤 | 结果 |
|---|---|
| GCC 上电（gdsc + 8 个时钟） | ✅ |
| UFSHCI：UCRDY / HCE / UIC DME_LINKSTARTUP / UTRL 队列 | ✅ |
| `TEST UNIT READY` 等待就绪（带延时轮询） | ✅ 设备最终 READY |
| `READ(10)` LBA 1 | ❌ 返回 **sense key ≈ 11（ABORTED COMMAND）** |

**下一步候选**
1. `START STOP UNIT`(0x1B, START=1) 让 LU 进入 active power mode 后再读
2. 核对 UPIU 的 "Expected Data Transfer Length" 与 PRDT 的 DBC 是否一致（4096 vs 4096）
3. 读 UFS **Unit Descriptor**（UIC `DME_GET`）确认 bLUEnable / bBootLunID / 逻辑块大小
4. 确认 LUN 号：ABL 用的可能是 LUN 0，但 UFS 的 "boot LUN" 映射可能把 LU 编号偏移

### 基线
- `probe.sh` + 0 停留镜像 = **ANDROID 16 s**（当前基线）
- 自校准后：`hold = 2 + 4*key`（sense key），init 失败 = 70 s 标记

## 二十八、2026-10-22：UFS 命令通但设备从不 READY（第 11 轮）

**扫描结果**：遍历 LUN 0–5，每个最多 25 次 `TEST UNIT READY`（带延时），
**没有任何 LUN 返回 ready**（停留 100 s 标记命中）。

**已确认能用的部分**
- GCC 上电（`ufs_phy_gdsc` + `clkref/ahb/axi/unipro/phy_core/phy_aux/ice/tx_symbol`）
- UFSHCI：UCRDY → HCE → UIC `DME_LINKSTARTUP` → UTRL 队列（stage 0）
- SCSI 命令确实被发出了（用 CAP / READ 都拿到 SCSI 状态回包）

**问题**
- 所有 LU 一律 NOT READY / ABORTED COMMAND
- 说明链路起来了但设备没被"唤醒到可服务状态"

**下一步候选（按 UFS 规范顺序）**
1. `START STOP UNIT`(0x1B) 带 power condition=1(active) 发给每个 LU
2. UIC `DME_SET` 设置 `PA_Tactivate`／进入 fast/high-speed power mode（Linux ufshcd 在
   link startup 之后会做 DME_GET `PA_MaxRxHSGear`、DME_SET `PA_TxGear`/`PA_RxGear` 等）
3. `DME_GET` 读 **Unit Descriptor**：确认 `bLUEnable`、`bBootLunID`、逻辑块大小
4. 若 3 显示 LU 未使能 → 需要 `DME_SET` `bLUEnable` 或用 "Configuration Descriptor" 配置
5. 备选：跳过 SCSI，直接用 UIC 命令空间做诊断（读 descriptor 不依赖 LU ready）

**工具状态**：`probe.sh` + 自校准 `report_at()`，标记间距 ≥20 s（因为 UFS 上电要等十几秒，
内部耗时已不再污染读数）。

**基线**：0 停留 = ANDROID 16 s；本次标记 20+10*lun（就绪）/100（都不就绪）/130（初始化失败）。

## 二十九、2026-10-22：START STOP UNIT 无效（第 12 轮）

对 LUN 0–5 各发 `START STOP UNIT`(0x1B, START=1, power condition=0 active) 后再轮询
`TEST UNIT READY`：**仍然没有任何 LU 就绪**（停留 100 s 标记）。

**当前 UFS 状态汇总**

| 环节 | 状态 |
|---|---|
| GCC 上电（GDSC + 8 时钟） | ✅ |
| UFSHCI 初始化（UCRDY/HCE/UIC DME_LINKSTARTUP/UTRL） | ✅ |
| SCSI 命令执行 + 收到 sense | ✅（sense key 2 NOT READY / 11 ABORTED） |
| LU 进入可服务状态 | ❌（TUR/SSU 都不行） |

**最可能缺失的步骤：UIC 的 gear/mode 协商**
Linux `ufshcd` 在 `DME_LINKSTARTUP` 之后还会做一串 UIC 操作，我们全跳过了：
`DME_GET(PA_MaxRxHSGear)` → `DME_SET(PA_TxGear/PA_RxGear/PA_TxTermination/PA_RxTermination)`
→ `DME_SET(PA_HSSeries)` → `DME_SET(PA_PWRMode = FAST_MODE)` → 再 `DME_SET(PA_Tactivate)`。
没有协商到高速模式时链路仍停在 PWM 慢速档，命令可能超时（我们的 `scsi()` 超时是 1 s）。

**下一步（明确）**
1. 实现 UIC `DME_GET/DME_SET`（opcode 0x16/0x17 + attribute selector），先读
   `PA_MaxRxHSGear` 确认对端能力，再按 ufshcd 的顺序设置 gear/power mode
2. 然后用 `DME_GET` 读 Unit Descriptor（`bLUEnable`、逻辑块大小），不经 SCSI
3. 上述都通了再重试 TUR → READ(10)

**风险提示**：这条线比上游项目深很多（sweet 的 ABL 把 UFS 留在可用状态），
预计还要 3–5 轮才能拿到第一次成功的块读。

## 三十、2026-10-22：UIC DME 通路打通（第 13 轮）

**新实现**（`shim/ufs.c`）：
- `ufs_uic_cmd(op, arg1, arg2, *arg3)` —— 通用 UIC 命令（写 ARG1..3 → 写 UICCMD →
  等 `IS.UCCS` → 查 `ARG2[23:16]` 结果码 → 读 `ARG3`）
- `ufs_dme_get(attr, *val)` / `ufs_dme_set(attr, val)`，属性选择子 = `(attr << 16) | gen_sel`
- opcode：`DME_GET = 0x01`、`DME_SET = 0x02`、`DME_LINKSTARTUP = 0x16`（后者此前已在用）

**实测**：`DME_GET(0x0083)` **成功返回**（没有错误码）→ UIC 命令通道可用 ✅
但读回的 `PA_MaxRxHSGear = 0`，UFS 3.1 设备理论上应是 4 —— 说明**属性号可能不对**
（UniPro 的 PA_* 属性编号我记错了；`0x0083` 那组是旧版猜测）。

**下一步**
1. 从可靠来源确认 UniPro 属性号（`include/ufs/unipro.h`）：
   `PA_MAXRXHSGEAR` / `PA_RXGEAR` / `PA_TXGEAR` / `PA_TXTERMINATION` /
   `PA_RXTERMINATION` / `PA_HSSERIES` / `PA_PWRMODE` / `PA_ACTIVETXDATALANES`
   （GitHub raw 当时被限流，可改用镜像或本地内核源码）
2. 用正确属性号重读 gear → 按 ufshcd 顺序 `DME_SET` 协商到 HS 模式
3. 再试 TUR / READ(10)

## 三十一、2026-10-22：UniPro 协商到 HS-G4 成功（第 14 轮）

**用 Bark 号修正 + 完整协商序列**
属性号取自 `include/ufs/unipro.h`（kernel.org 原始源，绕开 GitHub 限流）：
`PA_MAXRXHSGEAR=0x1587`、`PA_TXGEAR=0x1568`、`PA_RXGEAR=0x1583`、
`PA_TXTERMINATION=0x1569`、`PA_RXTERMINATION=0x1584`、`PA_HSSERIES=0x156A`、
`PA_PWRMODE=0x1571`（`FAST_MODE=1`）。

`ufs_negotiate()`：`DME_GET(PA_MAXRXHSGEAR)` → `DME_SET(termination/series/txgear/rxgear/pwrmode)`
→ `DME_GET(PA_RXGEAR)` 回读。

**实测：协商成功，回读 `RXGEAR = 4`（HS-G4 高速档）** ✅

**但**：HS 起来了，LU 仍然一直不 READY（停留 60 s 标记）。
说明问题在 **SCSI/UPIU 层或设备端 LUN 配置**，不再是链路速率。

**下一步**
1. 核对命令 UPIU 的字段（`flags`、`LUN`、`task tag`、Expected Data Transfer Length）
   与 UTRD（`CT/DD/I` 位、`PRDT offset/count`）—— 逐字段对照 ufshcd 的 `ufshcd_compose_upiu`
2. 用 UIC `DME_GET` 读 **Unit Descriptor**（`bLUEnable`、`bBootLunID`、逻辑块大小）
   确认 LU 是否已被 ABL 配置为使能；若 `bLUEnable=0`，需要写 Configuration Descriptor
3. 试 `DME_GET(PA_MAXRXDATALANES)` / `PA_ACTIVERXDATALANES`，确认只用 lane 0 是否合法

**UFS 全链路进度**
`GCC 上电 ✅ → UFSHCI 初始化 ✅ → UIC DME ✅ → HS-G4 协商 ✅ → LU 就绪 ❌ → SCSI 读写 ⏳`

## 三十二、2026-10-22：TUR 失败定性为 SCSI 状态（第 15 轮）

细分测量（先协商 HS-G4，再对 LUN 0 发一次 TUR）：

| 观察 | 结论 |
|---|---|
| `rc = 4` | 命令**传输完成**，但设备返回了非零 **SCSI 状态**（CHECK CONDITION） |
| 之前的 sense key | 2（NOT READY）/ 11（ABORTED） |

**所以不是 UPIU/队列的 bug，也不是链路速率问题**——设备明确说"我还没准备好"。

**最可能的根因**：ABL 交接后 UFS 设备的 **LU 配置丢失/未启用**。
我们为了让 PHY 可用而重开了 `ufs_phy_gdsc`；如果 ABL 同时关掉了 UFS 设备供电（VCC），
设备就会以上电默认状态回来——**LUN 全部未配置**（bLUEnable=0），此时任何 LU 都只会回
NOT READY，`START STOP UNIT` 也无济于事。

**要往前走，必须做 UFS 设备侧的 LU 配置**（写 Configuration Descriptor）：
- 需要走 **UTP Query Request UPIU**（transaction type 0x07，opcode WRITE DESCRIPTOR 0x02）
- 要写：`Device Descriptor` 的 `bNumberLU`、`bBootLunEn`，以及每个 LU 的
  `Unit Descriptor`（`bLUEnable`、`bBootLunID`、`qLogicalBlockCount`、`bLogicalBlockSize`）

⚠️ **风险（必须先确认）**：错误地写 Configuration Descriptor **会改变 LU 的尺寸/映射**，
轻则 Android 读不到数据、重则**抹掉 userdata 的分区视图**。这是本项目目前唯一可能
"真的把数据搞坏"的操作。

**替代方案（无风险）**
1. 在 shim 里**不重开 `ufs_phy_gdsc`**，而是先读取它原本的状态；若 ABL 已让 VCC 断电，
   则 UFS 这条路在当前约束下不可行 → 改用**其它日志通道**（见下）
2. 其它日志通道候选：
   - `PSCI_SYSTEM_OFF` + 断电后 RAM 内容不可读（不可行）
   - 把结果写进 **splash 保留区**，下一次 shim 启动再读（可做，但要两段式）
   - 依赖 `hold` 时长的多比特信道（已可用，最慢但零风险）

**当前建议**：先用"两段式 splash 保留区 + 时长信道"把 UFS 结论固化，
UFS 设备配置这件事**需要用户明确同意**后再动。
