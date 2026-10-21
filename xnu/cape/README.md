# CAPE (Redmi K60, SM8475) machine config — 待接入

这个目录是 **K60 板级支持的草稿**，还不是生效的补丁。等 `MACHINE_CONFIG=SWEET`
的第一次 CI 跑通（验证 xnu-8796 + 补丁序列 + macOS 工具链可用）之后，按下面的
四处改动合成一个编号补丁（放在 `xnu/patches/` 里、编号最大，保证在既有补丁之后应用），
再触发 `machine_config=CAPE`。

## 需要的四处改动（对照 `0004-sweet-machine-config.patch` 的写法）

1. `makedefs/MakeInc.def`
   - `SUPPORTED_ARM64_MACHINE_CONFIGS` 加 `CAPE`
   - `MACHINE_FLAGS_ARM64_CAPE = -DARM64_BOARD_CONFIG_CAPE -march=armv8.2-a+crypto`
     （X2/A710/A510 是 ARMv9，但本 build 不开 PAC，与 sweet 走同一条 plain arm64 路径）
   - `arm64e` 排除条件里加 `CAPE`
2. `osfmk/arm64/proc_reg.h`
   - 新增 `#elif defined (CAPE)` 分支，`MMU_I_CLINE 6` / `MMU_CLINE 6`
3. `pexpert/pexpert/arm64/CAPE.h`
   - 本目录的 `CAPE.h` 拷进去（值取自设备 live FDT：VTIMER INTID 27、GICR stride 0x40000）
4. `pexpert/pexpert/arm64/board_config.h`
   - `#ifdef ARM64_BOARD_CONFIG_CAPE` → `#include <pexpert/arm64/CAPE.h>`，
     `MAX_L2_CLINE 6`、`MAX_CPUS 8`、`MAX_CPU_CLUSTERS 3`（X2 / A710 / A510）、`CORE_NCTRS 8`

## 与 sweet 的关键差异（移植时要盯的点）

| 项 | sweet (SM7150) | cape (SM8475) | 影响 |
|---|---|---|---|
| 虚拟定时器 INTID | 19（QCOM 改了 PPI 编号） | **27**（标准 PPI 11） | 装错会收不到 tick |
| GICR stride | 0x20000 | **0x40000** | Rediscover 循环会跑飞 |
| CPU cluster | 1 | **3** | `MAX_CPU_CLUSTERS`、cluster 相关路径 |
| PMIC/按钮 | SPMI（老一代） | **新一代 SPMI** | 0004/0062 里的按钮/电源驱动需要重写 |
| UFS | 能读能写 | xnu 驱动未验证；shim 实测**能读不能写** | `ufsrw=1` 可写根要单独验 |
| 显示 | ABL 交接保留面板 | **ABL 关面板**（DSC cmd-mode） | 必须自带 DSI/DPU/DSC 驱动，否则无画面 |

## 先决条件

- 一次成功的 `MACHINE_CONFIG=SWEET` CI run（拿到 kernel artifact，确认工具链无碍）
- `tools/mkadt.py` 用 K60 的常量生成 ADT（见仓库根 `PORT-STATUS-K60.md` 第三节）
- shim 的 `payload.bin`（`tools/mkpayload.py`）与 boot-arg 组合按 cape 调整
