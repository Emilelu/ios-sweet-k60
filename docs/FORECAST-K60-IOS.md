# K60 完整 iOS 推演与计划（2026-10-24 晚）

## 当前位置（有证据）

```
内核启动 ✅ → 显示 console ✅ → UFS 473GB ✅ → HFS+ iOS 根 ✅
→ 8 核 ✅ → launchd(pid 1) ✅ → tzinit/systatuscheck/prng_seedctl 执行 ✅
→ launchd_cache_loader: "Unable to connect to AMFI" 🔄（用户客户端已修，待验）
```

## 阶段推演（每一阶段：预期行为 / 最可能卡点 / 估计）

### 阶段 A：AMFI 连接（当前）
| 项 | 内容 |
|---|---|
| 预期 | `SWEET AMFI: user client selector N accepted` 后不再 abort |
| 最可能卡点 | libamfi 可能要求特定 scalar 输出（我们的实现全返回 0）|
| 若失败 | 看 selector 号和返回要求，逐个实现 |
| 估计 | **1–2 轮编译**（约 30 分钟）|

### 阶段 B：dyld 共享缓存加载
| 项 | 内容 |
|---|---|
| 预期 | `launchd_cache_loader` 找到 `dyld_shared_cache_arm64e` 并映射 |
| 已准备 | 缓存已硬链接到 `/System/Library/Caches/com.apple.dyld/` ✅ |
| 最可能卡点 | ① 架构不匹配（arm64e vs arm64）② cache 缺项 ③ trust cache 校验 |
| 若失败 | 换对应架构的 cache；补 trust cache（有 `0015` 相关补丁）|
| 估计 | **半天到 1 天** |

### 阶段 C：核心守护进程
launchd 会拉起约 200 个作业，按依赖顺序最可能先跑：

| 守护进程 | 依赖 | 最可能问题 |
|---|---|---|
| `configd` | IOKit 网络、SC 框架 | 网络接口缺失（无 WiFi/蜂窝）|
| `securityd` / `keybagd` | AppleKeyStore | 有桩（`AppleKeyStore registered` ✓）|
| `powerd` | IOPMrootDomain、PMU | PMIC（SPMI v7 不支持）|
| `notifyd` / `distnoted` | Mach IPC | 应该直接过 |
| `watchdogd` | — | 可能因某进程慢而杀进程 |
| `mediaserverd` | 音频 | 已调音禁用 VirtualAudio ✓ |

**关键洞察**：这些进程大部分**不需要硬件**就能活，所以这一阶段主要看"哪个先崩"。
| 估计 | **2–5 天**（逐个排查）|

### 阶段 D：backboardd（显示主进程）— **真正的门槛**
| 项 | 内容 |
|---|---|
| 依赖 | IOMobileFramebuffer（✅ 已实现全套 selector）、IOSurface（✅）、vsync 定时器（✅）|
| 我们缺 | ① 从未有用户态客户端连过 ② `IOConnectSetNotificationPort` 的正确性未验证 ③ 面板电源/背光控制 |
| 若失败 | 看日志里 `IOMobileFramebuffer` 的 selector 调用号，逐个修 |
| 估计 | **3–7 天** |

### 阶段 E：SpringBoard（界面）
| 项 | 内容 |
|---|---|
| 依赖 | backboardd + CoreAnimation（CPU 渲染）+ 一堆框架 |
| 最可能卡点 | ① 缺 Metal/GPU（可走 CPU 路径）② 缺硬件服务（相机/蓝牙）③ SpringBoard 自身校验 |
| 已准备 | `BKNoWatchdogs`、跳过 Setup、关 CommCenter |
| 估计 | **3–7 天** |

## 总估计（乐观→保守）

| 里程碑 | 乐观 | 保守 |
|---|---|---|
| AMFI + dyld 过 | 今天 | 1 天 |
| 核心守护进程稳定 | 2 天 | 5 天 |
| backboardd 出画面 | 5 天 | 10 天 |
| SpringBoard 主屏 | 8 天 | 14 天 |

**前提：没有"根本性"阻塞**（比如 dyld 缓存与内核 ABI 不兼容）。

## 已知的"根本性风险"（可能推翻估计）

| 风险 | 影响 | 概率 |
|---|---|---|
| dyld 缓存与 XNU 8796 ABI 不匹配 | 需要重建缓存（工具复杂）| 中 |
| iOS 16.7 的 userspace 需要 Apple Silicon 特性 | 无法启动 | 低 |
| backboardd 强依赖 GPU（不走 CPU 路径）| 必须先做 Adreno 驱动（周级）| 中 |
| watchdog 频繁杀进程 | 需要逐个调参 | 高（但有 `BKNoWatchdogs`）|

## 下一步（按顺序，每步可验证）

1. **CI 出内核 → 测 AMFI 连接**（当前）
2. 若过 → 看 dyld 加载日志
3. 若卡 → 按日志里的具体路径/错误补
4. 一旦 backboardd 起来 → 屏幕会变（`sweetfb_noconsole=1` 时 console 停止绘制）

## 今天已经证明的事（降低整体风险）

- UFS 驱动可用（473 GB 正确读取）
- HFS+ 挂载可用（11.5 GB 完整 iOS 根）
- 内核 exec 可用（launchd 真的跑起来了）
- 8 核可用（不是单核瓶颈）
- **显示交接代码完整**（IOMobileFramebuffer 全套 selector + SwapEnd 拷贝）
- 签名链可绕过（三个参数 + MAC 策略）

**这六条把"根本性阻塞"的概率压得很低。**

---

# 2026-10-24 晚 更新：核心守护进程已运行

## 已越过的阶段（屏幕证据）

```
IOK backboardd: open SweetHIDTouch type 1702865587 -> 0x0
SWEET HID: SweetHIDTouch open by IOHIDEventServiceUserClient -> 1
IOK backboardd: open SweetHIDButtons type 1702865587 -> 0x0
SWEET AKS: powerd sel 0 / sel 7 ...
SWEET Sandbox: containermanager op 16 / op 15
```

| 进程 | 状态 |
|---|---|
| launchd (pid 1) | ✅ |
| **backboardd** | ✅ 打开了我们的 SweetHIDTouch / SweetHIDButtons |
| powerd | ✅ 用 AppleKeyStore |
| containermanager | ✅ 沙盒操作 |
| dyld 系进程 | ❌ SIGKILL（far=0x1 空指针）|

**阶段 C（核心守护进程）基本完成。**

## 当前卡点

```
CRASH dirwkrtld(69) sig 9 far=0x1        ← 空指针解引用
panic: po_op_assert_free: Found at least one mapping to 0x8000000,
       First PTEP is a Kernel CPU mapping (pmap.c:2809)
```

两个可能方向：
1. `dirwkrtld` 的空指针是根因（缺少某个 sysctl / 缓存结构）
2. pmap 断言是根因（某个映射泄漏 → 内存损坏 → 各进程随机崩）

## 已确认的配置

- 内核 16 KiB 页（`pexpert/pexpert/arm64/SWEET.h: __ARM_16K_PG__ 1`）✓ 与 dyld 缓存匹配
- dyld 缓存硬链接在 `/System/Library/Caches/com.apple.dyld/` ✓
- AMFI 服务 + 用户客户端已注册（`user client ready`）✓
- 签名绕过（三参数 + MAC 策略）✓
- 可写根（`ufsrw=1`）✓
- 8 核 ✓

## 下一步建议

1. **查 `dirwkrtld` 是什么进程**（`strings` iOS 根里的 dyld 相关二进制）
2. **查 pmap 断言**：`0x8000000` 物理页被释放但仍有映射 → 谁泄漏了映射
3. 若 pmap 是根因，检查我们 UFS 驱动的 DMA 映射和 framebuffer 映射的清理

---

# 2026-10-24 深夜：HFSX 修复 + pmap 断言定位

## 重大修复：HFS+ 必须是大小写敏感（HFSX）

**问题**：镜像用 `newfs_hfs -v iOSRoot` 创建 → 签名 `H+` 版本 4 = **大小写不敏感** ✗
**iOS 要求**：签名 `HX` 版本 5 = 大小写敏感 + 日志式

**症状**（修复前）：
```
hfs: BlockAllocFreeInternal() trying to free unallocated blocks on volume iOSRoot
hfs: Runtime corruption detected on iOSRoot, fsck will be forced on next mount
CRASH containermanager(99) sig 5
```

**修复后**（`HX` 卷）：**两条错误完全消失**，`containermanager`、`powerd`、`backboardd` 全部正常 ✓

**教训**：sweet 原脚本用 `mkfs.hfsplus -s -J`，移植时把 `-s -J` 丢了。

**转换工具**：`tools/k60-convert-hfsx.sh`（挂载旧镜像 → 建 HFSX → rsync -aHAX，不需要重下 7 GB dmg）

## 当前卡点：pmap 断言（内存损坏）

```
panic: pmap_assert_free: Found at least one mapping to 0x80000000
       First PTEP is a Kernel CPU mapping
       caller 0xfffffe0007437884 = _vm_page_free_prepare_object+0xb8
```

**已确认**：
- `topOfKernelData = 0x824d4000`（正确，在内核结束 0x824cc000 之上）
- `avail_start = topOfKernelData`（XNU 源码 `arm_vm_init.c:1982`）
- 所以 `0x80000000`（内核第一页）**不该**被 VM 管理
- 但它被 `vm_page_free_prepare_object` 释放 ✗

**结论：有内存损坏** —— 某个 `vm_page_t` 的物理页号被写成了 0x8000。

**嫌疑**：
1. UFS 驱动的 DMA（PRDT 越界）
2. HID 服务（backboardd 打开后）
3. framebuffer band 映射
4. `driverkitd` 崩溃后的清理路径

**注意**：`driverkitd` 每次都崩（`ns 1 code 0x7`），且崩溃后紧接着就是 pmap 断言 → **时间相关性很强**

## 下一步候选

| 方向 | 手段 | 风险 |
|---|---|---|
| ① 去掉 `driverkitd` 作业 | 改 launchd.plist | 低 |
| ② 关闭 `ASSERT_PMAP_FREE` | 内核补丁（治标）| 中（可能掩盖真问题，但能让 iOS 继续）|
| ③ 定位内存损坏源 | 加 guard page / 用 zone 检查 | 高工作量 |

**建议先 ① + ②**：让 iOS 继续启动，看它能不能到 SpringBoard；之后再回头定位损坏源。

---

# 2026-10-24 收尾：内存损坏的定位进展

## 已确认的崩溃形态

| 版本 | 断言 | 说明 |
|---|---|---|
| 8 核 | `pmap_assert_free: mapping to 0x80000000` | 内核第一页被当可释放页 |
| 8 核（禁用 pmap 断言）| `[vm objects]: element modified after free` | **use-after-free** |
| 8 核 | `_sched_clutch_thread_remove+0x48` 数据中止 | 调度器结构被破坏 |
| **1 核** | `kern_event.c:4100 kn->kn_status & KN_QUEUED` + `vm_page.h:519 p < vm_pages_count` | **单核也有** → 不是 SMP ✗ |

**结论：内核内存损坏，与 CPU 数量无关。**

## 已排除的嫌疑（逐个验证过）

| 嫌疑 | 验证方式 | 结果 |
|---|---|---|
| HFS 大小写不敏感 | 转 HFSX 后重建 | ✅ **是真 bug，已修** |
| HFS 分配位图 | 直接解析位图对比 | 差 0，一致 ✓ |
| UFS 缓存维护时机 | 读代码 | 正确 ✓ |
| 512→4K 块转换 | 读代码 | 正确 ✓ |
| PRDT 表溢出 | 算容量 | 4 项正好 ✓ |
| `topOfKernelData` | 从 payload 头计算 | 0x824d4000 正确 ✓ |
| HID 定时器风暴 | 读代码（需 boot-arg）| 无定时器 ✓ |
| 日志环线程 | `sweetlog=0` | 仍崩 ✗ |
| 按键/触摸驱动 | `sweetkeys=0 sweettouch=0` | 仍崩 ✗ |
| SMP/clutch | `cpus=1` | **仍崩** ✗ |
| driverkitd | 换守护进程后崩在 fairnoded | 不是它 ✗ |

## 剩下最强的嫌疑

1. **帧缓冲的多重映射属性冲突** — fbcon 用 Normal-NC，`AppleCLCD` 用 WB，同一物理页不同属性在 ARM 上是规范违规 ✗
   证据：屏幕上下边缘的彩色条纹（FB 内存被写坏）
2. **UFS DMA 越界** — 虽然 PRDT 正确，但设备可能写超出 bounce buffer
3. **XNU 在这个平台上的 VM 初始化问题** — mondrian 也到 backboardd 就停，可能有同样问题

## 下一步（下一轮）

1. 试 `sweetfb_noconsole=1` + 完全不用 GUI 路径，看损坏是否消失
2. 试把 UFS bounce buffer 从 1 MiB 降到 64 KiB
3. 对照 mondrian 的 kernel 补丁（他们有 94 个，可能有 VM 相关修复）
4. 用 `zprint`/`vm_page` 调试工具定位被写坏的指针

## 今天达成的里程碑（全部有屏幕证据）

```
内核启动 → 显示 → UFS 473GB → HFS+ iOS根(HFSX) → 8核
→ launchd(pid 1) → AMFI 服务 → backboardd/powerd/containermanager/fairnoded 运行
→ backboardd 打开 SweetHIDTouch / SweetHIDButtons
```

**距 SpringBoard 只剩"内存损坏"这一个障碍。**

---

# 2026-10-24 深夜 2：内存布局修复 + 完整日志到手

## 🎯 根因确认：内核压在固件保留区上

**真实设备树有 38 个 reserved-memory 区域，合计 603 MiB，全在低内存区** ✗
我们的 ADT **一个都没声明** ✗，而内核加载在 `0x80000000`：

```
0x80000000  hyp_region (Hypervisor)  6 MiB   ← 内核正好压在这里 ✗
0x80900000  smem_region              2 MiB
0x80c00000  cdsp_secure_heap        70 MiB
0x8bc00000  mpss_region (modem)    306 MiB
...
```

**修复**：把内核移到固件保留区之间的空隙 `0xb4000000`（703 MiB，包含帧缓冲）

**结果：所有 pmap/vm_page/use-after-free 断言完全消失** ✓✓✓

## 📋 完整日志拉取流程打通（不用再拍照）

```
fastboot boot 测试镜像 → 崩溃
    ↓ fastboot reboot recovery（TWRP，有 root adb）
    ↓ dd if=/dev/block/by-name/userdata bs=1M skip=<末尾-5>
    ↓ 解析 "SWEETLOG" 头 + 环内容
```

日志环格式（我的驱动定的）：
```
块 0: "SWEETLOG" | u32 版本 | u32 环大小 | u64 总字节 | u64 序号
块 1+: 环内容，偏移 = n % 环大小
```
**拉取脚本**：`tools/sweet-log-pull.sh`（待写）

## ✅ boot 分区已备份

`~/hackios/boot-original-boot_a.img`（192 MB）—— **刷 boot 现在可逆** ✓

## iOS 实际启动到的位置（518 行完整日志）

| 阶段 | 状态 |
|---|---|
| 内核完整启动 | ✅ |
| AMFI 用户客户端 | ✅ `user client selector 6 accepted` |
| dyld 缓存送达 | ✅ `Cache sent to launchd successfully` |
| launchd 早期启动 | ✅ `Early boot complete` |
| fsck / mount-phase-1/2 | ✅ |
| **fips（corecrypto）** | ✅ **全部测试 PASSED** |
| keybag | ✅ `System Keybag loaded` |
| **usermanagerd** | ✅ 用户会话 + 5 persona |
| launchd_cache_loader | ✅ **通过了** |
| **backboardd** | ✅ 打开 SweetHIDTouch/Buttons + AppleKeyStore |
| powerd / containermanagerd / cfprefsd | ✅ |
| **launchd** | ❌ 段错误退出 |

## ❌ 当前唯一阻塞：dyld 共享区域为空

```c
/* osfmk/vm/vm_shared_region.c:1020 */
if (sr_first_mapping == (mach_vm_offset_t) -1) {
    kr = KERN_INVALID_ADDRESS;      ← 这里失败
}
```

**`sr_first_mapping == -1` = 共享区域没有任何映射** ✗

日志证据：`shared_region: [1(launchd)] check_np(...) vm_shared_region_start_address() failed`

**后果**：所有进程都**无法共享 dyld 缓存** → 各自独立加载 dylib → 部分进程（launchd、driverkitd）崩溃 ✗

**下一步**：
1. 在 `_shared_region_map_and_slide_np` 加日志，看 populate 为什么失败
2. 检查共享区域的 VA 空间是否足够（需要大块连续 VA）
3. 或绕过共享缓存，让进程独立加载（已在发生，只需修 launchd 的崩溃点）

## 其他已知缺失（mondrian 也遇到）

```
libignition: failed to lookup preboot: 25           ← 没有 preboot 设备/卷
ERROR: BindFS lookup of /private/preboot/<hash>/usr/standalone/firmware failed
mount: data volume missing, but not required        ← 没有独立 Data 卷
```
**不阻塞 launchd** ✓（mondrian 的判断正确）

---

# 2026-10-24 收尾 2：共享区域 handle 为 NULL

## 决定性数据（屏幕固定行）

```
SR base   0x0000000100000000   0xffffffe1334fe200   0x0000000000000000
          base 正确（4 GB）      size 垃圾 ✗          handle = NULL ✗
```

**`sr_handle = NULL`** → `vm_map_enter_mem_object()` 拿 NULL 必然失败
→ `sr_first_mapping` 保持 -1 → 共享区域为空
→ `vm_shared_region_start_address()` 返回 KERN_INVALID_ADDRESS
→ 所有进程无法共享 dyld 缓存 → launchd 段错误 → 系统 panic

## 已修的日志通道 bug（0156）

```c
// 之前：第一次写失败就永久退出
if (error) { printf("write failed"); return; }

// 现在：下个周期重试
if (error) { printf("write failed, retrying"); continue; }
```

## 遗留

1. `sr_handle = NULL` —— 需要查 `shared_region_make_shared_region` 的调用参数
2. 日志环仍未写入（即使 0156 后）—— 需要单独排查
3. driverkitd 崩溃
4. preboot/Data 卷缺失（不阻塞 launchd）

## 基建（长期受益）

- `tools/k60-log-pull.sh` —— TWRP 一条命令拉完整日志
- `tools/sym.py` —— 符号化任意 panic 地址
- `boot-original-boot_a.img` —— boot 分区备份（192 MB），刷机可逆
- `tools/k60-convert-hfsx.sh`、`tools/mk-simg.py`
- `ADT_PHYS_BASE` / `ADT_MEM_SIZE` / `ADT_SEED` 环境变量

---

# 2026-10-25 ：关键结论修正

## launchd 死于 SIGSEGV（已确认）

`bsd/sys/reason.h`:
```c
#define OS_REASON_JETSAM  1
#define OS_REASON_SIGNAL  2     ← namespace 2
```
日志里 `pid 1 exited -- exit reason namespace 2 subcode 0xb` = **SIGNAL 11 = SIGSEGV**

**排除**：不是 jetsam（内存压力）、不是正常退出、不是 OOM
**结论**：launchd 真的访问了非法地址

## 内存上限修正

之前算的"0xea000000 + 352 MB 可以加进来"是**错的** ✗：
- `0xe9f00000` 以上被 XPU 拦截（mondrian 笔记早写过）
- `ml_static_mfree()` 逐页访问 → 撞 XPU → **硬挂，无任何 panic 输出**
- 实测：`xnu-mem2.img` 启动后设备完全无响应 ✗

**可用的额外内存只有 `0xe0b00000 + 74.9 MB`**（在 XPU 边界以下 ✓）
→ 总内存从 703 MB 提到 **778 MB（+10.7%）**

**真正的内存杠杆是 dyld 共享缓存** ✓：
- 现在每个进程各自加载 dylib → 同一份库被映射 N 次
- 修好共享缓存 → 所有进程共享一份 → 省下数百 MB

## 共享区域诊断的两个值（实测）

```
SR base   0x100000000   0x0   0x0
          ↑ 这是 SHARED_REGION_SIZE_ARM64 的值，不是 BASE（0x180000000）✗
          ↑ size = 0 ✗  handle = NULL ✗
```

**说明读到的对象字段是错乱的** ✗ → `0164`（setup 入口 trace）会确认拿到的是什么。

## 当前 CI

| Run | 内容 |
|---|---|
| `40153567637` | `0165`（74.9 MB opt-in 内存交还）|
| 已完成 | `0162` 日志通道（tsleep→delay + panic 刷盘）✓ |

## 下一步（设备开机后）

```
① xnu-nophys.img  → 验证日志通道：看 "SWEET LOG: flushed" 和 "CRASH launchd ..."
② 拿到 launchd 的 pc/lr/far → 定位段错误地址
③ 根据地址判断：是否真的是共享缓存缺失导致
```

---

# 2026-10-25 ：推翻"共享缓存坏了"的假设

## 实测数据（日志固定行 + 滚动日志）

```
SWEET SR: base=0x180000000 size=0x100000000 handle=0xfffffe167f9ed620 nesting=0x180000000+0x100000000
```

| 字段 | 实测值 | 标准值 | 结论 |
|---|---|---|---|
| base | 0x180000000 | `SHARED_REGION_BASE_ARM64` | ✅ 正确 |
| size | 0x100000000 | `SHARED_REGION_SIZE_ARM64` | ✅ 正确（4 GB）|
| **handle** | **0xfffffe167f9ed620** | 非 NULL | ✅ **正常** |
| nesting | 0x180000000+0x100000000 | — | ✅ 正确 |

**共享区域完全正常。** 之前读到的 `size=0 handle=0` 是在错误的时机（区域尚未初始化）读的假象。

## `check_np` 失败是正常的

```
dyld 启动 → 调 check_np 看有没有已挂载的共享缓存
          → 此时还没装（sr_first_mapping == -1）
          → 返回失败 ✅ 这是标准启动流程
          → dyld 随后自己 map_and_slide 安装
```

**所以"dyld 共享缓存装不上"从头到尾都不是问题。**

## 影响

- 目标 ② 的前提需要重写：launchd 的 SIGSEGV **不是**共享缓存导致的
- 进程能正常运行（backboardd/powerd/containermanagerd 都活着）也印证了这一点
- dyld 会回退到 `/usr/lib/*.dylib` 逐个加载，功能上没问题

## 真正需要修的：日志线程只写一次（0167）

```
现象：日志环只有第一次写盘的内容（seq=1），永远抓不到崩溃前最后 90 秒
原因：lck_mtx_lock(&sweet_ufs_lock) 被 HFS 读盘长期占用
      → 日志线程阻塞 → 再也没写第二次
修法：lck_mtx_try_lock，拿不到就跳过这个周期
```

## 下一步

拿到 `CRASH launchd pc/lr/far` 后，用 `tools/sym.py` 符号化，定位真正的段错误点。

---

# 2026-10-25 上午：日志通道打通，拿到完整崩溃集

## 结论一：0167 有效，日志环能写多段

`tools/k60-log-pull.sh` 拉到 **56115 字节 / 705 行**，包含 `SWEET LOG: ufs busy,
retry next interval (total 39714)` 之后的内容 —— 说明 0167 的 try_lock 生效，
刷盘确实尝试了多轮。

`seq` 恒为 1 是**假象**：`sweet_log_flush_now()`（0162）里 header 的 `seq`
写死为 1，panic 刷盘一定会把 seq 覆盖成 1。以后判断"是否写了多段"看 `total`
和正文内容，不要看 `seq`。

## 结论二：那一轮的崩溃集全是 ADT 用错导致的

用 `artifacts/k60.adt`（旧布局，DRAM 从 0x80000000 开始，958 MB）启动，
`vm_page_bootstrap` 报 **55885 个空闲页**（= 873 MB，而物理可用只有 703 MB），
把 hypervisor / modem / cdsp 全当成了 RAM。结果：

```
CRASH lsd[97]  sig 11 far 0xff000020 (cache pc 0x1aae63478)
CRASH lsd[102] sig 11 far 0xffffffffffffffff
CRASH driverkitd sig 9
CRASH mediaserverd sig 5 far 0x20832b4d0
CRASH IOMFB_FDR_Loader sig 4
CRASH DumpPanic sig 6 "pthread_t was corrupted"
```

预警信号：**空闲页数换算成 MB 超过 703 就是 ADT 错了**，不用等崩溃。

## 结论三：换成正确 ADT 后，卡点变成 pmap 压缩 PTE 校验

正确 ADT（0xb4000000 + 703 MB）下，启动到 launchd 之后挂在：

```
panic(cpu 0): compressed PTE 0x... 0xffff80000f000000 has extra bits 0x...: corrupted?
             @osfmk/arm/pmap/pmap.c:519 (ARM_PTE_IS_COMPRESSED)
Panicked task: pid 47 DumpPanic
```

## 新假设（待验证）：帧缓冲物理页被当成普通 RAM 分给了 VM

- 帧缓冲在 `0xb8000000`，18.4 MB，`pmap_next_page()` 会把它交给 VM
- 控制台 `pexpert/arm/pe_serial.c` 每打一个字**直接写这块物理页**
- → 任何落在该区间的用户页或页表页都被控制台文字覆盖
- 正好解释：花屏（显示内容被别人的数据占用）、PTE/pthread 随机损坏、
  各进程在无关位置段错误

补丁：`0170`（`pmap_next_page` 跳过 0xb8000000-0xb9200000）
       `0171`（刷盘前最多等 400 ms 拿 UFS 锁，别再出现"环里是上一轮"）
       `0169`（在 handle_user_abort 里打印 ESR/FAR/ELR + 覆盖该地址的 vm_map entry，
               并在崩溃当下立刻刷盘 —— 之前 launchd 的崩溃地址根本进不了环）

## 判断下一步的判据

| 现象 | 结论 |
|---|---|
| 日志里出现 `SWEET MEM: panel buffer ... left out of the VM` | 排除生效 |
| 空闲页比上一轮少 1152 页 | 同上 |
| 不再出现 `compressed PTE ... corrupted?` | 假设成立 |
| 出现 `ABORT <名字>[pid] esr ...` | 真正的首错，按 ESR 的 FSC 判断：<br>0b0001xx = 翻译失败（没映射）<br>0b0011xx = 权限失败（映射了但不可访问）|

---

# 2026-10-25 中午：找到"无声猝死"的根因（内存窗口给大了）

## 根因：XNU 拿 physmap 跨度当可管理 RAM

`shim/loader.c` 里（无条件执行）：

```c
uint64_t phys_span = (p->phys_base < 0x100000000ULL) ?
    (0x100000000ULL - p->phys_base) : p->mem_size;
ba->memSize = phys_span > p->mem_size ? phys_span : p->mem_size;
ba->memSizeActual = p->mem_size;
```

- `memSize` 是**物理映射跨度**，为了让 `phystokv()` 能碰到 4 GiB 以内的一切
- `memSizeActual` 才是 ADT 里真实的 DRAM 大小（703 MB）
- `osfmk/arm64/arm_vm_init.c` 却直接 `avail_end = gPhysBase + mem_size`

→ 结果：VM 从 `0xdf000000` 一路管到 `0x100000000`（1216 MB），
  多出来的 513 MB 里有 346 MB 是 XPU 拒绝解码的（≥0xe9f00000）。
  分配器从低往高发页，用光 703 MB 之后就踩上去 → **同步外部中止，
  没有 panic、没有日志、连串口都没输出**，环日志最后一行停死。

**症状对上**：`vm_page_bootstrap: 72576` 之类超标的页数、
跑到 backboardd/45 秒后无声猝死、偶发花屏。

**修法**：`0172-k60-cap-managed-ram.patch` —— 在 arm_vm_init 里把
`mem_size` 收敛到 `memSizeActual`（只影响 VM 管理的范围，
physmap 仍然覆盖 4 GiB）。修完日志能写到 108 KB、41 次刷盘，
homed/securityd/maild/backboardd 全部起来。

## 帧缓冲：页要排除，但不能挖洞

`0xb8000000-0xb9200000`（1440×3200×4）在内核管理窗口内，
而控制台每打一个字都直接写这块物理页。

- ✗ 在 `pmap_next_page()` 里跳过 → 触发 `vm_page_init` 断言
  `phys_page == VM_PAGE_GET_PHYS_PAGE(m)`，因为 arm64 的 vm_page
  是**用数组下标反推物理页号**的，中间不能有洞
- ✓ 正确做法：页照常建，只是**不放进空闲队列**
  （`0170-k60-panel-buffer-out-of-vm.patch`，改 pmap_startup 的两个 release 点）

## 日志通道现状（可靠了）

| 补丁 | 作用 |
|---|---|
| `0167` | try_lock，不再被 HFS 卡死 |
| `0171` | 拿锁最多等 400 ms，panic 刷盘也等；`ufslogsecs=1` 时最多丢 1 秒 |
| `0162` | panic 时同步刷盘 |

**别再看 `seq`**：`sweet_log_flush_now()` 里 header 的 seq 写死 1，
判据是 `total` 和正文内容。

## 新工具

```
tools/k60-build-boot.sh <kernel> [out.img]   # 每次现生成 ADT + 校验 + 打包，杜绝旧 ADT
tools/k60-log-summary.py [log]              # 摘要：页数/首错 ESR/panic/尾行
tools/k60-log-pull.sh                        # TWRP 拉环
```

## 当前卡点：触摸驱动初始化

修完内存后跑满 45 秒，最后一行：

```
SWEET TOUCH: mapping GCC/TLMM/SE7
```

`bsd/dev/sweet_touch.c` 在 `sweettouchdelay`（默认 45 秒）后开始：
`ml_io_map(0x100000 GCC, 0x3900000 north TLMM, 0xa84000 SE7)` 然后
`touch_clocks()` 撞上无声猝死 —— 大概率是某个 MMIO 地址不该碰。

**下一步**：先用 `sweettouchdelay=600` 把触摸推迟到观察范围之外，
确认后面还能走多远；再逐块核对 GCC/TLMM/SE7 的地址。

## 里程碑：launchd 不再段错误

0172 生效后的一轮（`xnu-p4.img`，`ufslogsecs=1`）：

```
ring: total 108596, seq 41      ← 41 次刷盘，日志几乎完整
homed / biomed / cloudd / healthd / identityservicesd / imagent /
geod / coreduetd / adprivacyd / carkitd / dmd / coreduetd ... 全部起来
```

- **没有** `pid 1 exited`，**没有** `CRASH launchd`
- 唯一的 CRASH 是 driverkitd（sig 9，被 kill，无 DriverKit 提供者）
- 系统稳定跑了约 45 秒，最后一行 `SWEET TOUCH: mapping GCC/TLMM/SE7`

**即：原交接文档里"唯一阻塞 = launchd SIGSEGV"已经解决，
根因不是 launchd 逻辑，也不是共享缓存，是 VM 管到了不能碰的内存。**

## 触摸驱动：先把 Register 探针打开

`sweet_touch.c` 在 `sweettouchdelay`（默认 45 s）后开始，第一条访问就是
`GCC+0x5200c`（vote）。猝死无 panic，符合 XPU 拒访的特征。

- `0173` 在每个 GCC 访问前打印一行（最后一行即真凶），
  并新增 `sweettouchoff=1` 彻底关掉触摸线程
- 另注：从 `dump/live.dts` 看，K60 的触摸在 **spi@994000（qupv3_se5）**，
  GPIO 是 **20 reset / 21 irq / 71 avdd**，TLMM 在 **0xf000000**；
  驱动里写的 `0x3900000` + GPIO 6/7/8/9/90 + `0xa84000`（DT 里那是 SE9/i3c1）
  对不上，等探针结果再定

## 里程碑 2：SpringBoard 起来了，但画不出东西

`xnu-p6-notouch.img`（0172+0173，`sweettouchoff=1`）跑满 6 分钟：

```
SWEET AKS: SpringBoard sel 0 / 35 / 7 / 35 ...
SMP SpringBoard[80] pc ... backboardd[50] ... launchd[1] ...
```

- SpringBoard 是**活着的**，跟 AKS 正常交互，没有崩溃
- **launchd 全程没死**，几十个守护进程稳定运行
- 唯一 CRASH 仍是 driverkitd（sig 9，被 kill）

### 显示链路：通到"内容"这一层

`xnu-p7-screen.img`（`sweetfb_noconsole=1`）：控制台被停、屏幕全黑。
`xnu-p8-fbtest.img`（`sweetfb_test=1`）：内核往 `fPixels` 写的三色带**出现在屏上**
→ 面板扫描和外设拷贝都是好的。

### 真正缺的东西：内容生产

采样探针（0175）在 `present()` 里读 layer 0 的表面：

```
SWEET FB: layer 0 surface 3 first 0x00000000 nonempty 0/4096
```

整场只合成了 **3 帧，全是零**。原因：

```
IOK backboardd: matching IOProviderClass=IOAcceleratorES -> none
|backboardd| IOServiceMatchingfailed for: AppleM2ScalerCSCDriver
```

内核里没有任何 GPU / 图形加速器，CoreAnimation 拿不到渲染设备，
SpringBoard 的图层就是空的。**这不是我们的拷贝路径问题，是 iOS 渲染栈的硬要求。**

### 下一步：走苹果自己的 ProgressUI（真代码，不是画假图）

`ProgressUI.framework` 是苹果画开机图标和进度条用的框架，纯 CoreGraphics，
不依赖 GPU。镜像里只有 Info.plist + PNG，二进制在 dyld 缓存里（decmpfs 压缩，
7z 看不见，所以符号得在设备上问）。

做法：一个用户态探针 `dlopen` 它，把类和方法名打到控制台（launchd 会把子进程的
stdout 接到控制台，所以输出会进日志环）。

- 探针源码：`probe/sweetprobe.c`（clang -target arm64-apple-ios16.0 + ld64.lld
  产出 Mach-O，`-undefined dynamic_lookup` 不做静态链接）
- 安装：`sudo bash tools/k60-install-probe.sh`（写 `/usr/libexec/sweetprobe`
  **并把它加进 `/System/Library/xpc/launchd.plist` 这个聚合库**）
- 关键坑：`/System/Library/LaunchDaemons/*.plist` 是摆设，launchd 只读聚合库
  （证据：driverkitd 的独立 plist 还在，但它已经不加载了）

## ProgressUI 的实测结论（这条路的尽头）

用户态探针跑通之后，苹果自己的 `PUIProgressWindow` 是可以实例化的：

```
sweetbootlogo: dlopen ok
sweetbootlogo: PUIProgressWindow 0x21d10f4a0
sweetbootlogo: window 0x157d0aec0
sweetbootlogo: layer 0x0            ← init 之后没有 layer
sweetbootlogo: _createLayer
sweetbootlogo: layer 0x157d120c0    ← 手动建出来了
sweetbootlogo: tick 5 value 0.45 layer 0x157d120c0
```

要点：

- `initWithProgressBarVisibility:createContext:contextLevel:appearance:` 成功
- `_collectDisplayInfo` / `_createContext` / `_createLayer` / `_layoutScreen` /
  `_updateIOSurface` 全部可调，进度值也能设，进程稳定不崩
- **但整个过程没有任何 `SWEET SURF: create`** —— 它没走我们的表面服务，
  说明它把内容交给 CAContext（渲染服务器）去合成
- 屏幕：`sweetfb_noconsole=1` 停掉控制台后**纯黑**，没有画面

结论：ProgressUI 画出来的东西同样要经过 CoreAnimation 合成，
而合成需要 GPU。**没有 GPU 驱动，任何走 CA 的画面都上不了屏**，
这跟 SpringBoard 是同一道墙。

## 因此

- 显示链路（内核 → 面板）已证明可用：`sweetfb_test=1` 的三色带上屏
- 唯一缺的是"谁来产生并合成像素"，也就是 Adreno 的 Metal/IOAcceleratorES
- 在这一层冒充苹果 GPU 的代价极大，短期内不现实

## 加 stub 之后：窗口服务器真的开始出帧了，但帧还是空的

`AppleM2ScalerCSCDriver` 的 stub（0177）把那个每秒几千行的死循环止住了
（3398 → 49 行），backboardd 随后正常打开它（sel 1），并且：

```
SWEET SURF: backboardd create id 3 1440x3200
SWEET SURF: backboardd create id 4 1125x2436
SWEET SURF: backboardd create id 5 1440x3200
SWEET FB: swap 3 layer 0 surface 5
SWEET FB: swap 4 layer 0 surface 3     ← 3/5 交替，双缓冲，说明窗口服务器在走交换
SWEET FB: layer 0 surface 3 first 0x00000000 nonempty 0/4096
```

也就是说：显示管线、双缓冲、swap 全都通了，**唯一的空洞是没有人往缓冲里画**。
CoreAnimation 依旧没有渲染器（无 GPU），所以它每帧只清屏。

## 结论：现在只差"渲染器"这一个环节

| 环节 | 状态 |
|---|---|
| 内核 → 面板扫描 | ✅ 通 |
| IOSurface 创建/映射 | ✅ 通 |
| IOMobileFramebuffer swap | ✅ 通，双缓冲 |
| 显示服务（scaler stub）| ✅ 通，不再死循环 |
| CoreAnimation 渲染器 | ❌ 空 |
| 数据迁移触发 | ✅ 可复现（删 LastSystemVersion）|

两条可行的下一步，都不在"今晚"的量级：

1. **让 CA 走软件渲染器**。上游笔记提到 QuartzCore 里有 `CA::OGL::SW`（`sw_new_context`，
   完整 CPU 光栅化）以及 `CA_ENABLE_OGL`/`CA_FORCE_LOCAL_SERVER`/`CA_ALWAYS_RENDER`
   这些环境变量，但明确写着"selection logic not traced"。我们试过把这几项塞给
   backboardd/SpringBoard（`tools/k60-ca-env.sh`），没有改变行为。要继续就得从
   dyld 缓存里把 QuartzCore 的渲染器选择逻辑挖出来。
2. **Adreno 驱动**。`refs/UniXNU-pong` 里有 11700 行的 PongAdreno（GMU/CP/ZAP/SMMU，
   同 SM8475），但它是 UEFI + 自有内核树的产物，而且他们自己写着
   "runtime GPU rendering has not yet been confirmed"。

## 加速器 nub 之后（今晚最后一轮）

0178 让 `IOAcceleratorES` 可以被匹配：

```
IOAcceleratorES 轮询：302 次 → 2 次
SWEET ACCEL: backboardd sel 0/2/4/33 ... -> ok (no backend)
nonempty 0/4096      ← 帧仍然全零
```

backboardd 真的打开了加速器并开始问能力/状态，我们一律回「成功、无后端」，
于是 CoreAnimation 拿到设备但不渲染。

**结论：所有铺垫都完成了，唯一的空洞是命令提交后端。**

| 环节 | 状态 |
|---|---|
| 内存窗口（0172）| ✅ |
| 帧缓冲页排除（0170）| ✅ |
| 日志环可靠（0167/0171）| ✅ |
| launchd 不再崩 | ✅ |
| 数据迁移可触发 | ✅ |
| IOSurface / IOMFB / swap 双缓冲 | ✅ |
| 显示服务（scaler stub 0177）| ✅ |
| 加速器 nub（0178）| ✅ 可匹配 |
| **加速器后端（真命令提交）** | ❌ 唯一缺口 |

下一步只有两件事可选：
1. 把 `refs/UniXNU-pong/drivers/PongAdreno` 的后端接进 `SweetAcceleratorUserClient`
   （同 SM8475，但它是 UEFI + 自有内核树，需要逐层适配，且上游自述 runtime GPU 未确认）
2. 挖 QuartzCore 的渲染器选择，让 CA 走 `CA::OGL::SW` 软件光栅化（CPU 渲染）

## 血泪教训：不要就地给活着的 HFS 打块补丁

为了省掉每次 12.4 GB 的整包刷写，我做了一套差异补丁：
`k60-make-patch.py` 比较新旧镜像的 1 MiB 窗口，`k60-patch-userdata.sh` 在
TWRP 里把差异块 `dd` 到分区对应偏移。

刷完之后启动直接 panic：

```
dyld cache '(null)' not loaded: shared cache file 'prebuilt' failed
Library not loaded: /usr/lib/libSystem.B.dylib
cat_resolvelink: can't find inode=307361 on vol=iOSRoot
hfs: set hardlink bit on vol=iOSRoot cnid=307361 inoid=622826597
```

原因：**根文件系统是 ufsrw=1 可写的**，每次启动系统都会写日志、偏好、缓存，
HFS 的目录 B 树和属性文件早就和镜像里那份不一样了。按镜像的整 MiB 窗口覆盖，
等于用旧元数据盖掉新元数据，目录树就散了。

**结论**：整 MiB 的块补丁只能用于只读镜像；对活文件系统要改内容，
要么整包刷（12.4 GB，USB2 约 5 分钟），要么做文件级操作。
差异稀疏镜像也不行——bootloader 遇到 `DONT_CARE` 要去读分区保留原数据，
而它不提供读，会直接挂住。

## QuartzCore 的开关表（已挖出来）与 CA_NO_ACCEL 实验

用用户态探针（`probe/sweetminer.c`，job 塞在 `iomfb_fdr_loader` 槽位）扫
QuartzCore 的段，拿到 96 个真实开关名，关键是这几个：

```
CA_NO_ACCEL  CA_ACCEL_BACKING  CA_ENABLE_OGL  CA_METAL_IMAGE
CA_FORCE_LOCAL_SERVER  CA_ALWAYS_RENDER  CA_DISABLE_RENDER  CA_FORCE_TIMER
```

**踩坑记录**：第一次扫出来是 0 个，因为读的是 Mach-O 的链接地址 `vmaddr`，
运行时镜像有 slide，必须 `slide + vmaddr` 才是真正的段地址。

把 `CA_NO_ACCEL=1`（只留这一个）写进 backboardd 和 SpringBoard 的 job 环境变量，
重新流式刷入后启动：

```
SWEET FB: 18000 swaps        ← 窗口服务器很活跃
nonempty 0/4096              ← 缓冲仍然是空的
```

**结论：单靠环境变量推不动。**窗口服务器的 `sw_renderer` 创建入口被代码条件挡着，
要动它就得改 backboardd 的私有映射（UniXNU 是这么做的，他们是**关闭**这个入口，
说明默认开启过），属于逆向工作。

## 本轮新增的基建（长期有用）

- `tools/k60-…` 之外新加：`simg-expand.py`（流式还原，之前 OOM）
- **流式刷写**：`gzip -1 -c root-hfsx.img | adb shell "gzip -dc > /dev/block/by-name/userdata"`
  在 TWRP 里跑，177 秒完成，比 fastboot 稳（fastboot 一晚上被 USB 掉线坑了七八次）
- `gpu-fw/`：从 vendor 分区取出的 a730 固件（`a730_sqe.fw`、`a730_zap.*`、
  `gmu_gen70000.bin`），GPU 路线的必需输入，已经落地
- 教训：整 MiB 块补丁不能用于可写 HFS（会把目录 B 树踩坏）
