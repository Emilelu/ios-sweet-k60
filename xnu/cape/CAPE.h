/*
 * Board config for the Redmi K60 ("mondrian", Qualcomm SM8475 /
 * Snapdragon 8+ Gen 1, "cape" platform): Kryo = Cortex-X2 + 3x A710 +
 * 4x A510, ARMv9 but with pointer authentication *disabled* in this build
 * (like SWEET: the iOS 16 userland of the iPhone X is not PAC'd either).
 *
 * Generic ARM like SWEET (no APPLE_ARM64_ARCH_FAMILY), 16K pages, GICv3 with
 * two security states: Group 0 belongs to TrustZone, so every interrupt,
 * including the timer, is a Group 1 IRQ taken through sleh_irq().
 *
 * The values below come from the live FDT of the device
 * (~/hackios/dump/live.dts), not from the upstream QCOM kernel tree:
 *   - timer: arm,armv8-timer interrupts = <PPI 13 14 11 12>, i.e. the
 *     *standard* numbering (unlike sweet's 1/2/3/0), so the virtual timer
 *     XNU uses (CNTV) is INTID 16 + 11 = 27.
 *   - GIC: intc@17100000, reg = <0x17100000 0x10000 0x17180000 0x200000>,
 *     redistributor-stride = 0x40000, 8 cores => 8 * 0x40000 = 0x200000.
 */

#ifndef _PEXPERT_ARM64_CAPE_H
#define _PEXPERT_ARM64_CAPE_H

#define NO_MONITOR                1
#define NO_ECORE                  1

#define CAPE                      1

#define ARM_ARCH_TIMER
#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             8
#define __ARM_ENABLE_SWAP__       1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define __ARM_PAN_AVAILABLE__     1
#define __ARM_16K_PG__            1

#define HAS_GIC_V3                1

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif /* ! ASSEMBLER */

/* Standard architected timer PPIs on cape (13/14/11/12), so CNTV = 16 + 11. */
#define CAPE_VTIMER_INTID         27
#define CAPE_PTIMER_INTID         30
#define CAPE_IPI_SGI              1       /* SGI used for cross-CPU signals */

#define GIC_SPURIOUS_IRQ          1023    /* IRQ no. for GIC spurious interrupt */

#define GICR_PE_SIZE              0x40000 /* redistributor stride (live FDT) */

/* GICv3 register definitions; see GICv3 spec (Arm IHI 0069G) */
#define GICD_CTLR                 0x0

/* Non-secure view of GICD_CTLR (DS=0) */
#define GICD_CTLR_ENABLEGRP1      0x1
#define GICD_CTLR_ENABLEGRP1A     0x2
#define GICD_CTLR_ARE_NS          0x10
#define GICD_CTLR_RWP             0x80000000

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100
#define GICR_IPRIORITYR0                        0x10400

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               0x2
#define GICR_WAKER_CHILDRENASLEEP               0x4

#define ICC_CTLR_EOIMODE                        0x2

#define ICC_SRE_SRE                             0x1
/* End of GICv3 register definitions */

#endif /* ! _PEXPERT_ARM64_CAPE_H */
