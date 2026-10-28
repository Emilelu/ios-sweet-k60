# 0207 与 0206：两条路，不是一个补丁叠一个补丁

> 状态（2026-10-27）：**0207 已在 main（e136115）。0206 没有交付，也没有进
> `xnu/patches/`。** 它放在 `/tmp/t0206/0206.patch`，等 0207 的真机结果再决定。

## 1. 问题是什么

`SweetAdreno` 要给 CM3（GMU 固件）建一张 SMMU stage-1 页表。0205 把 cache 窗口
扩成整个 16 MB（iova `0x4000..0x1000000`），于是数据块变成 `0x1003000`（16.01 MB），
并且用 `inTaskWithPhysicalMask(...)` 要求它**整块物理连续**。

这块内存在延迟工作里才分配（`sweetadreno_delay`，默认 10 s，实测常用 120 s）。
真机日志：

```
smmu ctx: cache window 16760832 bytes (0x4000..0x1000000), data block 16789504 bytes physically contiguous
smmu ctx: data block (16789504 bytes contiguous) allocation failed (-1); the cache window stays unmapped and CM3 will not be released
```

保护逻辑是对的（没放复位、日志说明了原因），但这一轮白跑。根因不是"16 MB 不够"，
而是"16 MB **连续** + 开机两分钟后才要"。

## 2. 两条路

| | 0207（已合入，首选） | 0206（备用，未交付） |
|---|---|---|
| 做法 | 只映射固件真正会写的窗口：cache 仍 56 KB（`0xe000@0x4000`）+ 0x100000(0x14000) + 0x200000(0x14000) + 0x300000(0xc000) | 整窗 16 MB（`0xffc000@0x4000`）全部映射，但**不要求物理连续** |
| 数据块 | `0x51000` = 324 KB | `0x1003000` = 16.01 MB |
| 分配 | `inTaskWithPhysicalMask`，任何时刻都能拿到 | `inTaskWithOptions`（允许分散）+ 逐页 `getPhysicalSegment()` |
| 页表 | 5 + 3 条 range，约 84 条 4K leaf | 5 条 range，约 4100 条 4K leaf |
| 风险 | 依赖"固件只碰这三段"这个判断 | 不依赖判断，但把 16 MB 全映上，TLB/走表开销更大 |
| 失败时 | 日志会说明哪条 range 建不出来 | 逐页检查，任何一页拿不到就整块放弃并报页号 |

**两者互斥。** 0206 的补丁是 diff 在 0207 **之后**的树上：它把 `0xe000` 改回
`0xffc000`、删掉 `SWEET_SMMU_CTX_EXTRA_*`、`ranges[8]` 回到 `ranges[5]`。
也就是说，套上 0206 就等于**撤掉 0207**，不存在"先 0207 再 0206 叠加"的用法。

## 3. 什么时候用哪条

先看 0207 的真机结果：

1. **0207 成功**（324 KB 分配成功、CM3 出复位、FW_INIT=0x100、后续 HFI 有进展）
   → 0206 不需要。把它留在 /tmp 或删掉，别加进 `xnu/patches/`。整窗映射的唯一
   理由就是"不知道还缺哪一页"，这个理由被 0207 否掉了。
2. **0207 的 324 KB 分配成功，但 CM3 仍然复位**（典型表现：放复位后整机复位、
   盘上日志停在 `console ring flushed before releasing CM3` 之后；或者 FW_INIT
   超时）→ 说明三段之外还有 iova 被访问。此时 0206 才是对的解法：与其一轮一轮
   猜"还缺哪一段"，不如把整个 16 MB 映上；而逐页散映射让 16 MB 不再需要连续。
3. **0207 连 324 KB 都分配失败**（日志是 `data block (331776 bytes ...) allocation
   failed`）→ 那是另一个问题（分配 API/掩码/时机），0206 也救不了，先去查
   `inTaskWithOptions`/`inTaskWithPhysicalMask` 的返回值与掩码。

判据上再加一条：0207 的失败如果伴随 SMMU fault（`gfsr != 0` 或 `FAULT LATCHED`），
基本可以确定是"访问了没映射的 iova"，直接走第 2 条。

## 4. 真要启用 0206 时怎么做

```bash
# 已合入 0207 的树上直接套（已验证 --dry-run 通过）
git apply /tmp/t0206/0206.patch
# 或者，若要保持补丁序列可重放：把它作为 0207 之后的一条新补丁
cp /tmp/t0206/0206.patch xnu/patches/0208-k60-cache-16m-scattered.patch
```

然后照常 CI + 上机。注意它会把 0207 的 `EXTRA` 三段删掉（本来就在 16 MB 窗口里），
所以**不要**同时保留 0207 的 `EXTRA_*` 定义，否则数据块大小会算出两套。

回滚：`git checkout -- iokit/Kernel/arm/SweetAdreno.cpp`（或反向套 0206.patch），
回到 0207 的状态。

## 5. 0206 的测试在哪、怎么跑

- 补丁：`/tmp/t0206/0206.patch`
- 报告：`/tmp/t0206/test-report.txt`（47 项，全绿）
- 台架：`/tmp/t0206/run.sh`（只用 `/tmp/t0206`；仓库的 `host.cpp`/`run.sh`/`stubs`
  一个字没动）
  - 第一份仿真台用仓库桩：没有 `IOBufferMemoryDescriptor`，跑"拿不到 → 不放复位"。
  - 第二份用 `/tmp/t0206/simstubs`：`inTaskWithOptions` 返回**乱序物理页**
    （第 i 页 = base + (i×37 mod 4099)×4096），并且可以注错（整块失败 / 第 N 页
    没有物理段）。host 侧有一份**独立**的 4K 表遍历，逐页核对 leaf 是否指向
    该页自己的 pa；另有一个负向对照（故意改坏一条 leaf，必须被抓到）。
- 关键断言：`ASSERT ctxleaf: ok（逐页核对 4100 页，其中 4098 页的物理地址不挨着
  上一页）`、`ctxwalk 4100 pages over 5 ranges all translate`。

## 6. 0206 仍然不解决的问题

- 它不告诉你"固件到底会碰哪些 iova"，只是把 16 MB 全映上，赌 16 MB 够。
- 4100 条 4K leaf 对 TLB 的压力比 0207 的 84 条大得多；如果 GMU 顺序扫 cache，
  真机上可能表现为变慢，而不是立刻出错。
- 16 MB 虚拟内存本身在开机两分钟后是否一定拿得到，仍要上机确认（失败有日志、
  不放复位，不会静默）。
