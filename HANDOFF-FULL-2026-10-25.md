# Redmi K60 上移植 iOS（ios-sweet）—— 完整交接文档

> 写于 2026-10-25，用户暂停 K60 任务转做其他项目。
> 本文档目标：任何人（或未来的我）读完就能接手，不需要翻历史会话。

---

## 一、一句话现状

**iOS 用户态已经能跑到"全部启动任务完成 + backboardd 打开我们的 HID 驱动"，
唯一阻塞是 `launchd` 最后 SIGSEGV 导致系统 panic。屏幕有花屏现象，可能与崩溃同源。**

距 SpringBoard（苹果开机动画）还有未知距离，取决于 launchd 崩溃的根因。

---

## 二、硬件与目标

| 项 | 值 |
|---|---|
| 机型 | Redmi K60（代号 **mondrian**）|
| SoC | Snapdragon 8+ Gen 1（**SM8475**）|
| 序列号 | `ffec9d48` |
| 屏幕 | 1440×3200，帧缓冲在 `0xb8000000` |
| 内存 | 12 GB 物理，**AP 只能访问 2 GB**（`0x80000000..0x100000000`）|

**目标**：把 iOS（XNU 8796 / iOS 16 内核）跑起来，最终显示苹果开机动画。

---

## 三、当前设备状态

| 项 | 状态 |
|---|---|
| 引导方式 | `fastboot boot <img>`（**未刷入 boot 分区**）|
| 设备当前 | 可能在 fastboot 或 TWRP，也可能关机 |
| userdata 分区 | ✅ 已写入 11.5 GB HFSX 格式 iOS 根文件系统（**安卓数据已按用户授权清空**）|
| boot 分区 | ✅ **未改动**，备份在 `~/hackios/boot-original-boot_a.img`（193 MB）|
| 恢复安卓 | 需刷官方固件包（userdata 的安卓数据已丢）|

### 进入各模式

```bash
adb reboot bootloader      # 从 TWRP/安卓 → fastboot
fastboot reboot recovery   # 从 fastboot → TWRP
# 设备挂死时：长按电源 10 秒
```

---

## 四、目录与文件清单（全部在 `~/hackios/`）

### 4.1 核心产物

| 路径 | 大小 | 说明 |
|---|---|---|
| `iosroot/root-hfsx.img` | 12 GB（16 GB 逻辑）| **iOS 根文件系统**，HFSX 大小写敏感 + 日志式，已含所有 tweak |
| `boot-original-boot_a.img` | 193 MB | **原厂 boot 分区备份**（刷 boot 前必须保留）|
| `artifacts/kernel.development.sweet` | 17 MB | 当前 CI 内核（含最新补丁）|
| `artifacts/kernel-diag.sweet` | 17 MB | 同上，但 **pmap 断言被 NOP 掉**（诊断用）|
| `artifacts/k60.adt` | 9.9 KB | Apple 设备树（ADT），K60 版 |
| `artifacts/k60-pinned.adt` | 9.9 KB | 固定种子的 ADT（**有内存布局问题，别用**）|
| `artifacts/payload-k60.bin` | 17 MB | 内核 + ADT + boot_args 打包体 |
| `last-boot-kernel.log` | 15-42 KB | 最近一次从 TWRP 拉出的内核日志 |

### 4.2 仓库

| 路径 | 说明 |
|---|---|
| `~/hackios/ios-sweet/` | **sweet 原始项目**的本地副本（81 个补丁，不含 K60 专用补丁）|
| `~/hackios/k60-repo-full/` | **K60 完整仓库**（GitHub `XTOM0706/ios-sweet-k60` 的克隆，**141 个补丁**）← **这是权威版本** |
| `~/hackios/k60-repo/` | 旧的部分副本（21 MB，可能过时）|

**⚠ 重要**：K60 专用补丁（`0140+`）**只存在于 GitHub 和 `k60-repo-full/`**，
`ios-sweet/xnu/patches/` 里**没有**。改补丁必须从 `k60-repo-full/` 出发。

### 4.3 工具（`tools/`）

| 工具 | 用途 | 用法 |
|---|---|---|
| **`k60-log-pull.sh`** | ★ **从 TWRP 一条命令拉完整内核日志** | `./tools/k60-log-pull.sh [输出.txt]` |
| **`sym.py`** | ★ 符号化 panic 地址 | `python3 tools/sym.py 0xADDR` 或 `--grep 符号名` |
| **`nop-pmap-assert.py`** | ★ 把 `pmap_assert_free` 的 5 处调用改成 NOP | `python3 tools/nop-pmap-assert.py [src] [dst]` |
| `verify-kernel.py` | 验证内核里关键补丁是否生效 | `python3 tools/verify-kernel.py <kernel>` |
| `ci-fetch-kernel.sh` | 从 GitHub Actions 拉内核 | `tools/ci-fetch-kernel.sh <run_id>` |
| `rebuild-xnu-image.sh` | 拉内核 + 建 payload + 建镜像（**参数硬编码，慎用**）| |
| `k60-flash-root.sh` | 刷 userdata（需 sudo + 13 GB 空间）| `sudo tools/k60-flash-root.sh` |
| `mk-simg.py` | 稀疏镜像编码器（已修 bug）| |
| `k60-convert-hfsx.sh` | 把 HFS+ 镜像转成 HFSX | |
| `k60-root-tweaks2.sh` | 第二轮镜像调整（preboot 结构 + 删 driverkitd）| 需 sudo |
| `k60-finalize-root.sh` | 清理脏标记 + 挂载 + 跑 tweaks | 需 sudo |
| `dev.sh` | 报告设备状态（fastboot/android/off）| |
| `check-kernel-timer.py` | 检查内核有没有 CNTP 写（已知溢出 bug）| |

### 4.4 文档

| 文件 | 行数 | 内容 |
|---|---|---|
| **`HANDOFF-FULL-2026-10-25.md`** | 本文件 | ★ 完整交接 |
| `FORECAST-K60-IOS.md` | 420+ | 完整推演 + 12 项已排除清单 |
| `PLAN-REMAINING.md` | 100+ | 剩余计划 + 暂停点 |
| `HANDOFF-2026-10-25.md` | 140 | 上一版交接（较早）|
| `HANDOFF-K60-MILESTONE.md` | 99 | 原四目标里程碑 |
| `ios-sweet/K60-HANDOFF-2026-10-23.md` | — | 更早的交接 |
| `ios-sweet/NOTES-K60-MANUAL.md` | — | 手工笔记 |

### 4.5 参考材料

| 路径 | 用途 |
|---|---|
| `dump/live.dts` | **K60 真实设备树**（1.7 GB 目录里，reserved-memory 分析靠它）|
| `refs/` | 参考项目源码 |
| `rootfs/` | 早期 rootfs 试验 |
| `xnu-*.img` | 各种测试镜像（17 MB × 20+，可删）|

---

## 五、启动流程（每轮测试的动作）

```bash
# 1. 确保设备在 fastboot
adb reboot bootloader        # 或手动音量下+电源

# 2. 启动镜像
fastboot boot ~/hackios/xnu-XXX.img

# 3. 等 3-4 分钟（iOS 启动 + 崩溃）

# 4. 设备会挂死（panic 页保持）→ 长按电源 → 进 TWRP
fastboot reboot recovery     # 如果还能连上
# 否则长按电源，然后音量上+电源进 recovery

# 5. 拉日志（核心诊断手段）
cd ~/hackios && ./tools/k60-log-pull.sh /tmp/log.txt

# 6. 分析
grep -E 'CRASH|panic|SWEET' /tmp/log.txt
python3 tools/sym.py 0xADDR
```

---

## 六、已完成的工作（有证据）

### 6.1 内核层

| 项 | 状态 | 证据 |
|---|---|---|
| XNU 8796 启动到 bsd_init | ✅ | `SWEET-STAGE-50: bsd_init entered` |
| 8 核 SMP | ✅ | `cpus=8`，调度器正常 |
| 显示输出（内核自绘）| ✅ | **XTOM 字样**显示在面板上 |
| 内存布局修复 | ✅ | 内核加载在 `0xb4000000`（避开固件区）→ **所有 pmap/vm_page 断言消失** |

### 6.2 存储层

| 项 | 状态 | 证据 |
|---|---|---|
| UFS 驱动（SM8475 UFS 3.1）| ✅ | `"userdata" on LUN 0: 473033 MiB, read-write` |
| HFS+ / HFSX 驱动 | ✅ | `hfs: mounted iOSRoot on device b(1, 0)` |
| 根文件系统挂载 | ✅ | `BSD root: ufsroot0, major 1, minor 0` |

### 6.3 用户态（**最远的进展**）

| 组件 | 状态 | 证据 |
|---|---|---|
| `launchd` (pid 1) 启动 | ✅ | `launchd UUID: ...` `Early boot complete. Continuing system boot.` |
| **全部 boot task** | ✅ | fsck, mount-phase-1/2, data-protection, obliteration, **fips（corecrypto 全 PASSED）**, keybag, **usermanagerd（5 个 persona）**, tzinit, finish-restore, launchd_cache_loader |
| AMFI 服务 | ✅ | `SWEET AMFI: user client selector 6 accepted` |
| dyld 缓存投递 | ✅ | `Cache sent to launchd successfully` |
| **backboardd** | ✅ | 打开 `SweetHIDTouch` / `SweetHIDButtons` / `AppleKeyStore` |
| `powerd` | ✅ | 通过 AppleKeyStore sel 0/7/17 |
| `containermanagerd` | ✅ | sandbox op 15/16 + AKS sel 17 |
| `cfprefsd` / `keybagd` / `logd` | ✅ | 有日志输出 |
| **`launchd` 存活** | ❌ | **SIGSEGV**（`reason namespace 2 subcode 0xb`）|

---

## 七、已排除的原因（**别重复走**）

| # | 假设 | 排除依据 |
|---|---|---|
| 1 | HFS+ 位定义搞反 | 已修：`attrs = (attrs \| 0x100) & ~0x800`（0x100=干净卸载）|
| 2 | HFS 大小写不敏感 | ✅ **真 bug，已修**：必须 HFSX（`HX` v5），`newfs_hfs -s -J` |
| 3 | UFS 唤醒命令错 | ✅ 已修：必须发 START STOP UNIT 到 wlun `0xd0`，byte4=`0x10`，且用 spin 不能用 `delay` |
| 4 | `MNT_ROOTFS` 未前置 | ✅ 已修 |
| 5 | 代码签名拦住 launchd | ✅ 已绕过（3 个 boot-arg + 2 个补丁）|
| 6 | AMFI 找不到/连不上 | ✅ 已修（类名 `AppleMobileFileIntegrity` + IOUserClient）|
| 7 | 内核压在固件保留区 | ✅ **真 bug，已修**（0x80000000 是 Hypervisor → 移到 0xb4000000）|
| 8 | **dyld 共享缓存装不上** | ❌ **假设不成立**：实测 `base=0x180000000 size=4G handle=非NULL` 完全正常 |
| 9 | `check_np` 失败是 bug | ❌ 正常流程（dyld 启动时还没装缓存）|
| 10 | SMP/clutch 竞态 | ❌ 单核也崩 |
| 11 | 特定守护进程问题 | ❌ 换 image 崩在不同进程 |
| 12 | 内存种子/physmap slide | ❌ 换种子结果一样 |
| 13 | `sweet_sample` 竞态 | ❌ 禁用后仍崩 |
| 14 | HID 定时器风暴 | ❌ 需要 boot-arg 才启用，默认无 |
| 15 | UFS PRDT 表溢出 | ❌ `PRDT_MAX=4`，正好 4 项 |
| 16 | 高内存区可用 | ❌ **XPU 拦截**：读 `0x100000000` 立即同步外部中止 |
| 17 | `0xe9f00000` 以上可用 | ❌ **XPU 拦截**，`ml_static_mfree` 会硬挂（无任何输出）|

---

## 八、当前卡点与下一步

### 8.1 卡点：launchd SIGSEGV

```
pid 1 exited -- exit reason namespace 2 subcode 0xb
                          ↑ OS_REASON_SIGNAL  ↑ 11 = SIGSEGV
panic: launchd exited
```

**关键未知**：崩溃地址（`pc` / `lr` / `far`）

### 8.2 用户观察到的现象（重要线索）

> **"会花屏"** —— 屏幕内容变成彩色条纹/乱码

这个现象和日志里"屏幕上下边缘彩色条纹"一致 →
**说明帧缓冲内存被破坏** ✗
**可能与 launchd 崩溃同源**（都是内存被踩）✓

### 8.3 诊断阻塞：日志只写一次

```
现象：日志环 seq 永远是 1，每次拉到的都是"第一次刷新时"的内容
原因（推测）：日志线程 lck_mtx_lock(&sweet_ufs_lock) 被 HFS 读盘长期占用 → 阻塞
已尝试修复：0167 改用 lck_mtx_try_lock（拿不到就跳过这个周期）
状态：未验证（构建被后续提交取消）
```

### 8.4 补丁状态清单

| 补丁 | 内容 | 状态 |
|---|---|---|
| `0140` | UFS wlun 唤醒 + spin 等待 | ✅ 生效 |
| `0142` | AMFI 允许 invalid page | ✅ |
| `0143` | USB 探针 opt-in | ✅ |
| `0144` | exec 代码签名绕过 | ✅ |
| `0145` | AMFI IOService + IOUserClient | ✅ |
| `0147` | logo band 映射（320 行而非全屏）| ✅ |
| `0148` | USB 电源轨走触摸路径 | ✅ |
| `0151` | pmap 断言调用者诊断 | ✅ |
| `0152` | 上次启动日志回读 | ✅ |
| `0154` | **UFS DMA canary**（检测设备写越界）| ✅ 未触发 → 排除 DMA 越界 |
| `0155` | 共享区域日志（固定行）| ✅ 已用 |
| `0156` | 日志写失败重试 | ✅ |
| `0157` | 共享区域创建日志 | ✅ 已用（得到关键答案）|
| `0158` | 日志线程探针 | ✅ |
| `0159` | 共享区域创建固定行 | ✅ |
| **`0162`** | **日志通道：`tsleep`→`delay` + `panic()` 同步刷盘** | ✅ **有效**（日志能写盘了）|
| `0166` | 特权全授予 | ❌ **已撤**（导致启动几秒就死）|
| **`0167`** | **日志 `lck_mtx_try_lock`** | 🔄 **待验证**（关键）|
| `0168` | 只授 `PRIV_GLOBAL_PROC_INFO`(1002) 给 logd | 🔄 待验证 |

### 8.5 下一步优先级

```
1. 【最高】验证 0167 是否让日志写出多段（seq > 1）
   → 如果能，就有了完整崩溃日志

2. 拿到 CRASH launchd 的 pc/lr/far
   → 用 sym.py 符号化
   → far 指向哪里决定修法：
     - 0x0~0xfff        → 空指针，查 launchd 逻辑
     - 0x180000000 附近  → 共享缓存访问问题
     - 0x16xxxxxxxxx    → 堆/栈损坏

3. 追踪"花屏"出现的时机
   → 如果和 launchd 崩溃同时出现 → 同一根因（内存被踩）
   → 如果更早 → 帧缓冲映射问题

4. 如果 launchd 修好 → backboardd 出画面（代码已实现 ✓）
   → 然后 SpringBoard
```

---

## 九、关键技术参数（改代码必看）

### 9.1 内存布局（**极其重要**）

```
AP 可访问：  0x80000000 .. 0x100000000（2 GB）
其中固件占用：603 MB（38 个 reserved-memory 区域）
  - hyp_region@0x80000000      6 MiB  ← 内核原来的位置 ✗
  - smem_region@0x80900000     2 MiB
  - cdsp_secure_heap@0x80c00000 70 MiB
  - mpss_region@0x8bc00000    306 MiB（modem）
  - ...
可用最大值： 0xb4000000 + 703 MiB（含帧缓冲）← **当前使用**
XPU 边界：   0xe9f00000（以上完全不可访问 ✗）
高内存区：   0x1_00000000 以上（4.2 GB）**不可访问** ✗
```

**改动内存布局的规则**：
- 内核必须加载在 `0xb4000000` 或类似的干净区
- 帧缓冲 `0xb8000000` **必须在 DRAM 窗口内**（早期 console 用 `phystokv`）
- 不要把 `0xe9f00000` 以上的地址交给 VM（`ml_static_mfree` 会硬挂）

### 9.2 帧缓冲

```
物理地址： 0xb8000000
分辨率：   1440×3200
stride：   5760 字节（1440×4）
大小：     18,432,000 字节（约 17.6 MiB）
```
显示能工作是因为 **ABL 的 DPU 一直在扫描这块内存** ✓（不是我们配置的 ✓）

### 9.3 关键常量

| 项 | 值 |
|---|---|
| 内核 `__TEXT` VA | `0xfffffe0007004000` |
| 页大小 | **16 KiB**（`pexpert/pexpert/arm64/SWEET.h: #define __ARM_16K_PG__ 1`）|
| 共享区域基址 | `0x180000000`（4 GB 大小）|
| UFS wlun | `0xd0` |
| GIC 重分发器 | `0x17180000`，stride `0x40000` |

### 9.4 必需的 boot-args

```
debug=0x8 keepsyms=1 cpus=8 -noprogress serial=1 sweet_notests=1
rd=ufsroot0 ufsroot=userdata ufsrw=1
amfi_get_out_of_my_way=1 cs_enforcement_disable=1 amfi_allow_any_signature=1
ufslogsecs=5
```

| 参数 | 作用 |
|---|---|
| `serial=1` | 必须，否则无 kprintf/fbcon 输出 |
| `cpus=8` | 8 核 |
| `ufsroot=userdata` + `ufsrw=1` | 从 userdata 挂根，可写 |
| `amfi_get_out_of_my_way=1` 等 | 绕过代码签名 |
| `ufslogsecs=5` | 日志刷盘间隔（秒）|

---

## 十、CI 与构建

### 10.1 GitHub Actions

```
仓库： XTOM0706/ios-sweet-k60
文件： .github/workflows/xnu.yml
触发： push 到 main（改 xnu/patches/ 下任何文件）
耗时： ~10-15 分钟（macOS runner，已加 ccache）
产物： xnu-kernel（89 MB）+ xnu-buildtree（22 MB）
```

**注意**：成功/失败标志不重要 ✓ —— 看产物大小：
- 89 MB = 构建正常 ✓
- 0.2 MB = 构建失败 ✗

### 10.2 拉内核

```bash
export GH_TOKEN=$(cat ~/.config/gh-token)
# 查最近 run
curl -sS -H "Authorization: Bearer $GH_TOKEN" \
  "https://api.github.com/repos/XTOM0706/ios-sweet-k60/actions/runs?per_page=1"

# 查产物 ID
curl -sS -H "Authorization: Bearer $GH_TOKEN" \
  "https://api.github.com/repos/XTOM0706/ios-sweet-k60/actions/runs/<RUN_ID>/artifacts"

# 下载（90 MB，需要代理）
curl -sSL -H "Authorization: Bearer $GH_TOKEN" \
  "https://api.github.com/repos/XTOM0706/ios-sweet-k60/actions/artifacts/<ARTIFACT_ID>/zip" -o k.zip
unzip -q k.zip
```

### 10.3 生成测试镜像

```bash
cd ~/hackios/ios-sweet

# 1. 生成 ADT（内存布局）
ADT_PHYS_BASE=0xb4000000 ADT_MEM_SIZE=0x2c000000 \
  python3 tools/mkadt.py /tmp/adt.bin --k60

# 2. 打包 payload
python3 tools/mkpayload.py <kernel> /tmp/adt.bin /tmp/pl.bin \
  --cmdline "debug=0x8 keepsyms=1 cpus=8 -noprogress serial=1 ..."

# 3. 构建 boot.img
cd shim
HDRVER=4 CFLAGS_EXTRA="-DK60_XNULOG -DK60_NO_PHYSMAP_EXT" \
  PAYLOAD=/tmp/pl.bin ./build.sh
cp out/shim-boot.img ~/hackios/xnu-test.img
```

**可选编译宏**：
- `-DK60_XNULOG` — 正常启动 XNU（不加则只跑 shim 诊断）
- `-DK60_NO_PHYSMAP_EXT` — 不扩展 physmap（**安全，当前默认**）
- `-DK60_RDCHECK` — DDR 可访问性探针

---

## 十一、血泪教训（下次别再犯）

1. **补丁必须基于最新 buildtree 的累积源码生成**
   - 我用旧源码生成过 5 次补丁，全部被静默跳过 ✗
   - 正确做法：从 CI 产物 `xnu-buildtree` 解出目标文件，在其上改，再 `diff`
   - 生成后必须 `patch --dry-run` 本地验证

2. **每次拿内核先验证字符串**
   ```bash
   python3 -c "d=open('kernel','rb').read(); print(b'我的新字符串' in d)"
   ```
   不验证 = 白跑一轮 15 分钟

3. **panic 会清屏**
   - 屏幕固定行（`sweet_fbcon_trace`）在 panic 后也被覆盖 ✗
   - 唯一可靠的备份是**日志环** → 所以日志通道是第一优先

4. **设备硬挂 vs 普通 panic**
   - 普通 panic：屏幕保持 panic 页，USB 可能消失
   - 硬挂（如 XPU 访问）：**完全无输出**，连 panic 都没有 ✗
   - 判断：`tools/dev.sh` 返回 `off` 且长时间无反应

5. **`fastboot boot` 不可靠时会失败**
   - 如果 `probe.sh` 超时，直接 `fastboot boot <img>` 重试一次
   - 有时需要先 `adb reboot bootloader` 确保在 fastboot

6. **磁盘空间**
   - 刷 userdata 需要 **13 GB**（稀疏镜像 12 GB）
   - `iosroot/root-hfsx.simg` 是中间产物，刷完可删

7. **`rebuild-xnu-image.sh` 参数硬编码**
   - 它用 `artifacts/k60.adt` + 固定 cmdline，不适合自定义测试
   - 自定义请手工走 10.3 的三步

---

## 十二、如果要在新会话继续

### 最小启动上下文

```
1. 读 ~/hackios/HANDOFF-FULL-2026-10-25.md（本文件）
2. 读 ~/hackios/PLAN-REMAINING.md（剩余计划）
3. 设备状态：cd ~/hackios && ./tools/dev.sh
4. CI 状态：见第十节
```

### 第一件事

```bash
# 验证 0167 是否生效（日志能写多段）
# 1. 看最新 CI 产物
# 2. 装内核 → 构建镜像 → fastboot boot
# 3. 崩溃后进 TWRP → ./tools/k60-log-pull.sh
# 4. 看 ring header 的 seq 是否 > 1
```

### 目标优先级

```
目标①日志通道      → 0162 已修，0167 待验证
目标②dyld共享缓存  → 实测不是问题，需要重写为"launchd SIGSEGV 根因"
目标③backboardd出画面 → 代码已实现（IOMobileFramebuffer 全套 selector）
目标④SpringBoard   → 前置：backboardd 需要先稳定
```

---

## 十三、其他重要信息

### 13.1 参考项目

| 项目 | 机型 | 进度 |
|---|---|---|
| `Chengdong-Yu/UniXNU-pong` | Nothing Phone 2（同 SM8475）| **到主屏** ✓ 但走 UEFI 路线，非 ios-sweet |
| `xNekom/ios-mondrian` | POCO F5 Pro（**同 K60**）| 到 backboardd，**和我们同一位置** |
| `HackiOS/ios-sweet` | iPhone 7 | 原始项目 |

### 13.2 GitHub 认证

```bash
cat ~/.config/gh-token        # token 在这
export GH_TOKEN=$(cat ~/.config/gh-token)
```

### 13.3 用户偏好

- 用 **fish shell**
- 不能提供 sudo 密码 → root 命令需要**用户自己粘贴执行**
- 要求：**每一步都要有屏幕/二进制证据**，不信 CI 的 "success"
- 要求：**不要干等**（不要空等 CI / 不要重复轮询）
- 要求：**自查 + 复查**（补丁要验证、结论要交叉验证）
- 网络需要代理（用户会开）

### 13.4 安卓恢复

```bash
# userdata 已清空（iOS 根在里面）
# 恢复安卓需要：
# 1. 下载小米官方固件包（K60 / mondian，约 5 GB）
# 2. fastboot 刷入（或 miflash）
# 3. boot 分区未改动，不需要单独恢复
```

---

**文档结束。有任何不明处，先查 `FORECAST-K60-IOS.md`（420+ 行完整推演）。**
