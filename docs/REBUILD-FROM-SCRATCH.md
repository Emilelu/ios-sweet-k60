# 把 K60 重新变成这台 iOS 机器

设备上的 iOS 全在 userdata 里，格式化 data 就没了，所以这份是从零恢复的顺序。

## 需要的两样东西（都在这台机器上）

| 东西 | 路径 | sha256 前 16 位 |
| --- | --- | --- |
| iOS 根镜像（写进 userdata） | `~/hackios/iosroot/root-hfsx.img` | `de8425cb43f6c2c2` |
| 能进锁屏的启动镜像 | `~/hackios/artifacts/xnu-0218.img` | `bb823ee3d5479af0` |

根镜像里已经包含：跳数据迁移的 `com.apple.migration.plist`（`LastSystemVersion
= 20H392`）、给 lsd 用的 `/chosen` 假 WiFi MAC（在设备树里，不在镜像里）、
以及 iOS 16.7.16 的完整用户态。

## 顺序

1. 手机进 TWRP（`adb devices` 能看到 `recovery`），确认 `userdata` 没被挂载。
2. 把根镜像流式写回去（约 6.5 分钟，41 MB/s）：

   ```sh
   cd ~/hackios
   adb shell umount /dev/block/by-name/userdata 2>/dev/null
   dd if=iosroot/root-hfsx.img bs=4M | adb shell 'dd of=/dev/block/by-name/userdata bs=4M conv=notrunc,fsync'
   ```

3. 重启到 bootloader，内存启动那颗镜像（**不要** dd 进 boot_a，会连 recovery 一起冲掉）：

   ```sh
   adb reboot bootloader
   fastboot -s ffec9d48 boot artifacts/xnu-0218.img
   ```

4. 判据：屏幕先出苹果标志，进度条走完后出现锁屏。想拉日志就重启回 TWRP，
   `tools/k60-log-pull.sh <输出文件>`。

## 设备树的三个关键值

都在 `ios-sweet/tools/mkadt.py` 里，改完要重打镜像：

- `/chosen` 的 `mac-address-wifi0`（6 字节假 MAC）：没有它 lsd 建库时会卡在
  MobileGestalt 的网络匹配通知上。
- `disp0` 的 `canvas-width` / `canvas-height`：**必须等于面板 1440×3200**，
  报别的尺寸 UIKit 会去找不存在的 bounding path 位图然后 abort。
- `display-scale` = 3。

## 已知未完成

- 触摸：卡在 RSC 块访问（读 `0x17a00000` 就死），等上游修。
- 壁纸：没配置过，锁屏是黑底，不是 GPU 问题。
- GPU：没有实现，M 系列补丁只到 GMU 预演。
