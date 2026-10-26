# K60 内存窗口离线校验台

`MEMORY-PLAN.md` 里的每个数字都从这里出。设备不在、也没有 macOS，所以把
`pmap_startup()` 的取页逻辑原样搬到 x86 上跑，再用两个独立来源的物理内存表
判定"放给 VM 的页是不是 AP 真能解码的 DRAM"。

```
./run.sh          # 全绿
```

三层：

1. `memplan.py` 从 `dump/live.dts`（ABL 交给 Android 的活 FDT：DRAM bank +
   全部 no-map carve-out）和 `dump/iomem.txt`（同一台机器上 Android 的
   `/proc/iomem`：AP 真正用到的 System RAM）重建物理表，结果存进
   `k60-physmap.json` 并带两个源文件的 sha256。dump 在就逐项比对，漂移即报错。
2. `sim.c` 是 `osfmk/vm/vm_resident.c:pmap_startup()` 的页面模型：
   `npages = span/(PAGE+sizeof(vm_page))`、`pmap_steal_freeable_memory()` 先把
   vm_page 数组从窗口底部抠走、然后按 `first_avail` 递增取页、命中 notch 的页
   不释放。输出"哪些字节区间最终会进空闲队列"。
3. `check.py` 把输出变成判据：空闲区间必须完全落在 iomem System RAM 内；窗口内
   每个 carve-out 必须被 0184 的 notch 覆盖；panic log 必须在 DRAM 里且避开
   notch；ADT 生成器（和实际打包用的是同一份 mkadt.py）产出的 dram-base/size
   必须与方案一致。

另外三组负例必须失败，否则这些判据是空的：0172 之前的 physmap 窗口、
`artifacts/k60.adt` 的旧布局、以及去掉 gunyah notch 的 P1 窗口。还有一条正向
对照：用 2026-10-25 那次坏 ADT 启动的窗口，模型必须算出和屏幕上一模一样的
`55885` 个空闲页（`OLD_ADT_FOOT = 0x2488000` 就是这么反向解出来的；它属于那颗旧
内核，和 `KERNEL_FOOT` 无关——后者随当前内核镜像的 `topOfKernelData` 变）。
