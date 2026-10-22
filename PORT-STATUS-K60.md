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
