# 回来后从这里继续

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
