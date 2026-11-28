/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6328 PMIC interrupt controller glue (see drivers/mfd/mt6328-irq.c) */

#ifndef __MFD_MT6328_IRQ_H__
#define __MFD_MT6328_IRQ_H__

struct mt6397_chip;

int mt6328_irq_init(struct mt6397_chip *chip);
void mt6328_irq_exit(struct mt6397_chip *chip);

#endif
