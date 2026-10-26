# SweetAdreno 离线校验台

`sweetadreno=1` 的只读探测和 `sweetadreno_power=1` 的上电序列，在设备不在、
也没有 macOS 的情况下用假寄存器文件跑一遍，逐条核对 vendor/Pong 语义。

```
./run.sh 0180     # 全绿
./run.sh 0179     # 有 3 项 FAIL：这是修正前的行为，留作对照
```

三层：

1. `extract.py` 按补丁序号重建 `iokit/Kernel/arm/SweetAdreno.cpp`（0179 新建，
   0180 叠加），保证校验的就是要上机的那份，而不是手抄件。
2. aarch64 目标（`--target=aarch64-none-elf`）真编译一次，`dsb sy` 这类内联汇编
   只有这个目标会暴露问题。
3. `host.cpp` 把 `ml_io_map()` 指到假的寄存器块，用 hook 模拟 CLK_OFF 清位、
   GDSC 状态位、mux update 位；`delay()` 只累加时间。x86 版只把 `dsb` 屏障换成
   空操作、把两处裸指针访问换成带记录的访问器，寄存器逻辑一行不改。

四个场景对应真实硬件上的合法状态：happy（全清）、hwcg（GCC 0x81004 保持
CLK_OFF 但 bits0..1==3）、delayed（0x911c 不清 CLK_OFF）、busy（root mux 的
update 位残留）。
