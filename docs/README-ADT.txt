artifacts/k60.adt 是旧布局，别用
==============================

它声明的 DRAM 是 0x80000000 + 0x39200000（到 0xb9200000），
把 hypervisor(6MiB@0x80000000)、smem、cdsp(70MiB@0x80c00000)、
modem(306MiB@0x8bc00000) 全算成了可用 RAM。

用它启动会看到：vm_page_bootstrap 报 55885 个空闲页（873 MB，
远超物理可用的 703 MB），随后所有守护进程在 dyld 缓存里随机 SIGSEGV，
屏幕花屏。2026-10-25 因此白跑两轮。

正确做法（每次现生成，不要缓存）：

  cd ~/hackios/ios-sweet
  ADT_PHYS_BASE=0xb4000000 ADT_MEM_SIZE=0x2c000000 \
    python3 tools/mkadt.py /tmp/adt.bin --k60

生成的 ADT 里 dram-base=0xb4000000、dram-size=0x2bf00000（703 MB，
末 1 MiB 是 embedded panic log）。验收方法：

  python3 -c "d=open('/tmp/adt.bin','rb').read(); import struct; print(struct.pack('>Q',0xb4000000)[::-1] in d)"
