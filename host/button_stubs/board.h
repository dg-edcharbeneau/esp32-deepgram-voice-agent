/* Host stub: the one board fact boot_button.c reads. The S3's pin; the logic
 * under test does not depend on which. Found ahead of main/board.h, which
 * needs a real IDF target and BSP. */
#pragma once
#define BOARD_BOOT_GPIO 0
