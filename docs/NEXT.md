# 回来后从这里继续

## 2026-10-26 内存轮（离线，别和 GPU 那轮混）

- **703 MB 不是天花板**，它只是"payload 在 `0xb4000000`、上界卡在 gunyah 前"这个摆法的
  `dram-size`。真正的可用内存是 **41272 页 = 644.9 MiB**（内核 + 面板 + vm_page 数组都在这 703 里）。
- 两个方案已算清，本地校验台全绿：`cd k60-repo-full/tools/memplan-selftest && ./run.sh`
  - P1（只抬上界，**+74.5 MiB** → 719.4 MiB）：`MEMPLAN=p1 ./tools/k60-build-boot.sh ...`
  - P2（payload 下移到 `0xa3000000`，**+337.5 MiB** → 982.3 MiB）：`MEMPLAN=p2 ...`
- 两个方案共用**一个**新补丁 `0184-k60-ram-notches.patch`（把 0170 的单一面板判断换成
  5 段 notch 表）。**0184 必须先进内核**，否则抬窗口就会踩 gunyah → 无声挂死。
- 上机判据：`vm_page_bootstrap` 页数应为 **46042（P1）/ 62869（P2）**，不是 41272。
- 细节、地址表、needs-device 清单全在 `MEMORY-PLAN.md`。
- **§10 新增「Android 多段内存对照」**：Android 在同一台机器上用到 8.2 GiB（6.9 GiB 在
  4 GiB 以上），Linux 靠 memblock 多段 + reserved/nomap + 稀疏 section 做到；XNU 缺的只是
  登记和稀疏元数据。**但 4 GiB 以上的"墙"其实没验证过**——之前探的 `0x100000000` 是 28 GiB
  空洞。一次 `K60_RDCHECK` 探针（读 `0x840000000`）决定要不要走 B 路线（2 个补丁、10.45 GiB）。

## 2026-10-26 收尾：当前状态与设备回来后的顺序

**状态**

| 项 | 值 |
|---|---|
| 补丁序列 | 0179 探测 → 0180 时钟语义 → 0181 预检/验证 → 0182 分阶段/快照 → 0183 C 链接 → 0185 GMU 固件入内核（`0168` 已删：上下文被 0064 取代、宽授权有害，见 GPU-PORT-NOTES §14） |
| 本地自测 | `cd k60-repo-full/tools/adreno-selftest && ./run.sh 0185` → 14 项全过；`gmu-model` 11 项全过 |
| CI 内核（0179–0183） | `artifacts/kernel-adreno-0183.sweet`，17,303,824 字节，`NOUNDEFS` |
| 可刷镜像 | `artifacts/xnu-adreno-m1m3.img`（cmdline `sweetadreno=1 sweetadreno_stage=3`）——**不含 0185** |
| 未推 | 本轮提交（等内存侧修完提交归属） |
| M3 上传 | 未实现；cache 段要 GPU 可见内存、HFI 要 IOVA，依赖 kgsl SMMU |

**设备回来后的执行顺序**

1. `K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_stage=3 sweetadreno_fwinfo=1" ./tools/k60-build-boot.sh <最新内核> artifacts/xnu-adreno.img`
2. `fastboot boot artifacts/xnu-adreno.img`（M1+M2+M3 只读，一次启动）
3. `./tools/k60-log-pull.sh` + `python3 tools/k60-log-summary.py`
4. 判据顺序：`fw vector … plausible` → `probe done` → `snapshot[pre-power]` →
   `GPUCC verified: n/7` → GMU 三段状态。全绿才谈 M3 上传（先要有 SMMU）。

## 2026-10-26 离线轮之后（先读这段）

- **GPU**：`0179` 的寄存器表核对无误，但时钟等待有 3 处与 vendor 语义不符，
  已写 `0180-k60-adreno-clock-waits.patch`（本地分支，**未推送**），并用
  `k60-repo-full/tools/adreno-selftest` 仿真验证（0179 三项 FAIL → 0180 全绿）。
  细节、四层对照表、M1 判据全在 `GPU-PORT-NOTES.md`。
- **M1 判据改了**：别再等「chipid 0x7030002」——那是 DT 常量不是寄存器。
  现在看 `SWEET ADRENO: probe done` + 各块不是全 `0xffffffff`。
- **构建**：`K60_CMDLINE_EXTRA="sweetadreno=1 sweetadreno_verbose=1" ./tools/k60-build-boot.sh <kernel> <out.img>`，
  已实测能打出 cmdline 正确的镜像（`artifacts/xnu-adreno-cmd.img`）。但现有
  `kernel-accel.sweet` **不含 Adreno 代码**，要等 CI 编出 0179/0180 的内核。
- **CI 已出内核（2026-10-26）**：run `40276193603` 的 `xnu-kernel` 94.0MB，内核
  17,303,824 字节、Mach-O `NOUNDEFS`、含 0179–0183 全部字符串。注意 CI 的 `Build`
  是红的（自 10-23 起一直红、与 Adreno 无关），但 `Collect` 是 `if: always()`，
  产物照样可用——别再用「等绿」当判据。
- **镜像已就绪**：`artifacts/xnu-adreno-m1m3.img`（cmdline 含
  `sweetadreno=1 sweetadreno_stage=3`），设备回来直接
  `fastboot boot` 它，一次启动就能看 M1→M2→M3 三段。
- 代理与模型两件事本轮已改（`~/.local/bin/miyu`、`opencode` 包装器；
  `~/.miyu/config/config.jsonc` 默认模型 → `glm-5.3`）。Gitee 仍然搁置。

## 状态一句话

iOS 除"画像素"外全通：内核、文件系统、launchd、SpringBoard、显示管线、双缓冲 swap
都在跑，所有合成帧为零，因为内核没有 GPU、CA 拿不到渲染器。

## 下一步（设备回来时）

```bash
cd ~/hackios
./tools/k60-build-boot.sh artifacts/kernel-<最新>.sweet xnu-adreno.img
```

构建时把 cmdline 换成带 `sweetadreno=1` 的版本（手工跑 mkpayload 那三步），
然后 `fastboot boot xnu-adreno.img`，看是否打印：

```
SWEET ADRENO: probe done
```

chipid 应为 0x7030002。通过之后再试 `sweetadreno_power=1`。

## 未推送的提交

`k60-repo-full` 里 `eff0717`（Adreno 骨架）还在本地，网络恢复后先推：

```bash
cd ~/hackios/k60-repo-full
git push https://x-access-token:$(cat ~/.config/gh-token)@github.com:443/XTOM0706/ios-sweet-k60.git HEAD:main
```

## 镜像与资产

| 文件 | 说明 |
|---|---|
| `iosroot/root-hfsx.img` | 当前根镜像（含探针、CA_NO_ACCEL、迁移重置）|
| `artifacts/kernel-accel.sweet` | 含 0177/0178（scaler + accelerator stub）|
| `gpu-fw/` | a730 固件（sqe、zap、gmu），GPU 路线必需 |
| `ADRENO-PLAN.md` | 寄存器地图 + M1–M4 里程碑 |
| `NEXT-ROUND-PROMPT.md` | 下一轮的完整提示词 |

## 已确认的关键事实（别再重复验证）

- launchd 不再 SIGSEGV；根因是 memSize 给大了（0172）
- 帧缓冲页不能进空闲队列，也不能在 vm_page 数组里挖洞（0170）
- 日志看 total 不看 seq；`ufslogsecs=1` 最多丢一秒
- 整 MiB 块补丁不能用于可写 HFS
- fastboot 不稳，用 TWRP 的 gzip 流式写入

## 2026-10-26 第二轮（0181/0182，本地全绿）

- 上游写集合逐条对上（48 次写一一对应），剩下的差异都在"判据"层，已修：
  0181 预检 + 收尾验证；0182 分阶段 `sweetadreno_stage=1|2|3` + 五点寄存器快照。
- 本地校验台 12 项全过：`cd k60-repo-full/tools/adreno-selftest && ./run.sh 0182`。
  （含 prepowered / stuck / mapfail / 无 boot-arg / M1 零写 / GMU 模型 11 项）
- GMU 偏移坑：`w*4-0x68000`，不是 `w*4`。
- 待拍板：GMU 固件怎么进内核（A 嵌 C 数组 / B boot payload 段 / C 先不做），
  建议 A。决定之前 M3 只能停在只读态。
