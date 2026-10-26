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

版本矩阵（`./run.sh <补丁号>` 的通过项数，单调递增即为补丁序列的净效果）：

| 重建到 | 通过项 | 失败项 |
|---|---|---|
| 0179 | 3 | 3（hwcg / delayed / busy） |
| 0180 | 6 | 0 |
| 0181 | 10 | 0 |
| 0182 | 12 | 0 |

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

建议 A：和上游同构、排错时不用对齐两套打包逻辑，代价只是仓库里多一个固件数组文件。

方案 A 的脚手架已经跑通（未接进构建、未提交固件数组本身）：
`tools/mk-gmu-fw.py gpu-fw/gmu_gen70000.bin > SweetAdrenoGmuFw.h`
生成 57,272 字节 → 296,216 字节头文件，脚本会把生成的数组回读解析、
与源文件逐字节比对（实测一致），头文件单独 g++ 编译通过。

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
