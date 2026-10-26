# GPU 移植笔记（2026-10-26 离线轮）

本机没有设备，也没有 macOS，所以这一轮只做三件事：读上游、本地编译/仿真校验、
把上机判据写死。**没有上机，没有推送任何远端。**

## 0. 一句话结论

`0179` 的寄存器地址/位域是对的，但时钟等待有 3 处与 vendor 语义不符（会在正常
硬件上误报失败），只读探测还先读了最可能挂死的 GX 域。已写成 `0180` 修正，
并在本地仿真台上验证（见 §4）。M1 的验收判据里「chipid 0x7030002」是文档错误：
它是 DT 常量，不是寄存器，代码永远打不出硬件读到的 chipid。

## 1. 参考仓

| 仓 | 提交 | 本地位置 | 内容 |
|---|---|---|---|
| `UniXNU/UniXNU` | `ae68700` | 稀疏克隆（drivers,PongAdreno/docs/userland，4.7 MB） | 框架 + 新代际：`drivers/Adreno740`、`drivers/AdrenoMetal`、`docs/zh/porting/gpu.md` |
| `Chengdong-Yu/UniXNU-pong` | `ab74f07` | `~/hackios/refs/UniXNU-pong`（完整） | 同 SM8475 的板级实现 `drivers/PongAdreno`（2781 行） |

⚠ 上游文档自相矛盾：`docs/zh/devices/pong.md` 写「Adreno 730 硬件加速与 Metal
已接通」，英文 `docs/devices/pong.md` 写「hardware acceleration is not confirmed」。
按英文和 README 为准——**未确认**。别把中文那句当既有成果。

## 2. 四层对照：对 K60 能搬什么

| 层 | 上游载体 | 对 K60（A730/gen7-4-0）的结论 |
|---|---|---|
| 内核 AGX 提交 ABI | `PongAdreno/submit/`：`NativeAdrenoABI.h`、`AdrenoPM4.c/h`、`NativeAdrenoClient/Vm/GmuPlatform/GmuMemory/*`（1667 行） | **可搬，但要改**。chip_id 就是 `0x07030002`（`PongAdrenoGmu.h`），和 K60 的 `qcom,chipid` 一致；PM4 包与 BO/VA 生命周期是通用 gen7。改动：`pong_` 前缀/ramoops 诊断换成我们的 `Sweet*`；服务发布要接到 `IOAcceleratorES`（我们 0178 的空壳正好当挂点）；它假设 GMU 已起，顺序必须排在我们 M3 之后 |
| 板级层 PongAdreno | `PongAdrenoPower.c`、`PongAdrenoClocks.c`、`PongAdrenoGmu.c`、`PongAdrenoZap.c`、`PongAdrenoCp*.cpp` | **能直接当参考实现**。Cape GPUCC 寄存器/位域与我们完全同源（逐条比对见 §3）；`PongAdrenoClocks.c` 就是 `SweetAdreno` 上电序列的出处。GMU 三段区域 `{0,0x4000,1},{0x4000,0xffc000,2},{0x10004000,0x4000,3}`、RSCC/PDC 序列可直接照抄 |
| 用户态 metal2vulkan + Turnip | 不在 `UniXNU-pong` 里；`UniXNU` 的 `AdrenoMetal` + `scripts/build-turnip-darwin.py` + `.cache/mesa-work` | **不能直接搬**。它是「自己编 Turnip + 自研 metal2vulkan」的构建产物层，且面向 A740；A730 要先有内核 kmd 与 fence，才谈前端。属 M4 之后 |
| 只读 union 注入 | `userland/`（`init.c`、`libmini`、`ui_compat.c`）+ `pong-payload.patch` | **不需要**。那是它们自己的 userland 引导方式；我们已经有 shim + HFSX 真根 + launchd 跑起来了，照搬反而会丢掉现有成果 |

## 3. 0179 骨架核对（逐项，有出处）

地址全部取自 `dump/live.dts`（不是文档抄的）：

| 块 | 我们的宏 | DT reg | 判定 |
|---|---|---|---|
| kgsl | `0x3d00000` / `0x40000` | `qcom,kgsl-3d0@3d00000` reg[0] | ✓ |
| gmu | `0x3d68000` / `0x37000` | `qcom,gmu@3d69000` reg[0]，**节点名写 3d69000、reg 却是 3d68000** | ✓（按 reg，不按节点名） |
| gmu_ao_blk_dec0 | `0x3d40000` / `0x10000` | 同节点 reg[2] | ✓ |
| gpucc | `0x3d90000` / `0x10000` | `clock-controller@3d90000` | ✓（上游只用前 `0xa000`） |
| gcc | `0x100000` / `0x1f4200` | 上游同值 | ✓ |

位域：GDSC `0x9108` mask `0xf007` / `0x8000`，状态 `0x953c` bit31；PLL 两组
`LUCID_OLE` 的 9 个偏移 `{0x10,0x14,0x20,0x24,0x28,0x1c,0x30,0x34,0x38}` 与值表；
root mux cfg `0x100602/0x100603/0x100001` + mask `0x10371f`；7 条 branch
`{0x9004,0x9144,0x93e8,0x913c,0x9148,0x911c,0x7000}`；3 条 root
`{0x9318,0x93ec,0x9010}`；`0x9534` 清零——**与 `PongAdrenoClocks.c` 逐字一致**。

### 命题不成立的三处（仿真已复现）

| # | 0179 的写法 | vendor/上游语义 | 后果 |
|---|---|---|---|
| 1 | GCC `0x81004` 也去等 CLK_OFF(bit31) 清零 | HWCG 模式下 bit31 保持置位、且 `bits0..1==3` 即视为已开 | 健康硬件被判 `halted`，M2 直接中止 |
| 2 | GPUCC `0x911c` 也去 poll | vendor 把它标为 delayed：使能后只等 10us，**不 poll** | 同上，误报 halted |
| 3 | root mux 不看 update 位就写 | update 位仍置位时不得重配（会静默丢配置） | 速率配置被吞，时钟跑错频 |

另有一处**顺序**问题：0179 先读 kgsl（GX 域，GDSC 关着时读它，NoC 可能直接挂死
且无任何输出），再读 gmu/gpucc/ao。一次读挂死就什么都拿不到。

### `0180` 的改动

- 抽出 `sweet_adreno_branch(blk, off, delayed)`，三段语义一次说清（CLK_OFF 清 /
  NOC FSM `0x2...` / HWCG 3 / delayed 不 poll）。
- root mux 写入前检查 update 位，命中就停下并打印。
- 探测顺序改为 gpucc → gmu_ao → gmu →（`sweetadreno_kgsl=1` 才读）kgsl，每段先打
  一行「要读哪个块」，挂了也知道挂在哪。
- 日志里明确写出 chipid 是 DT 常量。
- M1 路径依旧**零写寄存器**；上电序列仍只在 `sweetadreno_power=1` 下跑。

## 4. 本地校验方法（可复跑）

`tools/adreno-selftest/`（已并入补丁仓库）：把补丁里的 `SweetAdreno.cpp` 原样编译，
`ml_io_map()` 返回一块假寄存器文件，寄存器行为（CLK_OFF、GDSC 状态位、mux update）
用 hook 模拟。

```
cd tools/adreno-selftest
# A. 按真实架构编译（保住 dsb sy 内联汇编）
clang++ --target=aarch64-none-elf -std=c++17 -O2 -DSWEET -I stubs -ffreestanding \
        -fno-exceptions -fno-rtti -c build/SweetAdreno.cpp -o /tmp/a.o
# B. x86 仿真（只把 dsb 屏障与裸指针访问换成函数）
./run.sh 0179   # 旧版
./run.sh 0180   # 新版
```

实测结果（`0180` 已全绿）：

| 场景 | 0179 | 0180 |
|---|---|---|
| happy | `clock sequence returned 0` | 同 |
| HWCG（0x81004 保持 bit31、bits=3） | `GCC branch 0x81004 halted (0xa0000003)` | `GPUCC up` |
| delayed（0x911c 不清 CLK_OFF） | `branch 0x911c halted (0xa0000000)` | `GPUCC up` |
| busy（0x9318 update 位残留） | 继续写 mux（静默丢配置） | `root mux 0x9318 update already pending, stopping` |

版本矩阵（`./run.sh <补丁号>` 的通过项数）：

| 重建到 | 默认 | `K60_ALLOW_MANGLED=1` | 说明 |
|---|---|---|---|
| 0179 | 0（链接守卫拦下） | 3 | 3 项失败：hwcg / delayed / busy |
| 0180 | 0（同上） | 6 | 时钟语义全修好 |
| 0181 | 0（同上） | 10 | 加预检与收尾验证 |
| 0182 | 0（同上） | 12 | 加分阶段与快照 |
| 0183 | 12 | 12 | 修完 `extern "C"`，守卫放行 |
| 0185 | 14 | 14 | 加固件自检（0183 之后才有意义） |

0179–0182 是历史版本，本身带着 C++ 修饰符号，链接守卫会直接拦下；回看它们的
时钟语义时用 `K60_ALLOW_MANGLED=1`。

附带证据：`gpu-fw/gmu_gen70000.bin`（57,272 字节）用上游 `AdrenoFirmwarePlanGmu`
+ A730 的三段区域解析，返回 success、15 个块，ITCM 段 0x1dc、0x4000 段 0xdcac、
`0x10007f98`/`0x10007fdc` 两个数据块都在——M3 的固件输入是结构合法的。

## 5. M1 上机判据（设备回来照这个看）

启动参数只加 `sweetadreno=1`（不加 `_power`，M1 全程不写寄存器）。

| 日志 | 通过条件 | 不通过说明什么 |
|---|---|---|
| `SWEET ADRENO: maps gpucc=… ao=… gmu=…` | 三个都非 0 | 有 0 = `ml_io_map` 失败：地址不在 physmap/ADT 覆盖内 |
| `reading gpucc (CX domain, safe)` + 4 行 `gpucc[i]` | 能打完 4 行 | 卡死/重启 = GPUCC 也在被关断的电源域，M2 必须先做 GDSC 再读 |
| `gpucc[i]` 的值 | 不是全 `0xffffffff` | 全 `0xffffffff` = 该块无时钟/NoC slave error；全 0 = 未上电或 XPU 挡 |
| `reading gmu_ao_blk_dec0` + 4 行 `ao[i]` | 能打完 | 卡死 = AO 域假设错了，回头看 DT 的 reg[2] |
| `reading gmu (may need CX's GDSC)` + 4 行 `gmu[i]` | 能打完 | 卡死在这里 = GMU 需要 GDSC/时钟，M2 顺序要调整 |
| `chipid is the DT constant 0x7030002 …` | 出现即可 | 这只是说明行，**不要**当硬件读到的 chipid |
| `kgsl read skipped` | 正常 | 想冒险读 GX 域再加 `sweetadreno_kgsl=1` |
| `SWEET ADRENO: probe done` | 出现 = **M1 通过** | 没出现 = 上面某一行之后断了，按断点定位 |

M1 通过后：`SWEET ADRENO: probe done` 之后再加 `sweetadreno_power=1` 看停在哪个
phase；`GPUCC up` = 时钟全开，可以进 M3（GMU 固件装载）。
若 M2 打的是 `GDSC already owned by hardware, stopping`，那是 bootloader 已经开过
GDSC 的好消息，别当成失败，直接往 M3 排。

## 6. 构建命令（已在本机跑通一次）

```
cd ~/hackios
# 内核换成交付的那颗（含 0179/0180），cmdline 追加 sweetadreno=1
K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_verbose=1" \
  ./tools/k60-build-boot.sh artifacts/kernel.development.sweet artifacts/xnu-adreno.img
fastboot boot artifacts/xnu-adreno.img      # M1 只读，风险低
./tools/k60-log-pull.sh                     # TWRP 里拉日志环
```

手工三步（脚本内部就是这三段，需要绕开脚本时用）：

```
ADT_PHYS_BASE=0xb4000000 ADT_MEM_SIZE=0x2c000000 \
  python3 ios-sweet/tools/mkadt.py /tmp/k60.adt --k60
python3 ios-sweet/tools/mkpayload.py <kernel> /tmp/k60.adt /tmp/k60-payload.bin \
  --cmdline "debug=0x8 keepsyms=1 cpus=8 -noprogress serial=1 sweet_notests=1 \
rd=ufsroot0 ufsroot=userdata ufsrw=1 amfi_get_out_of_my_way=1 \
cs_enforcement_disable=1 amfi_allow_any_signature=1 ufslogsecs=5 \
sweetadreno=1 sweetadreno_verbose=1"
cd ios-sweet/shim && HDRVER=4 CFLAGS_EXTRA="-DK60_XNULOG -DK60_NO_PHYSMAP_EXT" \
  PAYLOAD=/tmp/k60-payload.bin ./build.sh && cp out/shim-boot.img /tmp/xnu-adreno.img
```

本轮验证产物：`artifacts/xnu-adreno-cmd.img`（16,891,904 字节），镜像内 cmdline 实测含
`… ufslogsecs=5 sweetadreno=1 sweetadreno_verbose=1`。**注意它用的是现有
`kernel-accel.sweet`，不含 Adreno 代码**，只用来证明打包链路可用；真正要刷的必须是
CI 编出 0180 之后的内核。

## 7. 还缺什么

- xnu 只能在 macOS 上编：0179/0180 的最终编译确认要靠 CI（本轮没有推送任何远端）。
- M3 之后（GMU 装载、ZAP 认证、CP 微码）全部是新地，上游自己也没确认过运行时。
- 代理/Gitee 与本文件无关，见本轮汇报。

## 8. 一次启动走完 M1–M4：分阶段诊断（0182）

设备不是调试器，所以把阶段做成运行时参数，一次启动里累积执行、每段都留下判据行
和同一组寄存器快照：

| `sweetadreno_stage` | 做什么 | 关键判据行 | 失败停在哪 |
|---|---|---|---|
| 1（`sweetadreno=1` 默认） | 只读探测 gpucc/ao/gmu | `probe done` | 哪一段 `reading ...` 之后断了就是那块 |
| 2（或 `sweetadreno_power=1`） | GPUCC 上电 + 收尾自检 | `GPUCC verified: n/7 branches running` | 停在最后一个 phase |
| 3 | 只读判断 GMU 处在哪一步 | `GMU is held in reset` / `FW_INIT OK` / `HFI is up` | 只读，不会失败，只给状态 |

快照在 `end-m1`、`pre-power`、`post-power-ok`、`post-power-fail`、`m3-readonly`
五个点各打一遍，字段固定：`gdsc`（0x9108）、`hwctl`（0x953c）、`tcs`（0x9534）、
`br0..br6`（七条 branch）、`gmu syreset/boot/finit/hfi`。任何阶段失败时，日志里
已经有一份完整现场，不用回头补探针。

## 9. GMU 寄存器偏移的陷阱（实证，会直接踩坏）

上游 `gen7_reg.h` 里的 GMU 常量（`CM3_SYSRESET=0x1f800` 这种）**不是块内偏移**，
是"以 kgsl 基址 `0x3d00000` 起的 dword 序号"，上游换算函数就是
`offset(w) = w*4 - 0x68000`（0x68000 = GMU 基址 0x3d68000 − kgsl 基址 0x3d00000）。

| 常量 | w | 块内偏移 `w*4-0x68000` | 绝对地址 |
|---|---|---|---|
| `CM3_SYSRESET` | 0x1f800 | 0x16000 | 0x3d7e000 |
| `CM3_BOOT_CONFIG` | 0x1f801 | 0x16004 | 0x3d7e004 |
| `CM3_FW_INIT_RESULT` | 0x1f81c | 0x16070 | 0x3d7e070 |
| `HFI_CTRL_STATUS` | 0x1f980 | 0x16600 | 0x3d7e600 |

若直接拿 `w*4` 当偏移（0x7e000）会读到 0x37000 的 GMU 窗口之外。0182 里统一用
`SWEET_GMU_OFF()`，并且快照在越界前拒绝读。

## 10. M3 预演：固件 + GMU 引导模型（`tools/adreno-selftest/gmu-model.c`）

不是内核代码，是把 Pong 的计划逐步执行 + 一个按 gen7 语义写的最小 GMU 模型，
跑的是**真固件**：

```
固件 gpu-fw/gmu_gen70000.bin：57272 字节
  ok    TOC 解析 / 四个 payload 段 / ITCM 0x1dc + cache 0xdcac
  ok    ITCM 首字是栈指针 0x10007f80（PC=0x105，落在 ITCM 段 0x1dc 内）
  ok    BOOT_CONFIG=2 + 合法向量表 → 固件启动 → FW_INIT 0x100 → HFI 建立
  ok    BOOT_CONFIG≠2 → 拒绝启动（对应上机看到的 FW_INIT 超时）
  ok    截断固件 → 解析失败
  ok    不在复位状态 → 拒绝重入（防止踩到已在跑的固件）
11 项全过
```

**内核侧的 M3（真的写 ITCM/DTCM、`CM3_SYSRESET=0`、轮询 `FW_INIT`）还没实现**，
它取决于 §12 那个「固件怎么进内核」的选择，所以这里只标 unverified。

## 11. 证据台账

| 结论 | 状态 | 证据/验证方法 |
|---|---|---|
| 五块 MMIO 地址与 DT reg 一致 | verified | `dump/live.dts` 逐条比对（§3 表） |
| GPUCC 位域/偏移与 vendor 一致 | verified | 与 `PongAdrenoClocks.c` 逐项 diff |
| 时钟序列**写集合**与上游一一对应 | verified | 仿真转储 48 次写，逐条比对上游 update 调用 |
| 3 处时钟等待语义缺陷 | verified | 仿真 0179 三项 FAIL（§4 表） |
| 0181 预检 + 收尾验证 | verified | 仿真 `prepowered` / `stuck` 两条路径 |
| M1 不开开关时零 MMIO | verified | 仿真 `ASSERT no-mmio: ok` |
| 映射失败不崩、只打印并返回 | verified | 仿真 `mapfail` 场景，0 次写 |
| M1 只读全程零写寄存器 | verified | 仿真 `ASSERT zero-writes: ok` |
| GMU 固件结构合法 | verified | 上游 `AdrenoFirmwarePlanGmu` 15 块 + 本地模型 11 项 |
| GMU 引导寄存器序列 | **unverified**（模型层通过，内核未实现） | 实现 0183 后跑 `stage=3` + 仿真 |
| 上游 A730「已跑通」 | **unverified** | 英文 README 说 command submission/Metal discovery，中文 README 一处说未确认、表格又打 ✅；无 CI、无 issue、无日志文件；本地仓是浅克隆，**提交历史不能当反证**。验证方法：要他们的日志/抓包，或自己上机复现 |
| 任何真机寄存器读数 | **unverified** | 需要设备；判据见 §5、§8 |
| CI 首次构建失败原因 | verified | `nm -u` 证据：0182 的对象引用 `_Z5delayi`/`_Z9ml_io_mapmm`，内核里叫 `delay`/`ml_io_map`；0183 变成未修饰名。CI run 40275117403 的 build 步骤 7m05s 失败 |
| 0183 修复后本地全绿 | verified | `./run.sh 0183` 12/12；链接期守卫把 0182 直接判 FAIL |

## 11.1 第一次 CI 失败：C++ 名字修饰（已修，0183）

`iokitcpp` 这一档是按 C++ 编的。文件里原来写的是

```c
extern vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);
extern void delay(int us);
```

C++ 下这两个会变成 `_Z9ml_io_mapmm` / `_Z5delayi`，而内核导出的是 C 符号
`ml_io_map` / `delay`，链接期找不到。其它 Sweet 文件里调用 C 代码都包了
`extern "C"`，这个驱动漏了。

本地复现（不靠猜）：

```
$ nm -u sa-0182.o | grep -E 'ml_io_map|delay'
                 U _Z5delayi
                 U _Z9ml_io_mapmm
$ nm -u sa-0183.o | grep -E 'ml_io_map|delay'
                 U delay
                 U ml_io_map
```

0183 把两行包进 `extern "C" { }`；校验台新增链接期守卫：只要未定义符号里还有
`_Z` 修饰名（运行期的 `_Zd*`/`_Zn*` 除外）就直接 FAIL，防止同类问题再推到 CI。

## 12. 需要拍板：GMU 固件怎么进内核（决定 M3 能否落地）

固件 `gmu_gen70000.bin` 57,272 字节，内核要把它搬进 GMU 的 ITCM/DTCM。

- A. 编成 C 数组随内核一起走（上游 Pong 就是这么干的）：一次刷机就够，补丁大 57KB；
- B. 由 boot payload 追加一段、按 boot-arg 传物理地址：补丁干净，但要改 `mkpayload`/shim；
- C. 先不做，M3 继续只读。

**决定：A，已落地（`0185-k60-adreno-gmu-firmware.patch`）。**

- `tools/mk-gmu-fw.py gpu-fw/gmu_gen70000.bin > SweetAdrenoGmuFw.h`：
  57,272 字节 → 296,573 字节头文件（数组 + 4 段段表），生成时回读校验字节一致、
  段表与长度自洽。
- 0185 把 `iokit/Kernel/arm/SweetAdrenoGmuFw.h` 加进补丁序列，并在
  `sweetadreno_fwinfo=1` 下只读打印：`firmware 57272 bytes in kernel, 4 segments`、
  四段地址/长度/目标，以及从数组里读出的向量表
  （实测 `sp=0x10007f80 pc=0x105 plausible`）。整个过程零 MMIO，可以和 M1 同一次启动验掉。
- **上传本身还没做**：cache 段（0xdcac 字节）必须落在 GPU 可见内存、HFI 队列需要
  IOVA，这要 kgsl SMMU；在那之前写 ITCM/CM3_SYSRESET 只是半条路，`0185` 不做。

## 13. CI 产物实测（2026-10-26，run 40276193603）

结论先说：**CI 的 `Build` 步骤是红的，但产物是可用的内核，且含 0179–0183 全部代码。**

| 项 | 值 |
|---|---|
| run | `40276193603`（`2792bf6`，含 0179/0180/0181/0182/0183） |
| 产物体积 | `xnu-kernel` 94.0 MB（整包）；内核本体 `kernel.development.sweet` 17,303,824 字节 |
| 内核头 | `Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|PIE>` |
| `NOUNDEFS` | 链接期没有未定义符号 → 0183 的 `extern "C"` 修复生效（0182 会带着 `_Z5delayi` 挂在这里） |
| 字符串 | `SWEET ADRENO: probe done` 1、`GPUCC up` 1、`preflight` 3、`snapshot` 3、`chipid is the DT constant` 1、`kgsl read skipped` 1、`clock sequence returned` 1、`branch` 1 |
| 对照 | `SWEET SCALER`（0177）2、`SWEET ACCEL`（0178）3 也在 |

两个由此更正的事实：

1. **CI 不需要"绿"也能出内核。** `Collect` / upload 步骤是 `if: always()`，`Build` 失败
   之后仍会把 `out/` 打包上传。内核既然 `NOUNDEFS` 且带新字符串，说明失败发生在
   **编译/链接之后**（打包或附加目标），不影响我们要的东西。
2. **这个红不是 Adreno 补丁引起的。** 最近 25 次 run 全红；最后一次 success 是
   2026-10-23 05:31（`e6f1b79`），而 `0958d4f`…`e1afab0`（都早于 0179）同样是
   `Build` 5.5–6 分钟失败。所以「等 89MB 成功」这个判据本身不成立，应改成
   「看 `xnu-kernel` 里有没有当轮的字符串 + 内核准入符号」。
   红的具体原因仍在查（日志包分段下载中），查到后补在这里。

由此得到的可刷镜像（本轮实际生成，未上机）：

```
artifacts/kernel-adreno-0183.sweet   17,303,824 字节   003-0183 的 CI 内核
artifacts/xnu-adreno-m1m3.img        16,896,000 字节   cmdline 含
                                     "sweetadreno=1 sweetadreno_stage=3"
```

## 14. 0168 的处置：已删除（有成本与价值的取证）

`0168-k60-grant-logd-priv.patch` 从落地起每次都被跳过：

```
05:49:36 [warning] Skipping patch .../0168-k60-grant-logd-priv.patch
         (already upstream, previously applied, or malformed):
         error: patch failed: bsd/kern/sweet_amfi.c
05:49:36 error: bsd/kern/sweet_amfi.c: patch does not apply
```

**成本取证**（决定要不要为省时间而删）：CI 日志里补丁循环从 05:49:34.859 到
05:49:36.817，**155 条补丁合计 1.96 秒，单条中位 10.7ms**。跳过的补丁同样只花一次
`git apply --check` 的钱，删掉省下 ~0.01 秒，对一次 ~330 秒的构建可以忽略。
所以「加编译时间」这条不成立。

**价值取证**：它想授的是 `PRIV_GLOBAL_PROC_INFO`（logd 读别的进程记账）。而当前树
（`0064` 之后）是「只放行 `PRIV_WORK_INTERVAL`，其余一律 EPERM」，这个状态下项目
已经跑到 SpringBoard、守护进程正常——说明这个权限**不是**启动所需；反过来，宽授权
（`7e79868` 记录的那版）会让启动在几秒内死掉，是有害的。

**定论：无价值 → 删除**（`git rm` 语义，理由留在本条）。同时不再把它写进任何待办。
删掉之后树里 `sweet_sandbox_priv_grant` 仍是已验过的 deny-all 版本，行为零变化。

### 14.1 日志抓取的限制（记录在案）

两种协议都在 ~4MB 处被掐断，是链路/服务端对流长度的限制，不是 curl 参数问题：

```
HTTP/2 : curl: (92) HTTP/2 stream 1 reset by server (error 0x8 CANCEL)  4,034,712 字节处
HTTP/1.1: curl: (56) OpenSSL SSL_read: unexpected eof while reading     3,999,387 字节处
```

`05:53:52` 那一刻（Build 失败点）的原文这一侧拿不到；`gh run view <id> --log-failed`
在高速链路上可取。**不阻塞**：内核产物已 `NOUNDEFS` 且含 0181/0182 字符串，说明失败
发生在内核产出之后。

## 15. 待拍板清单（2026-10-26 收尾）

| # | 事项 | 选项 | 建议 |
|---|---|---|---|
| 1 | GMU 固件进内核（M3 前置） | **已定 A：编成 C 数组**（0185 落地） | 已执行 |
| 3 | 是否把这些提交推到远端 | 推／不推 | 推，推完 CI 出带 0185 的内核 |

第 3 项等内存侧把提交归属理完再一次性推；现有镜像
（`artifacts/xnu-adreno-m1m3.img`）已可刷，不含 0185。

## 16. 本轮最终状态（2026-10-26 收尾）

| 项 | 状态 |
|---|---|
| 补丁序列 | 0179 探测 → 0180 时钟语义 → 0181 预检/验证 → 0182 分阶段/快照 → 0183 C 链接 → 0185 固件入内核（`0168` 已删） |
| 本地自测 | `./run.sh 0185` 14 项全过；GMU 引导模型 11 项全过 |
| CI 内核 | `artifacts/kernel-adreno-0183.sweet`（17,303,824 字节，`NOUNDEFS`，含 0179–0183） |
| 可刷镜像 | `artifacts/xnu-adreno-m1m3.img`，cmdline `sweetadreno=1 sweetadreno_stage=3` |
| 未推 | 本轮全部提交（等内存侧修完归属） |
| M3 上传 | 未实现，卡在 kgsl SMMU（下一个大件） |
| 真机任何读数 | **unverified**，判据见 §5 / §8 / §16.1 |

### 16.1 设备回来后的执行顺序

```
cd ~/hackios
# 1) 用最新内核重打镜像（含 0185 时需要重新拉 CI 产物）
K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3 sweetadreno_fwinfo=1" \
  ./tools/k60-build-boot.sh artifacts/kernel-<最新>.sweet artifacts/xnu-adreno.img
fastboot boot artifacts/xnu-adreno.img          # M1+M2+M3 只读，一次启动

# 2) 日志
./tools/k60-log-pull.sh                         # TWRP 里
python3 tools/k60-log-summary.py <log>

# 3) 按顺序读判据
#    fw vector sp=0x10007f80 pc=0x105 plausible   → 内核里那份固件对
#    probe done                                    → M1 通过
#    snapshot[pre-power] …                         → 上电前现场
#    GPUCC verified: n/7 branches running          → M2 结果（0/7 = 失败）
#    GMU is held in reset / FW_INIT / HFI          → M3 现状
# 4) 只有上面都绿，才谈 M3 上传（需要 SMMU 先落地）
```

判据速查：`gpucc[i]` 全 `0xffffffff` = 该块无时钟/NoC 错；`GDSC is owned by firmware`
= bootloader 已上电（好消息，看 preflight 的 n/7）；`0/7 branches running` = 时钟没起来。

---

## 17. 第一次真机上机（2026-10-26 16:38）：门禁失败，Adreno 一行都没跑出来

> 结论先行：**镜像里确实有 Adreno（0183），但这一轮没有验证到它。** 启动在
> `SWEET TOUCH: gcc probe vote read` 处被打断，日志里 `SWEET ADRENO` = 0 条，
> 内存门禁（41271 页）也没过（实际 55885 / 2611）。**本轮对 GPU 作废**，
> 先修 boot_args/ADT 的交付，再谈 GPU；p1/p2 继续禁刷。

### 17.1 镜像核对（离线可复跑）

`artifacts/xnu-adreno-m1m3-verify.img`（16,896,000 B，16:19 由备弹构建）：

```bash
grep -ac "SWEET ADRENO" artifacts/xnu-adreno-m1m3-verify.img     # 50
grep -ac "SweetAdreno"  artifacts/xnu-adreno-m1m3-verify.img     # 27
# payload @0x30000: phys_base=0xb4000000 mem_size=0x2bf00000(703 MiB)
#                   adt_off=0xfe7e40 adt_size=0x2774
# ADT: dram-base=0xb4000000 dram-size=0x2bf00000
# cmdline 末尾: sweetadreno=1 sweetadreno_stage=3 sweetadreno_fwinfo=1
```

- 内嵌 payload 的 10 个段与 `artifacts/kernel-adreno-0183.sweet` **逐段 sha1 相同**
  （Mach-O 里的第 11 个 `__CTF` 与 `__LINKEDIT` 共用起始 VA，不是独立 payload 段）。
- IODeviceTree 匹配表里有 `'IOClass' = SweetAdreno; 'IOProviderClass' = IOResources;
  'IOMatchCategory' = SweetAdreno`。
- 仍**不含 0184/0185**（`sweetadreno_fwinfo`、`GMU firmware embedded` 均 0 处），
  所以 `fwinfo` 这次注定无效。

### 17.2 日志环是哪次启动的（机制，先记住再看数）

- `sweet_log_thread`（0047/0169）每 `ufslogsecs` 秒把 `sweet_log_total` 之前的
  console **从 ring 偏移 0 线性写盘**，header 的 `seq` 自增；而 `sweet_log_flush_now`
  （panic 路径，0162/0171）固定写 **`.seq = 1`**。
- 拉到的两份环头都是 `seq 1`（`total` 128771 / 134545），且内容第一行都是
  `SWEET-STAGE-40`（kprintf 第一句）→ **环内容就是 panic 那次启动自己的 console**，
  不是上上次残留。
- 0171 的 `printf("SWEET LOG: panic flush, total …")` 在 `total` 已经取完之后才打印，
  **不会**被写进这次 flush 的范围 → 环里没有这行是正常的，不能当"flush 没跑"。

### 17.3 panic 定位（这条是确定的）

```
pc 0xfffffff0074f2f98 − text base 0xfffffff007004000 = 0x4EEF98
→ kernel-adreno-0183.sweet: _touch_thread+0x178  (scalers/accel 三颗内核同址同指令)
   fffffe00074f2f8c: ldr x8, [x19, #2072]    ; 取 st.gcc（touch 的 gcc 映射）
   fffffe00074f2f94: movk w24, #5, lsl #16   ; w24 = 0x5200c
   fffffe00074f2f98: ldr w23, [x8, x24]      ; ← 故障读
```

- `x8` 就是日志里那句 `SWEET TOUCH: gcc map 0xfffffe3019470000`；
  `+0x5200c` → `far = 0xfffffe30194c200c`，落在 `ml_io_map(gcc, 0x60000)` 窗口内
  → **不是缺页**。
- `esr 0x96000010` = 同 EL data abort、WnR=0（读）、DFSC=`0x10` = **同步外部中止**
  （总线/XPU/电源域拒绝），和 0173 注释里的 "XPU abort" 一致。
- 目标寄存器是 `TOUCH_GCC_VOTE = 0x5200c`（0060 补丁 245–248 行），即 **GCC+0x15200c**
  的 GPU_CC GDSC/vote 寄存器。**不是 Adreno 的 ioremap，也不是 0184 的 notch。**
- 照片上的数字：内核格式串是 `pc: 0x%016llx … far: 0x%016llx`，`far` 只能是 16 位；
  转写的 `0xfffffff0a3019c200c`（18 位）必然有错位，以上面的指令 + 日志映射为准。
- 照片里 `GEN1/GEN5/DMR/Metadata/Bits maps/Extra: 0 - 0` = 日志 **44–52 行**的
  `zone_init: map GEN1` / `metadata` / `metabits` / `extra 0:0 (0B)`，不是异物。
- `Memory: 1076` = panic 头的 `Memory ID: 0x434`（0x434=1076）；内核里没有
  `Memory: %d` 这种格式串（只有 `Memory ID: 0x%x`）。

### 17.4 内存门禁：失败（本轮作废的直接原因）

> **⚠ 2026-10-26 17:0x 更正：本节结论作废。** 第 16 行的 `55885` 不是这次启动的
> 输出，而是**上一次启动**的 console 被本次启动重打进来的（详见 **§18**）。
> 正确表述是：**本轮没有测到门禁**，而不是"门禁失败"。下面这段保留作反面教材，
> 但**不要**再据此去查 shim/ADT 的交付链。

`/tmp/verify.txt:16`：

```
vm_page_bootstrap: 55885 free pages, 2611 wired pages, (up to 0 of which are delayed free)
```

- 期望 41271（±10），实际 **55885**；且**与旧日志（128771 B）这一行完全相同**。
- `55885 + 2611 = 58496` 页 = **914 MiB = 0x39200000**，正好是旧 `artifacts/k60.adt`
  的 `dram-size`；而 verify.img 的 ADT 是 `0x2bf00000`（703 MiB，上限 44992 页）。
  → 内核看到的 `dram-size` **不是**镜像里那份。
- 0172 的 cap 代码在 0183 里**确实存在**：
  - `0xfffffe000748b478 ldr x9, [x1, #728]`（`args->memSizeActual`）
  - `0x748b494 b.hs`；`0x748b4a4` 引用 `"SWEET MEM: physmap %llu MiB, VM manages %llu MiB (dram-size)"`
  - 但**两份日志都没有这行** → 分支没走，`memSizeActual` 读成了 0。
- `boot_args` 是被拷进内核的 `_const_boot_args` 再用的，拷贝长度 **736**
  （`0xfffffe0007ae226c mov w2, #736` → `memmove`），728 在拷贝范围内。
- 镜像里的 shim 就是当前源码编的（前 0x30000 字节 sha1 前 16 位 `cef6c5421ce4622e`，
  与 `ios-sweet/shim/out/shim-boot.img` 一致），二进制里**确有**对 `+728` 的 store
  （文件偏移 `0x46ec`），`loader.c` 里也写着 `ba->memSizeActual = p->mem_size;`。
- **矛盾**：shim 写 `0x2bf00000`、内核拷 736 B、0172 读 728 —— 却读出 0，
  且 `dram-size` 看着是 914 MiB。**这一步必须先打掉，否则所有内存判据都是假的。**

**修正上一轮的结论**：先前说"旧日志是旧 ADT 镜像、panic 那次镜像后来被重生成" —— **错**。
两份日志的 55885 与 ADT 无关（两次镜像 ADT 不同、数字一模一样），真正的问题是
内核看到的 boot_args/ADT 值不是 shim 刚写进去的。旧/新日志的 cmdline 不同
（旧无 `fwinfo`、新有），确实是两次不同镜像的启动，但内存数字同源。

### 17.5 为什么"没有 SWEET ADRENO"不能直接判"驱动没跑"

- 同一份环里 `SWEET SCALER/ACCEL … stub registered` 也**没有**，但这两个类确实启动了：
  `verify.txt:643–648` 有 backboardd 匹配 `AGXAccelerator`，`716–772` 打开
  `AppleM2ScalerCSCDriver`；而它们在 0183 里的 `start()` 确实调了 `IOLog`
  （`0xfffffe0007a76d6c` / `0x7a76d78 bl _IOLog`）。
- 所以 **start 期的 IOLog 有丢失**。要判定 Adreno 到底跑没跑，必须
  ①让启动活过 45 s（`sweettouchoff=1`），②最好在 probe 里加一条不依赖 IOLog 的证据
  （例如往固定物理页写魔数，或让后续读取者打印）。
- 弱证据：若 `stage 2` 真的把 GPU_CC 带起来了，touch 对 `GCC+0x15200c` 的读
  大概率不会再外部中止；现在仍死在同一行 → **至少 stage 2 没把 GPU_CC 拉起来**。

### 17.6 下一步（按顺序；确切命令）

**第 0 步（必须先做）**：打一颗只加 6 行打印的诊断内核，把交付链路钉死。

```c
/* osfmk/arm64/arm_vm_init.c，函数最开头 */
printf("SWEET BOOTARGS: memSize=%llu MiB memSizeActual=%llu MiB base=%llu MiB "
    "dt=%p dram_arg=%llu MiB\n",
    args->memSize >> 20, args->memSizeActual >> 20, args->physBase >> 20,
    (void *)args->deviceTreeP, memory_size >> 20);
```

```bash
cd ~/hackios
MEMPLAN=p0 K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3" \
  tools/k60-build-boot.sh <带上面打印的内核>.sweet artifacts/xnu-bootargs-probe.img
adb reboot bootloader && fastboot boot artifacts/xnu-bootargs-probe.img
# 回来进 TWRP 后：tools/k60-log-pull.sh /tmp/bootargs.txt
```

判读：

| 现象 | 结论 | 下一步 |
|---|---|---|
| `memSizeActual=0` | shim 的 store 没落到内核看到的那份 boot_args | 查 shim 的 `_end`/payload 搜索、`PAYLOAD_SAFE_PA`、结构体偏移一致性 |
| `dram_arg=914 MiB` | 内核读到的 ADT 是**旧的**（`0x39200000`），不是 shim 拷的那份 | 查 `adt_pa = kernel_end + (phys_base - virt_base)` 与 `deviceTreeP` 的 VA 换算 |
| `memSize=1216 MiB` 且上面两项都对，仍 55885 | 0172 之后还有别的 clamp/static 吃页 | 再量 `avail_start/avail_end`（再加两行打印） |

**第 1 步**：拿到一张真活过 45 s 的启动（这样才可能看到 Adreno）：

```bash
cd ~/hackios
MEMPLAN=p0 K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3 sweettouchoff=1" \
  tools/k60-build-boot.sh artifacts/kernel-adreno-0183.sweet \
  artifacts/xnu-adreno-0183-notouch.img
```

`sweettouchoff=1` 由 0173 支持，touch 直接 return，`GCC+0x15200c` 不会被碰。

**第 2 步**：判据顺序（这次是真的能判的版本）：
`probe done` → `snapshot[pre-power] gdsc=… hwctl=…` → `GPUCC verified: n/7` →
GMU 三段（`held in reset?` / `FW_INIT` / `HFI`）。`0/7` 或 `gdsc` 读成 `0xffffffff`
都是"该块没时钟"，别急着解释成固件问题。

**第 3 步**：`p1/p2` 在内核没 0184 notch 前继续禁刷；`K60_RDCHECK` 探针
（`artifacts/xnu-rdcheck.img`，18 个地址版，120 s 自动回 fastboot）可随时插队，
但它只回答"4 GiB 以上那个洞是不是死的"，与 GPU 无关。

**第 4 步**：0185（GMU 固件）仍不在 CI 产物里，`sweetadreno_fwinfo=1` 在此之前
只会得到 "fwinfo 无效"。

### 17.7 本轮证据台账

| 项 | 值 |
|---|---|
| 新日志 | `/tmp/verify.txt`，134,545 B / 1,671 行，16:38 拉取 |
| 旧日志 | `/tmp/k60-adreno1.txt`，128,771 B / 1,570 行 |
| 门禁行 | 两份都在**第 16 行**：`55885 free pages, 2611 wired pages` |
| 新日志关键行 | 123（previous-boot dump）· 643–648 / 716–772（backboardd 匹配 stub）· 1668–1671（touch 死亡） |
| 旧日志关键行 | 547/590/853（`flushed … seq 1/2/3`，证明周期 flusher 会累加 seq）· 1567–1570 |
| 镜像 | `artifacts/xnu-adreno-m1m3-verify.img`、`artifacts/xnu-adreno-m1m3.img`、`artifacts/kernel-adreno-0183.sweet` |
| 源码/补丁 | `ios-sweet/shim/loader.c:180–215`、0047/0156/0162/0171/0174、0060:245–248、0172/0173 |
| 反汇编 | `0x748b478`（读 728）· `0x748b4a4`（cap 打印）· `0x7ae226c`（memmove 736）· `0x7a76d6c/0x7a76d78`（stub 的 IOLog）|
| 设备 | 序列号 `ffec9d48`，停在 **TWRP**（`3.7.1_12-RedmiK60_v9.0_A15`），本轮只拉过日志，未刷未写 |

复跑最小集：

```bash
grep -n vm_page_bootstrap /tmp/verify.txt
grep -ac "SWEET ADRENO" /tmp/verify.txt            # 0
tools/sym.py --kernel artifacts/kernel-adreno-0183.sweet 0xfffffe00074f2f98
python3 ios-sweet/tools/adt.py dump /tmp/adt-m1m3.bin | head -20
```

---

## 18. 2026-10-26 17:0x 更正：§17.4 的门禁"失败"是**读错行**，本轮门禁其实没测到

> 一句话：`/tmp/verify.txt` 的**前 13,531 字节不是这次启动的 console**，是上一次启动
> 的 console 被本次启动重打进来的。§17.4 拿这一段的第 16 行当"本次启动的页数"，
> 于是得出"内核看到的 ADT 不是 shim 写的那份"——**这个结论没有任何证据支持**，
> 连带 §17.6 的第一步（打诊断内核查交付链）也失去前提。

### 18.1 证据 1：两份日志前 176 行**逐字节相同**，含不可能重复的计时

```
$ diff <(sed -n '1,140p' /tmp/k60-adreno1.txt) <(sed -n '1,140p' /tmp/verify.txt) | wc -l
0
$ diff <(sed -n '1,176p' ... ) ...                       # 同上，0 行差异
```

两次不同镜像、不同 cmdline 的启动，不可能都打印
`pmap_startup() init/release time: 3626 microsec`（第 14 行）—— 这是微秒级实测值。
两份日志的第 14/16 行同值同理。**第一次出现差异是第 177 行**：

```
176: SWEET AKS: suggestd sel 17 arg0 0 in 1/0 out 0/4096 -> 0x0
177: SWEET AKSSWEET LOG: previous boot, last 2644 bytes (seq 1)
```

### 18.2 证据 2：dump 区有明确边界与长度

| 项 | 值 |
|---|---|
| `SWEET LOG: end of previous boot log` | `verify.txt` 第 **227** 行，结束于第 **13,531** 字节 |
| 本次启动自己的第一条 console | 第 **228** 行（字节 13,592）`SWEET KEYS: unsupported arbiter version=0x70000000 apids=768` |
| 之后紧跟 | 229 `BSD root: ufsroot0` · 232 `SWEET LOG: console log to the last 4100 KiB` · 233 `SWEET LOG: thread running` · 234 `SWEET TOUCH: starting in 45 s` |
| 文件总长 | 134,545 B（与 header 的 `total` 一致，没有被截断）|

`grep -n vm_page_bootstrap verify.txt` → **只有第 16 行**。也就是说：**本次启动自己的
`vm_page_bootstrap` 从来没有进过这份环**，不是"值是 55885"。

### 18.3 根因：`0152-k60-log-dump.patch` 的 dump 把环的起点洗掉了

`bsd/dev/sweet_ufs.c: sweet_log_dump()`（0152，第 66–76 行）做的是：

```c
/* Pull the ring in bounce-sized chunks into the (idle) static ring. */
memcpy(sweet_log_ring + blk * UFS_BLOCK, bounce, n * UFS_BLOCK);   /* ← 直接写环 */
...
printf("SWEET LOG: previous boot, last %u bytes (seq %llu)\n", end - start, h.seq);
for (...) printf("%c", c);                                          /* ← 又写回环 */
```

它把上一轮环的内容 memcpy 进 **同一个 `sweet_log_ring`**（注释自己写着 "the (idle)
static ring"），再把这段内容 printf 出来。printf 走 console → 也写进
`sweet_log_ring`。结果是**本次启动最开头的那一段 console（`SWEET-STAGE-40` …
`pmap_startup` … `vm_page_bootstrap` … zone 列表 … `SWEET MEM:` 限幅打印）被前一次
启动的内容覆盖**。dump 的调用点在 `sweet_log_thread` 启动处（0152 末尾的
`+ sweet_log_dump();`），即 `BSD root` 挂上之后 —— 比写页数的时机晚，但写的位置
是环的起点，所以把开头的记录洗掉了。

`0171` 的注释其实已经踩到过这个坑（"nothing leaves the ring holding the previous
boot, which reads like a …"），只是没人把它和页数行联系起来。

### 18.4 那份 55885 到底是谁的：第 177 行之前的启动，base = 0x80000000

用 §10 的页数模型（`tools/memplan-selftest/build/sim`）反解：

```
$ ./build/sim --start 0x82488000 --end 0xb9200000
STAT span_pages=56158 npages=55885 array_pages=273 first=0x828cc000 free=55885
```

`0x82488000 = 0x80000000 + 0x2488000`（旧 kernel 的 topOfKernelData），
`0xb9200000 = 0x80000000 + 0x39200000`（旧 `k60.adt` 的 dram-size）。这正是
`artifacts/README-ADT.txt` 里那颗"会害死人"的旧布局启动 —— 它与**本次镜像无关**。

对照本次镜像的真值（`k60-inspect-boot.py` + payload 头实读，见 §18.5）：
`[0xb64e4000, 0xdff00000)` → `sim` 给 **42,423** 页，减去 0170 的 1,152 个面板页
= **41,271** —— 与预期门禁值一致。

### 18.5 顺手校准的两个常量（`memplan.py` 需要跟）

payload 头在镜像文件偏移 **0x30000**（`0x8d3f` 那个 `SWEETXNU` 是 shim 里的字符串
常量，不是头）。实读字段：

| 字段 | 值 |
|---|---|
| version / nsegs | 1 / 10 |
| virt_base | `0xfffffe0006000000` |
| entry_va | `0xfffffe0007494488` |
| adt_off / adt_size | `0xfe8d00` / `0x2774` |
| kernel_end | `0xfffffe00084dc000` |
| phys_base / mem_size | `0xb4000000` / `0x2bf00000` |
| 推出 | `adt_pa=0xb64dc000`、`ba_pa=0xb64e0000`、`top=0xb64e4000`、**foot=`0x24e4000`** |

`memplan.py` 里写的是 `KERNEL_FOOT = 0x24e0000`（少 0x4000，即 1 个 16 KiB 页；`free` 41271 vs 错误的 41272 —— 见 §18.10）。
段表最紧的一处：seg9 结束于 `0xb64d8910`，ADT 起于 `0xb64dc000`，**间隙只有 0x36f0**。

### 18.6 对下一步的影响（改判）

| 原计划 | 改判 |
|---|---|
| 打诊断内核查"内核看到的 ADT 是谁写的" | **不需要**。没有任何证据说交付链断了；ADT/结构体布局核对也一致（+24 memSize / +96 deviceTreeP / +108 CommandLine / +728 memSizeActual，`sizeof==736`）|
| 0186 = 6 行早期打印 | 位置错了：早期打印**注定被 dump 洗掉**。改成在 `sweet_log_dump()` **之后**（`SWEET LOG: thread running` 一带）重打页数与 `args` 字段，那一段能活下来 |
| 门禁判据 | 必须用**第 228 行之后**的内容；`grep -n vm_page_bootstrap` 只允许命中"本次启动段"里的行 |
| Adreno 一行都没有 | 需要重判：`probe` 的打印在 `BSD root` 之后，属于"能活下来"的区域；所以 `SWEET ADRENO` = 0 仍然是真信号（或 IOLog 丢失），但**页数那条已经不能当证据** |

**结论**：这一轮不是"内存交付链断了"，而是**量具坏了**。修量具（把页数/参数打印挪到
dump 之后，或让 dump 不写环起点）比修链路便宜得多，且是获取任何 GPU 判据的前提。

### 18.7 反汇编复核：内核的 boot_args 只有一份，且来自 shim 的 `x0`（排除"偏移错位"）

对 `artifacts/kernel-adreno-0183.sweet` 的 `__TEXT_EXEC,__text` 整段反汇编（`llvm-objdump`）
逐条追下来的结果：

| 锚点 | 地址 | 汇编 | 含义 |
|---|---|---|---|
| 符号化 | `0x7ae226c` → `_arm_init+0x5f8`；`0x748b478` → `_arm_vm_init+0x74`；`0x748b4a4` → `_arm_vm_init+0xa0` | — | — |
| 入口 | `0x7494488` | `b _start_first_cpu` | LC_UNIXTHREAD 的 pc |
| 存 x0 | `0x74960f4` | `mov x20, x0` | shim 传的 boot_args PA |
| 结构体读 | `0x7496148/614c/6150/6158` | `ldr x22,[x20,#8]` / `[x20,#0x10]` / `[x20,#0x18]` / `[x20,#0x2d0]` | virtBase / physBase / memSize / bootFlags |
| PA→VA | `0x7496458/645c` | `add x0,x20,x22` / `sub x0,x0,x23` | `x0 = PA + virtBase − physBase` |
| 拷进 const | `0x7ae2264/2268/226c` | `mov x0,x20` / `mov x1,x19` / `mov w2,#0x2e0` | dst=`_const_boot_args`(0x717ba68)，src=x19=shim 的那个指针，736 字节 |
| 发布 | `0x7ae2278` | `str x20,[x8,#0xd48]` | `_BootArgs = &const_boot_args` |
| 0172 读点 | `0x748b478` | `ldr x9,[x1,#0x2d8]` + `cmp/csel` | `memSizeActual ?: mem_size` |

内核侧偏移（与 shim 的 `struct boot_args` 逐字段对照，全一致）：

| 字段 | 内核 | shim |
|---|---|---|
| virtBase / physBase | +8 / +16 | +8 / +16 |
| memSize | +24（`0x748b450 ldr x8,[x1,#0x18]`） | +24 |
| topOfKernelData | +32（`0x748b528 ldr x0,[x28,#0x20]` 紧跟 `bl _phystokv`） | +32 |
| Video | +40 | +40 |
| machineType | +88 | +88 |
| deviceTreeP | +96（`0x7a870b4 ldr x9,[x19,#0x60]` → `PE_state.deviceTreeHead`） | +96 |
| deviceTreeLength | +104（`0x7a870bc ldr w9,[x19,#0x68]`） | +104 |
| CommandLine | +108（`0x7a85784 add x0,x8,#0x6c`，`x8 = PE_state.bootArgs`） | +108 |
| bootFlags | +720（`0x7496158 ldr x26,[x20,#0x2d0]`） | +720 |
| memSizeActual | +728（`0x748b478`） | +728 |
| sizeof | 736（`0x7ae226c mov w2,#0x2e0`） | 736 |

→ **没有第二份副本，也没有从固定地址/DT `/chosen` 重取 boot_args 的路径**；
"同一个 736 字节结构前半新后半旧"在结构上不可能。§18.1–18.4 的"日志读错行"
是唯一自洽的解释。此外 `ci/xnu/pexpert/pexpert/arm64/boot.h:58` 的成员表与 shim 相同，
这是"布局一致"的第二来源。

### 18.8 镜像/shim 取证复核（同日，独立子分析）：三条假说全部排除，机制只剩 §18.3

| 假说 | 判定 | 证据 |
|---|---|---|
| shim 写错字段/写错基址 | **排除** | shim `.text 0x36d4 ldr x8,[x22,#0x40]`（payload→mem_size）→ `0x36ec f9016e68 str x8,[x19,#0x2d8]`（+728 = memSizeActual）；`x19 = ba_pa` 由 `0x35bc and x19,x9,#~0x3fff` 得出 |
| 抓错/陈旧 payload 副本 | **排除** | payload 头在**文件 0x30000**（内核区相对 0x2f000 = `nm shim.elf` 的 `_end`，`build.sh` 先 truncate 再 cat，命中 `off=0` 是确定性的）；`0x8d3f` 那个 `SWEETXNU` 是 `.rodata` 里的字符串常量，永远在扫描起点之前 |
| 段/补零覆盖 ADT 或 boot_args | **排除（方向也反了）** | 最高段末尾 `0xb64d8910`，`adt_pa=0xb64dc000`，间隙 `0x36f0` = "最高段末尾→16K 对齐边界"的构造余量；且 `loader.c` 写序是 段(171–177) → ADT(182) → boot_args(186–217)，ADT/ba 最后落盘 |
| ADT 里有第二份 dram 或旧布局残留 | **排除** | 全 ADT（0x2774 B）扫 LE u64：`0xb4000000`/`0x2bf00000`/`0xdff00000` 各 1 次；`0x80000000`/`0x39200000`/`0xb9200000`/`0x4c000000` 各 **0** 次。无 `/memory` 节点（用的是 `/chosen` dram-base/size） |
| 镜像里的 shim 与 `loader.c` 不同源 | **排除** | `[0x1000,0x30000)` 的 sha256 = `13403464…294ad`，与 `out/Image[0:0x2f000]` 完全相同 |

**dram 值**：`/chosen` dram-base `0xb4000000`、dram-size `0x2bf00000`、`/pram` `0xdff00000+0x100000`
（`dram-base + dram-size == /pram base`，严丝合缝）。

**`0x0152` dump 的变量流（决定性）**：

```c
/* 写侧 0047 */  at = os_atomic_add_orig(&sweet_log_total, size);  ring[(at+i)%SIZE] = c[i];
/* dump 0152 */  want = MIN(h.total, SIZE);  first = (h.total - want) % SIZE;
                 memcpy(sweet_log_ring + blk*UFS_BLOCK, bounce, n*UFS_BLOCK);  /* ← 盖同一批偏移 */
```

- `sweet_log_total` **从不重置**，写偏移 ≡ `total`，所以 dump 的 `memcpy` 恰好落在
  "本次启动已写过的同一批偏移"上 —— **1:1 替换**，一个字节不剩。
- 这不是巧合：上一轮 `h.total`（13,531）与本次到达 dump 时刻的 `total` 几乎相等
  （启动前段确定性），所以每次启动都会整段换掉自己的早期 console。
- 实证：`verify.txt[0:10832]` 与 `adreno1.txt[0:10832]` **逐字节相同**，第一个差异字节
  落在一个内嵌 dump 头的 `seq` 数字上；重放区里嵌着 **4 个** 嵌套 dump 头
  （`0x1c33/0x1c6d/0x1ca2/0x1d28`，N=4846/3056/3042/3394）= dump 文本自己进环、被再持久化。

### 18.9 因此 0186 该写什么（三条，按可靠性）

1. **根因**：`sweet_log_dump()` 不要拿 `sweet_log_ring` 当转储中转 —— 改成读进独立缓冲
   或直接逐块打印，环不再被历史数据污染（`total` 与内容重新自洽）。一行不改语义。
2. **门禁行必须持久化且可回放**：`v_page_free_count`/`avail_start`/`avail_end`/
   `args->memSize`/`memSizeActual`/`topOfKernelData` 写进 **header block 的保留字段**
   （该块 4096 B 只用了 ~40 B，flusher 每次 flush 都重写），下一轮 dump 打一行
   `SWEET LOG: prev gate free_pages=N …`。**只"dump 之后重打一行"不够**：dump 只回放
   最后 48 行，重打的行会被启动后段挤出窗口。
3. **校准**：`memplan.py` 的 `KERNEL_FOOT` 应为 `0x24e4000`（现为 `0x24e0000`，差 4 页）；
   `account(P0, foot=0x24e4000)` = **41271**，与 `k60-inspect-boot.py` 的 41271 差 1 页 ——
   判据用 ±10，别卡 1 页。

### 18.10 扣分裁决：门禁值到底是多少（41271，不是 41272）

独立复算（`account(P0, foot=0x24e4000)` + `sim`）与三点交叉验证：

| 说法 | 判定 | 依据 |
|---|---|---|
| **41271** | ✅ 正确 | `span_pages 42631 − array 208 = npages 42423`；`42423 − notched 1152 = 41271` |
| 41272 | ❌ 属于**更短的内核** | `foot 0x24e0000` ⇒ `kernel_end 0xfffffe00084d8000`，那是 `artifacts/xnu-adreno-cmd.img`；本镜像 `kernel_end 0x84dc000` ⇒ `foot 0x24e4000` |
| 那 1 页是什么 | 就是 `ba_pa` 那一页 | `loader.c:186 zero64(ba_pa,16K)` 写的是 shim 自己的 `struct boot_args`，物理上永不可能空闲；`topOfKernelData = ba_pa+0x4000` 才是 VM 起点 |
| 面板 notch 页数 | **1152**，不是 1151 | 18 MiB / 16 KiB = 1152 |
| 是不是 panic log 那 1 MiB 的锅 | 不是 | 不扣 panic 会得 41335（+64 页），量级完全不对 |
| `sim.c` 与 `account()` 语义 | 一致 | 两边都 `first = start + array_pages*PAGE`，数组页**不进** free（差 208 页的量级），所以不是 ±1 的来源 |

`memplan.py` 的 `KERNEL_FOOT` 已改 `0x24e4000`；`tools/k60-inspect-boot.py` 的期望表把键从
`(dram-base, dram-size)` 扩成 `(dram-base, dram-size, foot)`，让两颗内核各归各的 41272/41271，
未知组合仍然硬失败。`check.py` 的 `55885` 正向对照与三条负例**一行未动**，仍然全绿/全红符合预期。

---

## 19. 集成孤儿线的遗产（2026-10-26 17:4x）

背景：17:00–17:21 有另一条会话的子 agent 在本机反复 `fastboot boot`（job `c89c5a`），
会话关闭后变成孤儿，刷了 4 次。设备侧只读核查结论：`boot_a` 前 2 MiB 是 `ANDROID!`、
无 `SWEETXNU`，**分区没被写过**；`ios-sweet/shim/loader.c` 也没被改。它的判断建立在
§18 那条重放残影上，所以"把 ADT 压到 511 MiB / 加 `maxmem=512`"那一支**不采纳**。
下面这些留下来。

### 19.1 采纳：shim 回读令牌（它的想法，这次真的落地了）

它在 `loader.c` 里加过把物理值追加进 `CommandLine` 的实验，但**从未编进被刷的 shim**
（`out/` 里那份 `loader.c` 改动后来丢了，`boot-args` 行里没有 `snp=`）。现在按同一思路
正式实现：`ios-sweet/shim/loader.c` 在写完 `boot_args` 之后追加

```
 sweetba=<phys_base>:<payload mem_size>:<ba->memSize>:<ba->memSizeActual>:<adt_pa>:<topOfKernelData>:<kernel_end_pa>
```

理由：早期 console 一定被 `sweet_log_dump()` 重放覆盖，而 launchd 在**重放之后**
（`verify.txt:303` 那种位置）会把 `CommandLine` 原样打进 `boot-args = …`。于是每次启动
都自带一份"shim 自己以为写了什么"的证据，与 0186 的 `SWEET GATE`（内核读到了什么）
互为对照：**两边不一致就是交付链断，一致就轮到窗口/VM 逻辑**。

**验证时的坑（重要，否则会以为没编进去）**：`sweetba=` 这个字面串在 `out/Image` 里
**grep 不到** —— clang 把这个常量循环展开成了立即数 `strb`（`' '`,`s`,`w`,`e`,`e`,`t`,`b`,`a`,`=`），
所以只能靠反汇编确认：

```
0x36d0  str  x9, [x19, #728]        ; memSizeActual
0x36d4  ; 扫 NUL 找 cmdline 末尾
0x36f0  cmp  w8, #465               ; n + 142 < 608
0x370c  strb w9, [x8, #108] …       ; ' sweetba='（立即数逐字节）
0x3748  bl   0x3814 <put_hex64>     ; 七个 16 位十六进制数，':' 分隔
```

`build.sh` 的"零 ABS64 重定位"断言通过（否则脚本会直接 `exit 1`）。

### 19.2 采纳：它的三组不变性数据，作为 §18 的旁证

同一颗内核下把 ADT 从 703 MiB 压到 511 MiB、又加 `maxmem=512`，`vm_page_bootstrap`
始终 55885 —— 它把这条解释成"内存账不吃 boot-args"，其实是**重放文本所以不会变**。
这组数据现在归档，作为 §18 的第三个独立佐证。

### 19.3 采纳：仓库里的 shim/ 之前是**落后 145 行**的旧版

`k60-repo-full/shim/` 与现役 `ios-sweet/shim/` 相比：`loader.c` 差 145 行（缺
`PAYLOAD_SAFE_PA`、"payload 追加在 `_end` 之后"的查找分支），且**缺** `panel_init.h`、
`panel_init_nodsc.h`、`usb.c`、`usb.h`。也就是说按仓库那份源码**编不出当前这颗 shim**。
本次把现役源码整体同步进仓库（`out/` 产物不进库）。

### 19.4 顺手修的：`tools/k60-log-summary.py` 的崩点

`fsc()` 里 `'reserved 0b%06b' % code` —— `%b` 不是合法转换符，`.get()` 的默认值会先求值，
所以**任何一条 ABORT 都会让脚本抛 ValueError**（历史上"faults 段是空的"其实是崩溃）。
已改成 `format(code, '06b')`；并把"只认重放边界之后的内容"写进判定。

### 19.5 归档但不采纳

| 资产 | 位置 | 状态 |
|---|---|---|
| `xnu-diag-maxmem512.img` / `xnu-diag-adt512.img` / `xnu-diag-snp.img` / `xnu-diag-snp-p0.img` | `artifacts/` | **作废**，见 `artifacts/README-LEGACY-512.txt`：它们验证的是不存在的病，且 shim 里没有令牌 |
| `maxmem512.txt` / `adt512.txt` / `snp.txt` / `verify.txt` / `k60-adreno1.txt` / `notouch.txt` | `artifacts/logs/2026-10-26/` | 归档为 §18 的证据，别再当"本次启动"读 |

---

## 20. 2026-10-26 18:21 上机（0186）：**门禁第一次真正测到，并且通过**；M1/M2 都过，M3 在门口

镜像 `artifacts/xnu-0186-p0.img`（16,896,000 B，sha256 前 16 位 `5e3a523bdf191252`），
内核 17,287,688 B（含 0183 + 0185 GMU 固件 + 0186 门禁），cmdline
`sweetadreno=1 sweetadreno_stage=3`。日志 `/tmp/b0186.txt`，135,755 B。

### 20.1 0186 生效的直接证据（先确认量具）

```
199: SWEET LOG: previous boot, last 3842 bytes (seq 31)     ← dump 头（重放 17:13 那次）
268: SWEET LOG: end of previous boot log
269: SWEET GATE: free=41272 …
```

**第 1–198 行是本次启动自己的 console**（`SWEET-STAGE-26` … `PE_init_iokit` …
`bsd_init` …），这在修复前是**注定消失**的那一段。重放区只剩 199–268（3,842 B），
不再是整段 13.5 KB —— 环没被洗，`total` 与内容重新自洽。

### 20.2 门禁：**PASS**

```
SWEET GATE: free=41272 physBase=0xb4000000 memSize=0x4c000000 memActual=0x2bf00000
            topk=0xb64e8000 dram=0x0 managed=0x2bf00000 gPhysSize=0x2bf00000
```

| 字段 | 观测 | 判定 |
|---|---|---|
| `free` | **41272** | ✅（`manage` 703 MiB 下的正确量级；模型 41270，差 2 页见下）|
| `managed` / `gPhysSize` | `0x2bf00000` = 703 MiB | ✅ 0172 限幅生效 |
| `memActual` | `0x2bf00000` | ✅ **shim 写的 = 内核读的**，交付链实测闭环 |
| `topk` | `0xb64e8000` = `phys_base + 0x24e8000` | ✅ 与 payload 头一致 |
| `dram` | `0x0` | ✅ 正常：这是 `arm_vm_init(memory_size=…)` 的入参，本移植给 0，限幅靠 `memSizeActual` |

**模型差 2 页**：`span 42630 页 − npages 42422 = …`，实测被排除 **1150** 页，模型按
0184 notch 表算 **1152** 页（18 MiB / 16 KiB）。32 KiB 的差在 `memplan-selftest`
待修（`account()` 的 notched 计数），不影响判据（±10）。

### 20.3 M1：probe done，但寄存器全是 0（不是 `0xffffffff`）

```
SWEET ADRENO: maps gpucc=0xfffffe8fff5fc000 ao=0xfffffe8fff60c000 gmu=0x…61c000
SWEET ADRENO: gpucc[0..3]=0   ao[0..3]=0   gmu[0..3]=0
SWEET ADRENO: probe done
SWEET ADRENO: GMU is out of reset (bootloader or an earlier run started it)
```

gpucc/ao/gmu **全零**：既不是越界中止、也不是 `0xffffffff`，说明 ioremap 有效、路径通，
只是上电前这些块不回读（或读的是复位值）。M1 的判据是"能读、不中止"，**通过**。

### 20.4 M2：**7/7 分支全跑起来**（本次最大的进展）

```
preflight gdsc=0x22f001 power=0x5ff4 → 3/7 branches report running   ← bootloader 已开 3 条
phase 1 GDSC (gpucc+0x9108) / 2 GCC branches / 3 PLLs / 4 root sources / 5 branches / 6 release
SWEET ADRENO: GPUCC verified: 7/7 branches running
SWEET ADRENO: GPUCC up
SWEET ADRENO: clock sequence returned 0
post-power: gdsc=0xf8228800 hwctl=0x80001fe0 tcs=0
            br1(0x9144)=0x1 br3(0x913c)=0x2221 br4=0x1 br6(0x7000)=0x1  （工作位全部清零）
```

即 0180 的三处时钟语义修正 + 0181 的收尾验证都是对的：**GPUCC 完整上电，7/7**。

### 20.5 M3：GMU 还按在复位里（下一道门）

```
snapshot[post-power-ok] gmu syreset=0x1 boot=0 finit=0 hfi=0
SWEET ADRENO: GMU is held in reset (M3 has not run yet)
SWEET ADRENO: GMU firmware has not reported FW_INIT OK (0x0)
SWEET ADRENO: HFI is not up (0x0)
```

0185 只是把固件**嵌进内核**（`fw vector` 字符串在、`sweetadreno_fwinfo` 在），没有装载器。
所以 M3 的正确下一步是：把 `gmu_gen70000.bin` 搬进 GMU ITCM（`0x1b400` 起按字）、
置 `CM3_BOOT_CONFIG`、开 `CM3_SYSRESET`、轮询 `FW_INIT_RESULT` 的 0xe00 位，
再接 HFI。**这块可以直接照着 `refs/UniXNU-pong/drivers/PongAdreno/PongAdrenoGmu.c` +
`PongAdrenoHfi.c` 移植**（共 ~280 + HFI 行），STUB 里已经是同一套寄存器语义。

### 20.6 仍然存在的干扰：45 s 后 touch 打死

日志尾部仍是 `SWEET TOUCH: gcc probe vote read`（`GCC+0x15200c` 同步外部中止）。它发生在
M1/M2 之后（M1/M2 在 IOKit 匹配期就完成），所以堵不住门禁与 M1/M2 的判定，但会阻止更长
的观察。下一轮换成 `sweettouchoff=1` 即可（0173 支持），届时才能看到 launchd 之后的东西
以及 GMU/HFI 的后续。

### 20.7 本轮判据表

| 项 | 期望 | 实测 | 结论 |
|---|---|---|---|
| `SWEET GATE` 存在且在重放边界之后 | 是 | 269 行 | ✅ 0186 生效 |
| `free` | 41272±10 | 41272 | ✅ |
| `managed`/`memActual` | `0x2bf00000` | 同 | ✅ 交付链闭环 |
| M1 `probe done` | 有 | 有 | ✅ |
| M2 `GPUCC verified: 7/7` | 7/7 | **7/7** | ✅ |
| M3 `FW_INIT OK` | — | `0x0`（GMU 仍在 reset） | ⏳ 下一阶段 |

## 21. 2026-10-26 18:5x 离线：M3 装载器（0187）写好，仿真 20/20

用户不在设备旁，这一轮只做离线能做完的部分：**把 GMU 固件装载写出来并验证**。

### 21.1 做了什么

`xnu/patches/0187-k60-adreno-gmu-load.patch`，在 `SweetAdreno.cpp` 里新增
`sweet_adreno_gmu_load()`，按 `PongAdrenoGmuStart` 逐段对应，但**只做纯 MMIO 能完成的部分**：

- `sweetadreno_gmu=1`：把固件的 ITCM/DTCM 段按字写进 GMU 窗口（135 个字，
  ITCM 0x1b400 / DTCM 0x1c400 起），**逐字回读比对**，然后写配置字
  （FW_INIT=0、BOOT_CONFIG=2、CM3_CFG=0x4052、GENERAL_10=0x07030200、
  GENERAL_9、AHB_FENCE=0x80c808a0、SYS_BUS/ICACHE/DCACHE）。
  **不动 CM3_SYSRESET**，可重跑，用来先证明"这个窗口真的写得进去"。
- `sweetadreno_gmu=2`：再加清 CX GBIF halt（kgsl+0x3c45*4 写 0、回读确认）、
  放 `CM3_SYSRESET`、轮询 `FW_INIT_RESULT` 的 0x1ff==0x100；读到 `0xe00`
  任意位置位就判固件报错；超时 100 ms 就把复位按回去，不留悬着的 CM3。
- `stage=4/5` 是 1/2 的简写。

### 21.2 明确没做的三样（会在日志里各打一行）

1. **cache 段**（`0x4000` 起 56 KB）——上游放在 kgsl 的 16 MB GMU cache 里，
   要先在内核侧要连续物理页并给出 iova；
2. **HFI 队列表**（`HFI_QTBL_ADDR`）；
3. **日志区**（`GENERAL_8`）。

这三样是 0188 的活。所以 0187 最好只能走到 `FW_INIT=0x100`，**不要指望 HFI 起来**。

### 21.3 离线怎么验的

- `tools/adreno-selftest` 新增三个场景：`gmuboot`（写 SYSRESET=0 后固件才回 0x100）、
  `gmureadback`（总线吞掉 ITCM 首字 → 必须报 `first mismatch at 0x5000`）、
  `gmubooterr`（FW_INIT 报 0xe00 → 必须 rc=-2）。
- `./run.sh 0187`：aarch64 目标真编译通过、C 符号未修饰、20 项全绿
  （含 6 条 M3 分支）。
- 另有既存的 `gmu-model.c`，用真固件（57,272 B）跑 TOC 解析与引导模型，11 项通过。

### 21.4 上机时的判据

```
SWEET ADRENO: gmu upload wrote 135 words from 4 segments, 1 segment(s) skipped
SWEET ADRENO: gmu readback 135 words, 0 mismatch
SWEET ADRENO: CX GBIF halt cleared
SWEET ADRENO: gmu boot rc=0 fw_init=0x100
SWEET ADRENO: GMU firmware booted (FW_INIT 0x100); HFI start is 0188
```

第一条就失败（`outside the GMU window`）说明固件段表与窗口定义对不上；
回读有 mismatch 说明窗口写不进去或位宽不对；`rc=-2` 看 FW_INIT 的 0xe00 位
是哪一位；超时则 CM3 已被重新按回复位，可以再跑一次。

---

## 21. M3（GMU 固件装载）：0187 定稿、两次上机的顺序，以及"未确认"清单

### 21.1 0187 的两种实现，只留了一个

18:44 与 18:48 先后出现两份 0187（都改 `iokit/Kernel/arm/SweetAdreno.cpp`）：

| 版本 | 内容 | 处置 |
|---|---|---|
| `dace546`（已提交，含 `tools/adreno-selftest` 的 6 条新用例，`run.sh 0187` 全绿） | `gmu=1` 上传 TCM + 逐字回读（**可重跑**）；`gmu=2` 才清 CX GBIF halt、放 `CM3_SYSRESET`、轮询 FW_INIT（100 ms 超时 + 失败回写复位）；入口自带 `syreset!=1` 与 `HFI bit0` 两道拒绝 | **保留** |
| 另一份（未提交，453 行，比提交版多 157 行） | 语义相反（`=1` 就放复位），另有独立的 7/7 复核与回滚 | 归档 `/tmp/agent-0187.patch`（sha256 `ebafe02a…`），**不入库** |

并存的后果是同一个文件里两套同名实现 → C++ 重复定义必炸，所以只能留一套。保留提交版的理由：
① 自带仓库标准校验台（20 项全绿），② `=1` 默认不放复位（第一次上机更保守），
③ 入口的两道拒绝（`syreset!=1` / `HFI bit0`）它本来就有 —— 另一份声称的"更安全"这点并不成立。

### 21.2 两次上机的顺序（都很短）

```bash
# 第一次：只上传 + 回读，可重跑，不改复位
K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3 sweetadreno_gmu=1"
# 第二次：真放复位启动 CM3
K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3 sweetadreno_gmu=2"
# 两次都建议再加 sweettouchoff=1，免得 45 s 后被 touch 打断
```

判据：

| 步骤 | 必须看到 |
|---|---|
| `gmu=1` | `gmu upload wrote … words … skipped`、`gmu readback … 0 mismatch`、`gmu load stopped before boot (mode=1, reset untouched)` |
| `gmu=2` | 上面全部 + `CX GBIF halt cleared` + `gmu boot rc=0 fw_init=0x100` + `GMU firmware booted (FW_INIT 0x100); HFI start is 0188` |
| 失败即停 | `GMU is not held in reset` / `HFI is already up` / `readback … mismatch` / `FW_INIT timeout after 100 ms` / `firmware reported an error` |

`fw_init=0x100` 且 `hfi=0` 是**预期**组合：HFI 启动是 0188。

### 21.3 未确认清单（照抄自独立实现的自查，必须在上机时留意）

| # | 未确认项 | 现状 | 若出问题的第一动作 |
|---|---|---|---|
| 1 | `CM3_BOOT_CONFIG=2` 的位域含义 | 照抄 Pong（同一颗 SM8475/Cape）；`GPU-PORT-NOTES` §10 的模型也只断言"≠2 会超时" | FW_INIT 超时 → 先试 `0` |
| 2 | ITCM/DTCM 字宽与 endian | 按 32 位小端拼字（Pong 用 `le32()`，寄存器窗口按 dword 索引） | 逐字回读会在第一个字就 mismatch 并停（不会带坏固件放复位）|
| 3 | 是否要先清 TCM；`TCM_RET_CNTL`（`gpucc+0xe0e4`）的极性 | **两版都没写**（Pong 也不清） | 若 `readback failed`，这是第一嫌疑 |
| 4 | HFI 中断线（kgsl 0x12c / GMU 0x130 / HFI 0x131）是否要在 M3 阶段先开 | **没开**：FW_INIT 走寄存器窗口回报，不经 HFI | 若 FW_INIT 一直 `0x0`，考虑补这一步 |
| 5 | **cache 段（56,492 B，Pong 要求落在 IOVA 0x4000 的 GPU 可见内存）缺失** | 两版都跳过（要 SMMU/IOVA，属 0188） | 固件首次读 cache 时的行为未知：可能总线错/GBIF 挂死/静默。这是本轮最大未知量 |
| 6 | `GENERAL_10=0x07030200`（Pong 注释写 `0x07030002`，值却是前者） | 照抄"值"不照抄注释 | 固件若按芯片号分支，这是第二嫌疑 |
| 7 | `RSCC_SEQ_BUSY` 前置检查（Pong 有） | 没做：`0x3d50000` 未映射，读它是否安全未确认 | — |
| 8 | `kgsl+0xf114`（CX GBIF halt）那一位的确切含义 | 只确认"Pong 写 0 并读回 0" | 读回非 0 就停 |

### 21.4 与 B 线（软件渲染）的关系

0187 只解决"GMU 能起来"；**能不能产生像素还要过 CP/ZAP/SQE 与 IOAccelerator 后端**。
而 B 线的 4 字节 dyld 缓存补丁是另一条独立捷径（`QuartzCore` 的
`EAGLServer::renderer()` 返回 NULL → 尾调用 `Server::render_update` → `OGL::SWContext`），
两者互不阻塞，可以并行推进。
