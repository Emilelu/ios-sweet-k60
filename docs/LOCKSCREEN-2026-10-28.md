# 锁屏（2026-10-28）投入产出记录

## 结论

显示链路走通：K60（mondrian，SM8475）上 iOS 16.7.16 能进锁屏。
触摸、壁纸、GPU 都还没有；触摸卡在 RSC 块不可访问。

## 显示：从「一帧都没有」到锁屏

四件事叠起来才行，缺一条 CA 就不提交任何帧：

1. 画布按面板原生 **1440×3200** 报，不做逻辑尺寸伪装。
   曾经改成 1125×2436 报 iPhone X，结果 UIKit 去找
   `BoundingPathBitmap-1125x2436-375x812-3.00x`，`Assets.car` 里名字在、
   数据拿不到，NSAssert 后 SIGABRT。名字由 `-[UIScreen _screenType]` 的类型表
   拼出，与面板无关；换任何有刘海的几何都会撞同一面墙。
2. `AppleCLCD` 发布 **TimingElements / ColorElements**（补丁 0218）。
   没有这两个属性，这块屏「没有模式」，CA 一帧不画。
3. `GetID` 返回 1（0 会被当成「没有显示」）；实现选择器 **77**
   （GetCurrentAbsoluteTime）与 **79**（GetBlock，`0x40` 回当前模式 id）。
4. 发布一个空的 **IOAcceleratorES**（0178，默认开），CA 才认为有渲染器。

## 壁纸：不是 GPU

- 撤掉空加速器后壁纸依然不出现，painted 两轮都约 10 万像素，排除 GPU。
- PosterBoard 活着：采样里一条线程在 CoreGraphics/vImage 里干活，主线程空闲。
- 更可能是跳过了 Setup、壁纸从未配置，锁屏画的是黑底。
- 一次满屏 `0xfffa00fa`（品红）的整帧，是某处拿到空图像时的填充色。

## 触摸：卡在 RSC

- 硬件（live FDT 已核）：SE4 `0x990000`、SPI 引脚 GPIO16-19、复位 20、
  IRQ 21、AVDD 71、坐标 14400×32000；IOVDD = `pm8350c_l2` /
  `rpmh-regulator-ldoc2`，init 1.8V / mode 4 / 支持模式 2,4。
- 已移植 UniXNU-pong 的 Goodix-over-GENI-SPI（0240-0243），按级别验证。
- 只上电那步通过；**读 RSC 第一个地址 `0x17a00000` 整机死**，没有 panic 输出。
  所以不是 TCS 槽位写法问题，是整块 RSC 不可达（XPU / 电源域 / 互连未就绪，
  或者必须由远程处理器配置）。0248 的探针给出这条硬结论。
- 绕道：`k60rpmhoff=1` 跳过投票，直接验 GCC 时钟 → GENI → SPI → Goodix id；
  id 能读出来就说明 IOVDD 本来就开着。

## 顺手修掉的三个坑

- IRQ 是开漏输出，没上拉读出来必然 0，不能据此判断「没供电」（0245 改成上拉，
  并在放开复位前再读一次）。
- 分级判断原来放在执行之后，`stage=3` 也会跑 IOVDD（0246 加 `k60rpmhoff`）。
- CI 原来 cancel-in-progress，推得太快会把正在跑的那条取消，产物就没了
  （workflow 已改为 false）。

## 当前镜像与判据

- 能进锁屏：`artifacts/xnu-0218.img`，或之后任何含 0218 的镜像。
- 扫描：`sweetfb_scan=N` 每 N 次 swap 整屏统计，输出
  `periodic scan swap N surface S px=.. painted=.. black=.. clear=.. first_painted=..`。
- 触摸：`k60touchstage=1..5`，`k60rpmhoff=1` 跳过 RSC，`k60rscprobe=1` 只读侦察。

## 下一步

触摸的阻塞点是 RSC 访问权限，不是协议层。等上游修好再接。
