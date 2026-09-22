#pragma once
// board.h — physical pin assignment for this controller board.
//
// Split out of shared.h: these are consumed by Core 1 only (the RS485 driver),
// and a pin map is a property of the board, not of either core.

// ─── Pins ──────────────────────────────────────────────────────────────────────
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6
