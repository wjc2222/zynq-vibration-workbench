/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __CONFIG_ZYNQ_AX7Z020_H
#define __CONFIG_ZYNQ_AX7Z020_H
/* Native ax7z020.env supplies the complete board-specific environment.
 * Suppress the generic header's legacy BOOTENV and conflicting RAM addresses.
 */
#define CFG_EXTRA_ENV_SETTINGS ""
#include <configs/zynq-common.h>
#endif
