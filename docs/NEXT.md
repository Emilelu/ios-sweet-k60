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
- **卡点**：xnu 只能 macOS 编；本轮没有推送任何远端，CI 还没编过 0179/0180。
  上机前必须先让 CI 出一颗带 `SWEET ADRENO` 字符串的 kernel。
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
