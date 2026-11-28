/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Meizu Linux mainline project
 *
 * SoC descriptions for the MediaTek CONSYS controller.
 *
 * Register offsets are the ones used by the vendor conn_soc driver for each
 * SoC:
 *   MT6735 family : mt6735/include/mtk_wcn_consys_hw.h
 *   MT6797        : mt6797/include/mtk_wcn_consys_hw.h
 *   MT6750/MT6755 : same topckgen/SPM layout as MT6735, the CONN2AP mask
 *                   register is at the MT6735 offset as well.
 */

#ifndef _MTK_CONNSYS_HW_H
#define _MTK_CONNSYS_HW_H

#include "connsys.h"

int connsys_btif_chip_reset(struct connsys_dev *cd);

#endif /* _MTK_CONNSYS_HW_H */
