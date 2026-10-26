# K60 内存计划：703 MB 是不是天花板

> 同源副本：`~/hackios/MEMORY-PLAN.md`（本轮新增，两边内容一致）。

2026-10-26 离线轮。设备不在手，本篇所有结论分成三类并明确标注：

- **本地已验证**：可在 x86 上跑、可复现（`k60-repo-full/tools/memplan-selftest`）；
- **静态推导**：从 `dump/live.dts` + `dump/iomem.txt` + XNU 源码推出来的，没有不确定性，
  但依赖那两份 dump 是同一台机器同一固件；
- **needs-device**：必须上机，判据写在 §7。

---

## 0. 一句话结论

**703 MB 不是天花板**，它只是"内核正好压在 `0xb4000000`、向上只能管到 gunyah 块之前"
这一种摆法算出来的 `dram-size = 0x2bf00000`。真正的约束有两条，都不是内存本身：
`avail_start` 永远等于 `topOfKernelData`（payload 落在哪，下界就在哪），而 arm64 XNU 只
认**一个连续窗口** `[avail_start, avail_end)`。窗口内不是我们的部分（固件 carve-out、
gunyah、qtee、无 DRAM 的洞）用"页面照建、永不入空闲队列"的 notch 机制排除即可 ——
`0170` 已经证明这条路能走。

| 方案 | 改什么 | ADT dram-size | 空闲页 | 相比今天 |
|---|---|---|---|---|
| **P0** 现状 | — | 703.0 MiB | 41272 页 / **644.9 MiB** | — |
| **P1** 抬高上界 | ADT 窗口 + notch | 788.9 MiB | 46042 页 / **719.4 MiB** | **+74.5 MiB** |
| **P2** 下移 payload | ADT 窗口（同上补丁） | 1060.9 MiB | 62869 页 / **982.3 MiB** | **+337.5 MiB** |

P1/P2 都只需要**同一个补丁**（`0184-k60-ram-notches.patch`）。P2 额外要一台上机确认
"`0xa3000000` 这一段现在没人用"（§7 第 2 条）。再往上（把窗口拉到 4 GiB）净收益只有
`0xf4600000` 那 186 MiB，而它被 Linux 标成 172 MiB reserved，风险不对等，列成实验项不入
方案（§6）。

顺带修正一个一直在用的数字：**703 MB 是 ADT 告诉 XNU 的 dram-size，不是可用内存**。
内核自己 + ADT + boot_args 占掉约 37 MiB，面板帧缓冲 18 MiB 不能进空闲队列，
vm_page 数组再吃掉约 3.7 MiB，所以今天真正能分配的只有 **41272 页 = 644.9 MiB**。

---

## 1. 方法：两份互不依赖的证据

| 来源 | 给的是什么 | 不能给什么 |
|---|---|---|
| `dump/live.dts`（ABL 交给 Android 的活 FDT） | DRAM bank 边界；38 个固定 no-map carve-out 的精确地址 | 哪些地址 AP **真的**能解码。它把 `0x80000000..0xea000000` 全叫 memory，其中 603 MiB 是固件 |
| `dump/iomem.txt`（同一台机器 Android 的 `/proc/iomem`） | AP 实际拿到的 `System RAM` 区间；哪段被谁保留 | 为什么缺。无 DRAM 的洞和 XPU/TZ 保护区在 iomem 里都是"没有这一行" |

所以做法是：**用 iomem 判可达性，用 DTS 判归属**，两边对不上就以 iomem 为准并标注。
`memplan.py` 把两份都解析出来，DTS 侧 38 个 carve-out 合计 560.1 + 43.0 = **603.1 MiB**，
和 2026-10-24 手数的"38 个区域、603 MiB"对得上，说明这份 dump 没跑偏。

---

## 2. 真实物理内存布局

### 2.1 DRAM bank（DTS `/memory`，`ddr_size = 0x0c` = 12 GiB 颗粒）

| # | 区间 | 大小 | 说明 / 可达性 |
|---|---|---|---|
| 1 | `0x0_80000000 - 0x0_ea000000` | 1696 MiB | 低 bank。**只有一部分可用**，见 §2.3 |
| 2 | `0x0_f4600000 - 0x1_00000000` | 186 MiB | 4 GiB 前的尾部。Linux 标 172 MiB reserved，**needs-device**（§7.4） |
| 3 | `0x8_00000000 - 0x8_37500000` | 885 MiB | 4 GiB 以上，XNU 侧**不可访问**（`FAR=0x1_00000000` 同步外部中止） |
| 4 | `0x9_00000000 - 0xa_80000000` | 6144 MiB | 同上；与 #5 重叠（vendor DTB 的写法，Linux 合并成 `0x840000000-0xa7fffffff`） |
| 5 | `0x8_40000000 - 0x9_00000000` | 3072 MiB | 同上 |
| 6 | `0x8_37b00000 - 0x8_39900000` | 30 MiB | 同上 |

去重后 AP 视角的 DRAM 总量约 8941 MiB（12 GiB 里剩下的在 TZ/hyp 手里，DT 根本不列）。
**4 GiB 以上整段不可用**是硬约束，本篇不再碰。

### 2.2 固件 carve-out（DTS `/reserved-memory`，38 个固定区间，603.1 MiB）

全表由工具打印（`memplan.py --table`），这里只按归属分组，标出对本计划有用的边界：

| 组 | 区间 | 合计 | 备注 |
|---|---|---|---|
| hyp / AOP / XBL log | `0x80000000-0x808f4000` 内 8 段 | 7.2 MiB | 内核绝对不能落在这里（旧 `k60.adt` 的根因） |
| smem / cpucp / cdsp secure heap | `0x80900000-0x85200000` 3 段 | 73.0 MiB | |
| video / slpi / cdsp / IPA / spss / spu | `0x85700000-0x8bc00000` 9 段 | 66.1 MiB | |
| mpss（modem） | `0x8bc00000-0x9ee00000` | 306.0 MiB | 低 bank 里最大的一块 |
| cvp / camera / adsp | `0x9ee00000-0xa2e00000` 3 段 | 64.0 MiB | adsp 正好结束在 `0xa2e00000` |
| xbl_ramdump / xbl_sc | `0xa6b80000-0xa6e40000` | 2.75 MiB | **P2 窗口内的第一个洞** |
| global_sync | `0xa6f00000-0xa7000000` | 1 MiB | P2 窗口内 |
| ramoops | `0xa7000000-0xa7400000` | 4 MiB | P2 窗口内（Linux 也 reserved） |
| splash（面板） | `0xb8000000-0xbab00000` | 43 MiB | 只有前 18.4 MiB（1440×3200×4）被 ABL 扫描，notch 只盖这一段 |
| gunyah / cpusys_vm / hyp_res | `0xe0000000-0xe0b00000` | 11 MiB | **P1/P2 的第二个洞**，`e9f00000` 那次中止的近亲 |
| trust_ui_vm（qrtr/vblk/swiotlb） | `0xe55f3000-0xe5700000` | 1.06 MiB | P1 的窗口上界就取在它前面的 `0xe54f0000` |
| tz_stat / tags / qtee | `0xe8800000-0xea000000` | 24 MiB | **`0xe9f00000` 落在 qtee 里**，`ESR=0x96000010 FAR=0xE9F00000` 的出处 |

### 2.3 AP 真的能解码的连续区段（iomem `System RAM`，< 4 GiB，合计 1325.9 MiB）

| 区间 | 大小 | 判断 |
|---|---|---|
| `0x80640000-0x80800000` | 1.75 MiB | 被 hyp 夹住，忽略 |
| `0x808f4000-0x80900000` | 0.05 MiB | 碎片 |
| `0x85200000-0x85700000` | 5 MiB | 碎片（video 与 cdsp 之间） |
| `0x85e00000-0x88000000` | 34 MiB | 碎片（slpi 之前，够不着） |
| `0x8b91c000-0x8ba00000` | 0.9 MiB | 碎片 |
| **`0xa2e00000-0xa6b80000`** | **61.5 MiB** | **P2 的 payload 新家**：放 37 MiB 内核后还剩 21 MiB 余量 |
| `0xa6e40000-0xa6f00000` | 0.75 MiB | 碎片 |
| **`0xa7000000-0xe0000000`** | **912 MiB** | 今天窗口的主体。里面 164 MiB（`0xd5c00000-0xe0000000`）被 Linux 运行时保留，但**现在就在我们的窗口里跑了很多轮** → 是普通 RAM，不 notch |
| **`0xe0b00000-0xe55f3000`** | **74.9 MiB** | **P1 的增量**，和 2026-10-25 记的"+74.9 MB"一致 |
| `0xe5700000-0xe8800000` | 49 MiB | 其中 44 MiB 被 Linux 保留；剩 5 MiB 值得不上 |
| `0xf4600000-0x100000000` | 186 MiB | 其中 172 MiB 被 Linux 保留；`needs-device`（§7.4） |

合计 1325.9 MiB。**能被一个连续窗口吃到的上限就是这个数减去内核占位**，
所以 P2 的 982 MiB 已经接近这个窗口模型的天花板。

### 2.4 一张边界表（后面所有方案都只用这些数）

| 地址 | 是什么 | 证据 |
|---|---|---|
| `0x80000000` | hyp_region 起点，内核不能落这里 | DTS + 旧 ADT 崩 |
| `0xa2e00000` | adsp 结束、61.5 MiB 空闲开始 | iomem System RAM |
| `0xa6b80000` | 空闲结束（xbl_ramdump 起） | iomem reserved |
| `0xb4000000` | 今天的 payload / ADT dram-base | `0172`、`k60-build-boot.sh` |
| `0xb8000000` | 面板帧缓冲，18.4 MiB 不能进空闲队列 | `0170` |
| `0xbab00000` | splash carve-out 结束；之后是普通 RAM | 当前窗口已在使用 |
| `0xd5c00000` | Linux 运行时保留 164 MiB 开始；**对本内核是普通 RAM** | 当前窗口已在使用 |
| `0xe0000000` | gunyah / cpusys_vm / hyp 保留 11 MiB | DTS |
| `0xe0b00000` | 74.9 MiB 自由 DRAM 开始 | iomem |
| `0xe55f3000` | trust_ui_vm 起 | DTS |
| `0xe8800000` | tz_stat/tags/qtee 起（TZ 所有） | DTS + `ESR=0x96000010 FAR=0xe9f00000` |
| `0xea000000` | **没有 DRAM**（不在任何 bank 里）直到 `0xf4600000` | DTS memory node |
| `0xf4600000` | 4 GiB 前最后一个 bank | DTS；shim 每次把 payload 暂存到 `0xf8000000`，说明可写（MMU off） |
| `0x1_00000000` | 4 GiB 墙，访问即中止 | 已多次验证 |

---

## 3. 为什么"703 MB 不是天花板"（推导，可对照源码）

1. `osfmk/arm64/arm_vm_init.c:2000`：`avail_start = args->topOfKernelData;`
   —— **下界不是 dram-base，是 payload 的结尾**。把 dram-base 调低而 payload 不动，
   一点内存都多不出来。反过来，payload 落在 `0xa3000000`，下界就变成
   `0xa54e0000`（`kernel_end + ADT + boot_args`）。
2. `arm_vm_init.c:2017`：`avail_end = gPhysBase + mem_size;`
   —— 上界就是一个数。arm64 没有 x86 那种 memory map，`pmap_startup()` 用
   `pmap_free_pages_span()` 一次性线性枚举 `[first_avail, avail_end)`。
   **想要两段不相邻的内存，得改 pmap 的取页和 vm_page 打包，那是十几个文件的活**。
3. 但"窗口里有一段不是我们的"是**已经解决**的问题：`0170`
   （`osfmk/vm/vm_resident.c`）把命中条件的页从 `vm_page_release_startup()` 前拦下来。
   页照样 `vm_page_init()`（不碰页面本身），只是永不入空闲队列，所以分配器永远拿不到。
   这比"在 `pmap_next_page()` 里跳过"正确 —— 后者会撞 `vm_page_init` 的
   `phys_page == VM_PAGE_GET_PHYS_PAGE(m)` 断言，因为 arm64 的 vm_page 是用**数组下标**
   反推物理页号的（`VM_PAGE_PACKED_FROM_ARRAY`）。
4. `0172` 的作用没变：shim 仍然把 `boot_args.memSize` 填成 physmap 跨度（`0x100000000-base`），
   VM 管多少由 `memSizeActual`（= ADT dram-size）决定。P1/P2 只是把这个数换大，
   同时把"新窗口里不是我们的段"加进 notch 表。
5. 于是"天花板"= **一个连续窗口内能放出的最多页** = (可达 DRAM 在窗口内的部分)
   − (carve-out) − (vm_page 数组)。按 §2.3，可达 DRAM 全在 4 GiB 以下合计 1325.9 MiB，
   扣掉内核占位和够不着的小碎片，P2 的 982.3 MiB 基本就是单窗口上限。

---

## 4. 方案对比

| | P0 现状 | P1 抬高上界 | P2 下移 payload | P3 拉到 4 GiB（实验） |
|---|---|---|---|---|
| ADT dram-base | `0xb4000000` | `0xb4000000` | **`0xa3000000`** | `0xa3000000` |
| ADT `ADT_MEM_SIZE` | `0x2c000000` | **`0x315f0000`** | **`0x425f0000`** | `0x5d000000` |
| dram-size（= 报给 XNU） | `0x2bf00000` | `0x314f0000` | `0x424f0000` | `0x5cf00000` |
| 管理窗口 | `0xb64e0000-0xdff00000` | `0xb64e0000-0xe54f0000` | `0xa54e0000-0xe54f0000` | `0xa54e0000-0xfff00000` |
| panic log | `0xdff00000` | `0xe54f0000` | `0xe54f0000` | `0xffe00000`（危险） |
| notch 页数 | 1152（面板） | 1856 | 2352 | 还要 +6 段 |
| 空闲页 / MiB | 41272 / 644.9 | 46042 / **719.4** | 62869 / **982.3** | ~1040（乐观 1406） |
| 可行性 | 已在跑 | 高 | 中（要确认 payload 落点） | 低 |
| 风险 | — | 只多 11 MiB 固件区在图里，已有 `ESR` 先例说明必须 notch | ABL 可能在自己的 kernel 载入地址上压到新窗口；61.5 MiB 洞里还有没有别的东西没人看过 | 跨 171.5 MiB 无 DRAM 洞 + 172 MiB Linux 保留区，panic log 落在"≥`0xe9f00000` 会中止"的规则里 |
| 需要补丁 | `0172`（已有） | **1 个**（`0184`） | **1 个**（同一个 `0184`） | `0184` + panic-base 覆盖 + 探针，共 3 个 |

推荐顺序：**先 P1**（改动最小、收益确定、上机判据单一），P1 稳定后再上 P2。
P2 与 P1 是同一个补丁，切换只是 `MEMPLAN=p2 ./tools/k60-build-boot.sh ...`。

---

## 5. 两个推荐方案的分段地址表

notch 表（`0184`，内核里静态表，5 段，全部 16 KiB 对齐）：

| 段 | 理由 |
|---|---|
| `0xa6b80000-0xa6e40000` | xbl_ramdump + xbl_sc，只有 P2 窗口会碰到 |
| `0xa6f00000-0xa7000000` | global_sync |
| `0xa7000000-0xa7400000` | ramoops（Linux 也 reserved） |
| `0xb8000000-0xb9200000` | 面板 1440×3200×4（沿用 `0170` 的边界） |
| `0xe0000000-0xe0b00000` | gunyah + cpusys_vm + hyp_reserved |

### P1（推荐先做）

```
ADT dram-base   0xb4000000
ADT dram-size   0x314f0000      (788.9 MiB)
avail_start     0xb64e0000      = dram-base + kernel_end(0x24d8000) + ADT + boot_args
avail_end       0xe54f0000      (dram-base + dram-size)
panic log       0xe54f0000 - 0xe55f0000   （窗口上方 1 MiB，DRAM 内，避开 trust_ui）
空闲队列        0xb6888000-0xb8000000   ( 23.5 MiB)
                0xb9200000-0xe0000000   (622.0 MiB)
                0xe0b00000-0xe54f0000   ( 73.9 MiB)
                = 46042 页 / 719.4 MiB
```

命令行：`MEMPLAN=p1 ./tools/k60-build-boot.sh <kernel> out.img`
（`k60-build-boot.sh` 已加 `MEMPLAN=p0|p1|p2`，默认 p0 行为字节不变。）

### P2（payload 下移 274 MiB）

```
ADT dram-base   0xa3000000      (在 61.5 MiB 空闲洞的起点，37 MiB 内核放得下)
ADT dram-size   0x424f0000      (1060.9 MiB)
avail_start     0xa54e0000      = dram-base + 0x24e0000
avail_end       0xe54f0000
panic log       0xe54f0000 - 0xe55f0000
空闲队列        0xa59dc000-0xa6b80000   ( 17.6 MiB)
                0xa6e40000-0xa6f00000   (  0.8 MiB)
                0xa7400000-0xb8000000   (268.0 MiB)
                0xb9200000-0xe0000000   (622.0 MiB)
                0xe0b00000-0xe54f0000   ( 73.9 MiB)
                = 62869 页 / 982.3 MiB
```

命令行：`MEMPLAN=p2 ./tools/k60-build-boot.sh <kernel> out.img`
（同一个 `0184`，不额外改内核。）

P2 之所以不用再动内核：`grep` 过 `xnu/` 与全部补丁，**没有任何一处把 `0xb4000000`
当内存基址**（只有 shim 的 `K60_RDCHECK` 里当显示字符串）。physmap 基址、`phystokv()`、
`ml_static_mfree()` 全部从 `boot_args.physBase` 推出来，`mkpayload.py` 的
`phys_base/mem_size` 也是从 ADT 的 dram-base/dram-size 读的（实测三个方案的 payload
头里 `phys_base` 分别是 `0xb4000000/0xb4000000/0xa3000000`）。

---

## 6. 不推荐但现在就能算清的两条路

**a) 多 bank / stage-2 alias。** 理论上可以把 `0xa2e00000` 和 `0xe0b00000` 两段分别喂给
VM。但 arm64 的 `pmap_startup()` 是线性的（§3 第 2 条），vm_page 用下标反推页号，
`pmap_free_pages_span()` 只有一个 span。要支持多段得动 `pmap.c` 的取页、`vm_page.h` 的
打包、`vm_resident.c` 的计数，还要让 `ml_static_mfree` / `pmap_steal` 都认这套 ——
**收益是 P2 之外的 5 MiB 碎片 + 4G 以上（不可达）**，完全不划算。stage-2 alias 更差：
XNU 的 physmap 要的是物理连续，用 hyp 做重映射等于把 hypervisor 也拖进来。

**b) 把窗口拉到 4 GiB（P3）。** 唯一新增的可达 DRAM 是 `0xf4600000` 那 186 MiB，
其中 172 MiB 被 Linux 标成 reserved；窗口要跨过 `0xea000000-0xf4600000`（171.5 MiB
**根本没有 DRAM**）和 tz/qtee 24 MiB。乐观上限 1406 MiB、保守 ~1040 MiB，但 panic log
只能放在 `0xffe00000`，正好违反"≥`0xe9f00000` 访问即中止"的既有结论。除非 §7.4 的探针
证明那条规则其实只针对 qtee，否则别碰。

**c) `0xf4600000` 那个 bank 值得单独一问。** shim 的 `PAYLOAD_SAFE_PA = 0xf8000000`
每次启动都把 17 MiB payload 拷过去再读回来（否则内核根本起不来），也就是说
**AP 在 MMU off、Device 属性下写 `0xf8000000` 是成功的**。而 `0xe9f00000` 的中止发生在
qtee 里。所以"`0xe9f00000` 以上都被 XPU 拦"这句话很可能是把
"qtee/tz 保护区 + 无 DRAM 洞"误当成了整段。这条只影响 P3，不影响 P1/P2，
但值得用 §7.4 的探针把结论钉死。

---

## 7. needs-device：上机步骤与判据

前提：P1 的镜像用 `MEMPLAN=p1 ./tools/k60-build-boot.sh <kernel> out.img` 现生成；
`0184` 必须先被 CI 编进内核（否则 notch 只有面板那一段，窗口一抬就会踩 gunyah）。

**1. 确认 notch 生效（最关键的判据）**
- 看 `vm_page_bootstrap: N free pages`：
  - P1 应看到 **46042 页**（±10 页，差在 `sizeof(vm_page)` 和 ADT 长度；
    换内核后 `kernel_end` 每变 1 MiB 这个数就少 64 页，先看 `PHYS`/`SEGS` 那两行）；
  - P2 应看到 **62869 页**；
  - 如果仍是 **41272 左右**：`0184` 没进内核（或 `MEMPLAN` 没生效）；
  - 如果看到 **大于 47000（P1）/ 64000（P2）**：dram-size 或 notch 有一个错了，
    **立刻停机**，这就是 2026-10-25 那次无声猝死的形状。
- 再看 `SWEET MEM: physmap ... MiB, VM manages ... MiB (dram-size)`：P1 应是
  `physmap 1216 MiB, VM manages 788 MiB`；P2 `physmap 1488 MiB, VM manages 1060 MiB`。
  （`0172` 那行 printf，physmap 不变，VM 变大。）

**2. P2 的 payload 落点（只有 P2 需要）**
- 目的：确认 ABL 把我们的 boot.img kernel blob 放在哪，别让 shim 在拷贝时把自己盖掉。
- 做法：开机看 shim 控制台第一屏的 `PAYLOAD self-image at +N lo <addr>`（`loader.c`
  无条件打印），拍照记下 `lo`。
- 判据：`lo + 0x40000`（shim 镜像大小 + 余量）**不在** `0xa3000000-0xa54e0000` 内 → 安全。
  典型值是 `0x80008000` 一带（ABL 的 kernel 载入地址），那就在安全区。
  如果恰好落在里面，换 `ADT_PHYS_BASE=0xa2800000`（整段下移 8 MiB）或退回 P1。

**3. P1/P2 的长时间稳定性**
- `sweettouchoff=1`，跑满 6 分钟，预期和 `xnu-p6-notouch.img` 一样：
  SpringBoard 活着、`SWEET FB: swap` 继续、没有 `ABORT` 行。
- 判据：日志环 `total` 持续增长；没有 `ESR=0x9600001x`（同步外部中止）/ `0xbf000002` 之类；
  没有 `compressed PTE ... corrupted`。
- 反向判据：任何一次"日志停在某个 MMIO/分配点、屏幕不动、无 panic"都要当成
  notch 漏了一段，用 `tools/k60-log-summary.py` 看最后一行。

**4. `0xf4600000` bank 探针（只影响 P3，可选）**
- 现成工具：`ios-sweet/shim/shim.c` 里的 `K60_RDCHECK` 块（编译时 `-DK60_RDCHECK`），
  把 `addrs[]` 换成下面这组，屏上逐行打印读到的值：
  `0xa2e00000, 0xa3000000, 0xa54e0000, 0xa6b7f000, 0xa6e40000, 0xa7400000,
   0xb6810000, 0xb8000000, 0xd5c00000, 0xe0000000, 0xe0b00000, 0xe54f0000,
   0xe55f2ff0, 0xe5700000, 0xe87ffff0, 0xe8800000, 0xe9f00000, 0xea000000,
   0xf45ffff0, 0xf4600000, 0xf8000000, 0xfffff000`
- 判据：**最后一行打印出来的地址之后的那一行不出现 = 该地址不可解码**（读会中止，
  没有异常向量，屏定格）。已知会停的是 `0xe9f00000`（qtee）和 `0xea000000`（无 DRAM）；
  如果 `0xf4600000` / `0xf8000000` 能读出来，P3 才有讨论价值。
- 注意：这是 MMU off、Device 属性下的读，和 XNU 的 Normal cacheable 访问不是一回事，
  只能证伪不能完全证实；不同就说明硬件层面的解码没问题。

---

## 8. 别的内存杠杆（别只盯着这 300 MB）

FORECAST 里已经写过的结论，这里再钉一次：**真正的大头是 dyld 共享缓存**。现在每个
进程各自映射一份 dylib，同一份库在物理内存里重复 N 次；共享缓存修好后能省下的
是**数百 MB**，比 P2 的 +337 MiB 还多，而且不涉及任何地址窗口风险。
P1/P2 的价值在于"多 75/337 MiB 换来不再频繁 jetsam"，不是"解决内存问题"。

---

## 9. 本轮改动

| 文件 | 说明 |
|---|---|
| `k60-repo-full/xnu/patches/0184-k60-ram-notches.patch` | **新补丁**：把 `0170` 的单一面板判断换成 5 段 notch 表；`git apply --check` 通过 |
| `k60-repo-full/tools/memplan-selftest/` | 新增离线校验台（`run.sh` 全绿） |
| `k60-repo-full/tools/k60-build-boot.sh`、`~/hackios/tools/k60-build-boot.sh` | 加 `MEMPLAN=p0\|p1\|p2`（默认 p0，行为与之前一致；已实测三种都能出 ADT） |
| `~/hackios/MEMORY-PLAN.md` | 本篇 |

没动：`SweetAdreno.cpp`、`0180*`、`tools/adreno-selftest/`、`GPU-PORT-NOTES.md`、
`shim/shim.c`、远端（无 push）。

本地跑一遍：

```
cd k60-repo-full/tools/memplan-selftest && ./run.sh
```

它会重算物理表、跑页面模型、检查 0184 的 notch 覆盖、验证 mkadt 生成的 ADT，
并用三次坏窗口和一次真实启动（55885 页）做正反对照。
