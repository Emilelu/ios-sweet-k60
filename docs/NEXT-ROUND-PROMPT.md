# 下一轮提示词（直接复制给接手的人/助手）

你在接手 Redmi K60（mondrian，SM8475）上的 iOS 16.7.16 移植项目，代码在
`~/hackios`，权威补丁仓库是 `~/hackios/k60-repo-full`（GitHub
`XTOM0706/ios-sweet-k60`）。先读这三个文件再动手：

1. `~/hackios/FORECAST-K60-IOS.md` —— 全部推演、证据、踩过的坑（最重要，末尾是今天的最新结论）
2. `~/hackios/ADRENO-PLAN.md` —— GPU 路线的寄存器地图与 M1–M4 里程碑
3. `~/hackios/artifacts/README-ADT.txt` —— 那条会害死人的旧 ADT

## 现状（有证据，不要重做）

- 内核启动到 `bsd_init`、8 核 SMP、UFS 读写、HFSX 挂根，全部 OK
- **launchd 不再 SIGSEGV**。根因是 shim 把 `boot_args.memSize` 填成 physmap 跨度
  （1216 MB），XNU 拿它当可管理 RAM，管到了 XPU 拒访区，无声挂死。补丁 `0172`
  把它收敛到 `memSizeActual`（703 MB）
- 帧缓冲 `0xb8000000-0xb9200000` 的物理页已从空闲队列排除（`0170`，
  注意：**不能在 vm_page 数组里挖洞**，会触发 `vm_page_init` 断言）
- 日志环可靠：`0167` + `0171`，`ufslogsecs=1` 时最多丢一秒。**别信 `seq`**，
  panic 刷盘把 seq 写死为 1，看 `total` 和正文
- SpringBoard 活着、几十个守护进程正常、数据迁移可复现触发
  （删 `/private/var/mobile/Library/Preferences/com.apple.migration.plist` 的
  `LastSystemVersion`）
- 显示链路全通：IOSurface、IOMobileFramebuffer、双缓冲 swap 都在跑
  （`SWEET FB: swap 3 layer 0 surface 5` 与 surface 3 交替）
- **唯一缺口是渲染器**：所有合成帧都是零。没有 GPU，CA 拿不到渲染器
- 补了 `AppleM2ScalerCSCDriver`（`0177`）和 `IOAcceleratorES`/`AGXAccelerator`
  （`0178`）两个空服务后，backboardd 的轮询和死循环都消失了，但帧依旧为空
- QuartzCore 的 96 个真实开关已挖出（`CA_NO_ACCEL`、`CA_ACCEL_BACKING` 等），
  `CA_NO_ACCEL=1` 灌给 backboardd/SpringBoard 后 swap 到 18000 次，缓冲仍空
- GPU 路线骨架已提交（`0179`，本地未推送）：`SweetAdreno.cpp`，
  `sweetadreno=1` 只读探测、`sweetadreno_power=1` 跑 GPUCC 上电序列

## 下一轮要做的事（按顺序）

1. **先推 `eff0717`**（网络恢复后）：
   `cd ~/hackios/k60-repo-full && git push https://x-access-token:$(cat ~/.config/gh-token)@github.com:443/XTOM0706/ios-sweet-k60.git HEAD:main`
   然后等 CI 出 `xnu-kernel`（89 MB 才算成功，0.2 MB 是失败）
2. **M1 上机验证**：cmdline 加 `sweetadreno=1`，看
   `SWEET ADRENO: probe done` 和 kgsl 前几个字。chipid 应为 `0x7030002`。
   全 `0xffffffff` 或全 0 说明映射/电源域不对，别往下走
3. 只有 M1 通过才跑 `sweetadreno_power=1`，看它停在哪个 phase

## 工具与环境（都用过，直接使）

```
tools/k60-build-boot.sh <kernel> <out.img>   # 现生成 ADT + 校验 + 打包，杜绝旧 ADT
tools/k60-log-pull.sh [out.txt]              # TWRP 里拉内核日志环
tools/k60-log-summary.py [log]               # 页数/首错 ESR/panic/尾行
tools/k60-install-probe.sh                   # sudo：装用户态探针到根镜像
tools/k60-probe-via-job.sh                   # sudo：把探针塞进 iomfb_fdr_loader 槽位
probe/sweetminer.c                           # 挖 QuartzCore 开关（已跑通）
gpu-fw/                                      # 从 vendor 取出的 a730 固件
```

**刷写**：`fastboot` 一晚上被 USB 掉线坑了七八次，改用 TWRP 里的流式写入，
177 秒完成且稳：

```
gzip -1 -c iosroot/root-hfsx.img | adb shell "gzip -dc > /dev/block/by-name/userdata"
```

**不要**用整 MiB 块补丁改根文件系统：根是可写的，系统每次启动都在改目录 B 树，
按镜像覆盖会把 HFS 踩坏（`cat_resolvelink: can't find inode`）。

## 用户偏好

- 中文，每一步要屏幕或日志证据，不接受「CI 说成功」
- 不空转等待（CI 在编时做别的）
- 结论要自查复查
- 设备操作（按键、进 TWRP）只能他来做
