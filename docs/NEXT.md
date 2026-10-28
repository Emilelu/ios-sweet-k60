# 回来后从这里继续

> **2026-10-28 锁屏达成**：显示链路通了，K60 能进 iOS 16.7.16 锁屏。
> 配方与判据见 `docs/LOCKSCREEN-2026-10-28.md`（0218 发布 TimingElements /
> ColorElements、选择器 77/79、GetID=1、画布按面板 1440×3200）。
> 现在的阻塞是触摸：读 RSC 第一个地址就整机死，等上游修好再合入；
> 壁纸不是 GPU 问题，更可能是跳过 Setup 后从未配置过。
> 下面 10-26 的记录是关于 GPU 那条线的，仍然有效但优先级已降。

> **2026-10-26 18:21 上机结果（0186）：门禁 PASS，M1/M2 全过，卡在 M3**
> - `SWEET GATE: free=41272 physBase=0xb4000000 memSize=0x4c000000 **memActual=0x2bf00000**
>   topk=0xb64e8000 dram=0x0 **managed=0x2bf00000** gPhysSize=0x2bf00000` → `gate: PASS`
>   （模型 41270 vs 实测 41272，差 2 页 = 面板 notch 实际 1150 页，模型待修）
> - 0186 证明有效：日志第 1–198 行是**本次启动自己的** console（修复前必丢）；重放区只剩
>   199–268（3,842 B）。
> - **Adreno M1 `probe done`**：gpucc/ao/gmu 读数为 **0**（不是 0xffffffff，路径通）。
> - **Adreno M2 `GPUCC verified: 7/7` / `GPUCC up`**：preflight 时 bootloader 只开了 3/7，
>   六个阶段跑完 7/7，post-power `gdsc=0xf8228800 hwctl=0x80001fe0`。
> - **M3 未做**：`GMU is held in reset (syreset=0x1)`、`FW_INIT=0`、`HFI=0`。0185 只嵌入固件，
>   装载器还没写 —— 下一步照 `refs/UniXNU-pong/drivers/PongAdreno/{PongAdrenoGmu.c,PongAdrenoHfi.c}`
>   移植（ITCM `0x1b400` 按字、`CM3_BOOT_CONFIG`、`CM3_SYSRESET`、轮询 `FW_INIT_RESULT` 0xe00）。
> - 干扰项：45 s 后仍被 `SWEET TOUCH: gcc probe vote read` 打死 → 下一轮换 `sweettouchoff=1`。
> - 日志 `/tmp/b0186.txt`（135,755 B）；镜像 `artifacts/xnu-0186-p0.img`（sha256 前 16 位 `5e3a523bdf191252`）。
> - 完整证据：`GPU-PORT-NOTES.md` §20。

> **2026-10-26 18:00 两线并行（当前）**
> - **A 线（设备）**：CI run `40300004313`（head `6874c9d`，含 0186）产物 `xnu-kernel`
>   93,924,078 B 已出（`Build` 红是老毛病、`Collect` 绿）。正用
>   `tools/ci-fetch-kernel.sh 40300004313` 拉取；**到手后必须先验**：含 `SWEET GATE` 字符串
>   + Mach-O `NOUNDEFS`，否则不能上机。上机判据只有一条：`SWEET GATE: free=41271±10`。
> - **B 线（离线）**：`c23ce2` 在挖 QuartzCore 的渲染器选择逻辑（目标：让 CA 走
>   `CA::OGL::SW` 软件光栅化，绕开 Adreno 也能出真界面）。素材 = `iosroot/root-hfsx.img`
>   里的 dyld 共享缓存，用 `7z` 只提取需要的文件。
> - 仓库：`6874c9d`（0186）+ `7721dcd`（shim 同步/回读令牌/memplan 校准/文档 §18.10/§19）
>   都已推到 `main`；后者不含 `xnu/patches/**`，**没有**触发新 CI。
> - 真值校准：门禁是 **41271**（不是 41272；41272 属于更短内核 `kernel_end 0x84d8000`）。
>   `memplan.py KERNEL_FOOT=0x24e4000`、`k60-inspect-boot.py` 按 `(base,size,foot)` 索引。

### 0187 待办（0186 已验证但有三处行为要改，见 GPU-PORT-NOTES §19 与 logtail-selftest）
1. `tail` 打满 16 KiB 时补一行显式截断标记（现在尾部大 hexdump 会把 panic 正文挤掉，
   且头行读起来像完整日志）。
2. 48 行是**下界**不是上限（密集换行实测 232 行）；要么注明，要么改成先找行边界。
3. 打印起点落在 4096 块边界，首行常是半行；改成回退到上一个换行会更像旧行为。
> 这三条不影响本轮（存活日志 `total` ~150 KB，走不到 16 KiB 上限）。

### 已知的"别再做"清单
- 别再怀疑 shim/ADT 交付链：反汇编已证 boot_args 只有一份、来自 `x0`、偏移与 shim 逐字段一致。
- 别再把日志第 16 行当本次页数：那是 `sweet_log_dump()` 重放的**上一次**启动内容。
- 别用 `artifacts/xnu-diag-*512.img`、别信 `maxmem=512`/ADT 511 那一支（治的是不存在的病）。

> **2026-10-26 17:00 更正（先读这条）**：§17.4 的"内存门禁失败"是**读错行**。
> `/tmp/verify.txt` 前 13,531 字节是上一次启动的 console 被本次启动 `sweet_log_dump()`
> 重打进来的（与 `/tmp/k60-adreno1.txt` 前 176 行逐字节相同，连 `3626 microsec`
> 都一样），本次启动自己的 `vm_page_bootstrap` 根本没进环。那份 55885 属于更早一次
> base=0x80000000 的旧 ADT 启动（sim 逐页复现）。
> → **本轮门禁没测到，不是没通过；"boot_args/ADT 交付链断了"这个判断撤回。**
> 下一步不是查链路，而是把页数打印挪到 dump 之后（`sweet_log_dump()` 之后、
> `SWEET LOG: thread running` 一带）。证据与改判见 `GPU-PORT-NOTES.md` §18。
> 规划见 `PLAN-BOOTARGS-CHAIN.md`。

> **2026-10-26 16:50 上机轮结果（先读这段）**：内存门禁 **失败**（`vm_page_bootstrap`
> 55885 页，不是 41271），Adreno **一行输出都没有**，启动仍在 45 s 后被
> `SWEET TOUCH: gcc probe vote read` 打断。**这一轮对 GPU 无结论，p1/p2 继续禁刷。**
> 完整证据与下一步见 `GPU-PORT-NOTES.md` §17。

## 2026-10-26 内存轮（离线，别和 GPU 那轮混）

- **703 MB 不是天花板**，它只是"payload 在 `0xb4000000`、上界卡在 gunyah 前"这个摆法的
  `dram-size`。真正的可用内存是 **41271 页 = 644.9 MiB**（内核 + 面板 + vm_page 数组都在这 703 里）。
- 两个方案已算清，本地校验台全绿：`cd k60-repo-full/tools/memplan-selftest && ./run.sh`
  - P1（只抬上界，**+74.5 MiB** → 719.4 MiB）：`MEMPLAN=p1 ./tools/k60-build-boot.sh ...`
  - P2（payload 下移到 `0xa3000000`，**+337.5 MiB** → 982.3 MiB）：`MEMPLAN=p2 ...`
- 两个方案共用**一个**新补丁 `0184-k60-ram-notches.patch`（把 0170 的单一面板判断换成
  5 段 notch 表）。**0184 必须先进内核**，否则抬窗口就会踩 gunyah → 无声挂死。
- 上机判据：`vm_page_bootstrap` 页数应为 **46041（P1）/ 62868（P2）**，不是 41271。
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
