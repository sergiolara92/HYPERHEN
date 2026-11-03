// SPDX-License-Identifier: GPL-3.0-or-later
// TacHen publication ABI only; no dashboard implementation is imported.
#pragma once
#include <stdint.h>
#define TAC_DECK_MAX_PATCHES 10
typedef struct { uint64_t address; uint8_t before[14],after[14]; } TacDeckPatch;
