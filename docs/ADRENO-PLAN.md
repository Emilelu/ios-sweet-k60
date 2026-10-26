# Adreno 730 移植计划（K60 / SM8475）

来源：`refs/UniXNU-pong`（Nothing Phone 2，同为 SM8475）。它没有跑通运行时 GPU，
但寄存器、时钟、GMU 序列是同一块 Cape GPUCC，可以当参考实现用。

## 我们这台的寄存器地图（取自 `dump/live.dts`）

| 块 | 物理地址 | 大小 | 备注 |
|---|---|---|---|
| kgsl-3d0 | `0x3d00000` | 0x40000 | compatible `qcom,adreno-gpu-gen7-4-0`，chipid `0x7030002` |
| gmu | `0x3d68000` | 0x37000 | `qcom,gen7-gmu`，HFI 中断 0x130，GMU 中断 0x131 |
| gmu_ao_blk_dec0 | `0x3d40000` | 0x10000 | |
| gpucc | `0x3d90000` | 0x10000 | Cape GPUCC |
| gcc | `0x100000` | 0x1f4200 | |
| kgsl-smmu | `0x3da0000` | | TBU: 0x3de9000 / 0x3ded000 |
| cx_dbgc | `0x3d61000` | 0x800 | |
| rscc | `0x3d50000` | 0x10000 | |
| isense | `0x3d8b000` | 0x2000 | |

GPU 中断：kgsl 0x12c；GMU/HFI 0x130、0x131。

## 里程碑

### M1 只读探测（代码已写，未上机）

启动参数 `sweetadreno=1`。会打印 kgsl/gmu/gpucc/ao 的前四个字，
外加 `sweetadreno_verbose=1` 时的 kgsl 前 256 字节非零字。

**验收**：不写任何寄存器；能读到非零值。
（2026-10-26 更正：`chipid 0x7030002` 来自 DT 的 `qcom,chipid`，**不是寄存器**，
代码读不出来；原条目当作验收项会永远等不到。逐条判据见 `GPU-PORT-NOTES.md` §5。）
如果读到全 `0xffffffff`，说明该块无时钟/NoC slave error，先别往下走。

### M2 GPUCC 上电（代码已写，未上机）

启动参数 `sweetadreno_power=1`（在 M1 之后自动执行）。六个阶段各自先打日志：

1. GDSC（`gpucc+0x9108`）+ 等 `0x953c` bit31
2. GCC 分支（`0x62000` 的 bit15/16，然后 `0x81004/10/18`）
3. 两个 PLL（`gpucc+0x0000`、`+0x1000`，LUCID_OLE 配置）
4. 三个根时钟源（`0x9318 / 0x93ec / 0x9010`）
5. 七个分支（`0x9004/9144/93e8/913c/9148/911c/7000`）
6. 释放临时 force

**验收**：日志停在最后一个成功阶段；`SWEET ADRENO: GPUCC up` 表示时钟全开。
任何 `halted` / `timeout` 就停在那里，不要继续下一步。

### M3 GMU 固件启动

需要：把 `gpu-fw/gmu_gen70000.bin` 载入 GMU 的 ITCM（`0x1b400` 起，按字），
设置 `CM3_BOOT_CONFIG`，开 `CM3_SYSRESET`，轮询 `CM3_FW_INIT_RESULT` 的 0xe00 位。

**验收**：`FW_INIT_RESULT` 返回成功、GMU 通过 HFI 回一条消息。

### M4 CP 与 Metal

ZAP 认证 → CP 微码（`a730_sqe.fw`）→ 一条真实 fence 时间戳 → 才谈
`IOAcceleratorES` 用户态（他们用 metal2vulkan + Turnip 桥）。

## 现实判断

上游自己写着「Actual Pong GPU execution and accelerated presentation remain
unconfirmed」，最近一次日志停在 CP 初始化完成之前。所以 M3 之后每一步都是新地，
板级适配能抄，Metal 那一层要自己趟。

## 上机测试顺序（设备回来后）

```
fastboot boot <镜像，cmdline 带 sweetadreno=1>
# 看日志：SWEET ADRENO: probe done
fastboot boot <镜像，cmdline 带 sweetadreno=1 sweetadreno_power=1>
# 看日志停在第几阶段
```

## 2026-10-26 离线修订：0179 → 0180

设备不在，只做了本地编译 + 仿真。结论与证据见 `GPU-PORT-NOTES.md`，要点：

- 地址/位域与 `live.dts`、`PongAdrenoClocks.c` 逐项一致，**不用改**。
  （`qcom,gmu@3d69000` 的节点名是笔误，reg 是 `0x3d68000`，按 reg。）
- 0179 有 3 处时钟等待与 vendor 语义不符，会在健康硬件上误报 halted，已由
  `0180-k60-adreno-clock-waits.patch` 修正：GCC 0x81004 的 HWCG 3、GPUCC 0x911c
  的 delayed 不 poll、root mux 的 update 位检查。
- 探测顺序改为 gpucc → gmu_ao → gmu →（`sweetadreno_kgsl=1`）kgsl，
  免得一次 GX 域读挂死把前面的读数一起吞掉。
- 构建时用 `K60_CMDLINE_EXTRA="sweetadreno=1"`，脚本已支持。
