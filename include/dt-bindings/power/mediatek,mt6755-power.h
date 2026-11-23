/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * MT6755 SCPSYS power domain IDs.
 * Target path: include/dt-bindings/power/mediatek,mt6755-power.h
 *
 * Only DISP is wired up for the m681 display bring-up.  The other stock
 * MTCMOS domains (clk-mt6755-pg.c: MD1, C2K, CONN, MFG/MFG_ASYNC, ISP, VDE,
 * VEN, AUDIO, IFR, MDSYS_INTF_INFRA) can be added later using the same
 * register recipe.
 */
#ifndef _DT_BINDINGS_POWER_MT6755_POWER_H
#define _DT_BINDINGS_POWER_MT6755_POWER_H

#define MT6755_POWER_DOMAIN_DISP	0
#define MT6755_POWER_DOMAIN_MFG_ASYNC	1
#define MT6755_POWER_DOMAIN_MFG		2

#endif /* _DT_BINDINGS_POWER_MT6755_POWER_H */
