# logtail-selftest — 主机验证 `sweet_log_dump()` 的尾部读取

不编内核、不碰设备、不联网。一个 cc、一次运行，4 秒左右跑完 20 个定值用例
+150 个随机流；失败退出非 0。

```sh
./run.sh                      # 全部用例 + 150 个随机流
./run.sh --list               # 列出用例
./run.sh --case=wrap_head_777 # 单个用例
./run.sh --fuzz=4000          # 更多随机流（约 100 s）
./run.sh --sabotage=dst       # 注入已知 bug，必须变红
```

## 被测的两份实现

| 函数 | 出处 | 作用 |
|---|---|---|
| `sweet_log_dump_new()` | `xnu/patches/0186-k60-gate-log.patch`（`bsd/dev/sweet_ufs.c` `@@ -915,40 +926,51 @@`） | 被验证的新算法 |
| `sweet_log_dump_old()` | `xnu/patches/0152-k60-log-dump.patch` | 被替换掉的旧算法：48 行 oracle + 副作用阳性对照 |

搬运方式：函数体逐字节照抄，只有三处 HARNESS 改动 —

1. `sweet_log_ring` 从 `static char[4 MiB]` 变成页对齐的堆分配（`mprotect()` 需要单独一段映射）；
2. `printf` 用 `#define printf cap_printf` 重定向到捕获缓冲（kernel 的 console 语义：`\n` 原样、
   `0x20..0x7e` 原样、其余丢弃，与补丁里的打印循环一致）；
3. `ufs_read_blocks(base + 1 + blk, 1)` 换成"从模拟分区镜像里拷一个 4096 字节块到 bounce"的桩。

剩余差异只有 `g_sab` 的注入钩子（不加 `--sabotage` 时 `g_sab == SAB_NONE`，全部钩子都不命中）。
把钩子去掉后与补丁 hunk 的 `diff` 为空，这条是脚本比对过的，不是口头保证。

## 分区镜像模型

镜像里 `base` 块是 `sweet_log_header`，其后 4 MiB 就是环。环偏移 `r` 上的字节 =
绝对流序号 `k = total - 1 - ((total - 1 - r) % SIZE)` 的字节 —— 与
`sweet_log_flush()` 的 `off = pos % SWEET_LOG_SIZE`（`0152`/`0047`）一致。
`total < SIZE` 时 `[total, SIZE)` 填 `'#'` 当陈旧数据，读到这里就会被"必须是真尾部"的断言抓住。

每次 dump 前环里先放"本次启动已写的 `[0, written)`（`'Z'` 花纹）+ 上次启动留下的其余部分"，
这样既能测副作用，也保证旧算法读到的是它自己 memcpy 回来的上一轮内容。

## 每个用例断言什么

1. **副作用**：dump 之后整个 4 MiB 环逐字节不变；另外 `fork()` 一个子进程把环
   `mprotect(PROT_READ)` 后再跑一遍，任何写入都会 SIGSEGV（`sabotage=write` 的
   `child rc=-11, signal 11` 就是这个探针）。
2. **正确性**：打印出的 payload 必须等于"真字节流最后 `have` 字节"经同一 console 过滤器后的结果，
   失败会报第一个不一致的字节下标；另加独立的结构断言：打印区间的起点在环偏移空间里必须是
   4096 的整数倍（被 16 KiB 上限截断时除外）。
3. **行数**：未触及上限时，在可达范围内有 ≥48 个换行时必须给到 ≥48 行。
4. **旧算法对照**：旧算法输出必须等于"从真字节流里正向算出的最后 48 行"（oracle 自检），
   且新 ->旧 区间必须是后缀包含关系；同时旧算法必须真的改写了 `[0, written)`（证明测试有区分力）。

## 四个边界的实测结论

| 用例 | `h.total` | 实际行为 |
|---|---|---|
| `exact_block_4096` | 4096（4096 的整数倍） | 取 `have=4096`，一次块读把整个窗口拿完，不越界、不多读一块 |
| `small_3000` | 3000（< 4096） | `have=3000`，`first=0`，一次读 block 0 全部覆盖 |
| `just_over_block` | 5000（跨 2 块） | 先按 `boff+1` 截 904 字节（源偏移 `boff+1-n = 0`），再从 block 0 补 4096 → `have=5000` |
| `no_eol_tail` | 1009，末行无换行 | 末行残片原样打印（旧实现同样），结尾不补换行；`"\nSWEET LOG: end"` 的那一个 `\n` 只是分隔符 |
| `bigline_at_end` | 45000，尾部 40000 字节单行 | **16 KiB 上限生效**：`have=16384`，打印 16384 字节的同一行残片，**0 个换行**；旧实现给 44701 字节/48 行 |

### 单行 > 16 KiB：不是"截断"，是"整段日志消失"

`bigline_at_end` 里尾部是一行 40000 字节的 hexdump：新实现拿到的是这行的最后 16384 字节，
**一个换行都没有**，前面 50 行真实日志（旧实现能给出的 44701 字节 / 48 行）全部看不到；
而且打印头 `SWEET LOG: previous boot, last 16384 bytes` 没有任何"被截断"的提示。
`bigline_middle`（20 KiB 长行 + 后面 3 行）同样只剩 3 行。
如果关心"看不到 panic 行"，这是最值得改的一处：`have == cap` 时补一行显式标记，
或者先跳到下一个换行再开始收集。

## 与旧实现的两处行为差异（不是 bug，但要知道）

1. **48 行是下界，不是上限。** 行数只在整块读完之后才检查，所以实际行数 = 48 + 最后一块里的换行数。
   `newline_dense`（16 字节一行）实测打印 **232 行 / 3712 字节**，旧实现是 48 行 / 753 字节。
   最坏情况（整块都是换行）能到 48+4096 行。副作用：dump 的 console 时间与回卷量变大。
2. **打印区间起点落在 4096 块边界，不是行边界。** 所以 dump 的第一行通常是半行
   （`bigline_*` / `longlines_20000` / `typical_10000` 都是如此，最后那个 `(starts at a line
   boundary)` 标记就是用来区分这件事的）。旧实现从那个换行本身开始打，永远整行开头。

## 关于 `boff + 1 - n` 与 `pos`/`first` 回绕：没发现问题

证明（也是随机流在测的东西）：

* `n ≤ boff + 1` ⇒ 源区间 `[boff+1-n, boff]` 永远在块内，`blk` 不会和下一块错位；
* `n ≤ cap - have` ⇒ 目的区间 `[cap-have-n, cap-have)` 永远在 `tail` 内，且
  `tail[cap-have_new + i]` 的统计范围正好是刚写入的那段；
* `pos -= n` 不会下溢：`pos = want - have`；若 `want ≥ cap` 则 `n ≤ cap-have ≤ pos`；
  若 `want < cap`，则必有 `total = want < SIZE` ⇒ `first = 0` ⇒ `n ≤ boff+1 = ((pos-1) % 4096)+1 ≤ pos`。
  也就是说 `want` 一旦小于 16 KiB 就一定是"整段读完"，走不到需要跨过 `first` 的那一步。
* 回绕本身（`first > 0`、走到环偏移 0 再跳到 `SIZE-1`）由 `wrap_head_1byte` /
  `wrap_head_blk_1b` / `wrap_head_777_3x` 专门覆盖 —— 这三个用例的 `h.total % SIZE` 是 1/4097/777，
  只有它们才会让 `idx` 真正跨过 0（其他 wrap 用例的 `first` 远大于 16 KiB，一辈子走不到 0）。

## 注入的 bug（`--sabotage=`）

| 名字 | 注入的错误 | 被抓到的方式 |
|---|---|---|
| `dst` | 目的偏移 +1（少写的回绕算术类错误） | `payload is not the true tail: first mismatch at byte 4096` |
| `boff` | 截断用 `boff` 而不是 `boff + 1` | 字节数少 1 + `starts at ring offset 0x3001, not a 4096 boundary` |
| `clamp` | 不截断，改成从块首读（旧实现的写法） | 读到窗口外，`printed 16384 bytes but only 3000 exist` |
| `wrap` | 去掉 `idx` 的 `% SWEET_LOG_SIZE` | `read_err=1` / 长度不符 |
| `write` | 恢复 `memcpy` 回环 | 环被改写 + 写保护探针 `signal 11` |
| `lines` | 忽略 48 行上限 | `expected last 5904 bytes, got 10000` |

`--fuzz=N` 的随机流：长度覆盖 `1..3*SIZE`，其中一半强制落在
`laps*SIZE + {0,1,2,31,100,4095,4096,4097,8191,8192,16383,16384,16385}` 上，
换行密度 1%..60%，并随机插入 1..24000 字节的无换行段（模拟 hexdump）。
实测 `--fuzz=4000` 全绿。
