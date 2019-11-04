/**
 * emu2413 v1.0.0-alpha
 * https://github.com/digital-sound-antiques/emu2413
 * Copyright (C) 2001-2019 Mitsutaka Okazaki
 */
#include "emu2413.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPLL_TONE_NUM 3
static uint8_t default_inst[OPLL_TONE_NUM][(16 + 3) * 8] = {
    {
#include "2413tone.h"
    },
    {
#include "vrc7tone.h"
    },
    {
#include "281btone.h"
    } //
};

/* phase increment counter */
#define DP_BITS 18
#define DP_WIDTH (1 << DP_BITS)
#define DP_BASE_BITS (DP_BITS - PG_BITS)

#define wave2_2pi(e) ((e) >> 6)
#define wave2_4pi(e) (wave2_2pi(e) << 1)
#define wave2_8pi(e) (wave2_4pi(e) << 1)

/* dynamic range of envelope output */
#define EG_STEP 0.375
#define EG_BITS 7
#define EG_MUTE ((1 << EG_BITS) - 1)

/* dynamic range of total level */
#define TL_STEP 0.75
#define TL_BITS 6

/* dynamic range of sustine level */
#define SL_STEP 3.0
#define SL_BITS 4

/* dynamic range of operator output */
#define XB_BITS 11

/* dynamic range conversion */
#define EG2XB(d) ((d) << (XB_BITS - EG_BITS))
#define TL2EG(d) ((d) << (EG_BITS - TL_BITS))
#define SL2EG(d) ((d) << (EG_BITS - SL_BITS))

/* envelope phase primary counter */
#define EG_DP_BITS 15

/* sine table phase width */
#define PG_BITS 10
#define PG_WIDTH (1 << PG_BITS)

/* phase increment table */
static uint32_t dphase_table[8 * 512][16];

/* sine table */
static int16_t fullsin_table[PG_WIDTH];
static int16_t halfsin_table[PG_WIDTH];
static int16_t *wave_table_map[2] = {fullsin_table, halfsin_table};

/* log to linear exponential table */
static int16_t exp_table[256];

/* pitch modulator */
#define PM_PG_BITS 3
#define PM_PG_WIDTH (1 << PM_PG_BITS)
#define PM_DP_BITS 22
#define PM_DP_WIDTH (1 << PM_DP_BITS)

/* offset to f-num, approximately ±13.75cents depth. */
static int8_t pm_table[][PM_PG_WIDTH] = {
    {0, 1, 0, 0, 0, 0, 0, 0},    // F-NUM 00xxxxxxx
    {1, 1, 1, 0, -1, -1, -1, 0}, // F-NUM 01xxxxxxx
    {1, 2, 1, 0, -1, -2, -1, 0}, // F-NUM 10xxxxxxx
    {1, 3, 1, 0, -1, -3, -1, 0}, // F-NUM 11xxxxxxx
};

/* Amplitude LFO Table */
/* The following envelop pattern is verified on real YM2413. */
/* Each element is repeated 64 cycles. */
static uint8_t am_table[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,
    3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,
    6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  10,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 12,
    12, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 9,  9,  9,  9,
    9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,
    6,  5,  5,  5,  5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,
    2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0, //
};

/* Envelope secondary increment table. */
static uint8_t eg_incr_map[][4][8] = {
    {
        {0, 1, 0, 1, 0, 1, 0, 1}, // RM 0..12 RL 0
        {0, 1, 1, 1, 0, 1, 0, 1}, // RM 0..12 RL 1
        {0, 1, 1, 1, 0, 1, 1, 1}, // RM 0..12 RL 2
        {0, 1, 1, 1, 1, 1, 1, 1}, // RM 0..12 RL 3
    },
    {
        {1, 1, 1, 1, 1, 1, 1, 1}, // RM 13 RL 0
        {1, 1, 1, 2, 1, 1, 1, 2}, // RM 13 RL 1
        {1, 2, 1, 2, 1, 2, 1, 2}, // RM 13 RL 2
        {1, 2, 2, 2, 1, 2, 2, 2}, // RM 13 RL 3
    },
    {
        {2, 2, 2, 2, 2, 2, 2, 2}, // RM 14 RL 0
        {2, 2, 2, 4, 2, 2, 2, 4}, // RM 14 RL 1
        {2, 4, 2, 4, 2, 4, 2, 4}, // RM 14 RL 2
        {2, 4, 4, 4, 2, 4, 4, 4}, // RM 14 RL 3
    },
};
static uint8_t eg_incr_15[8] = {4, 4, 4, 4, 4, 4, 4, 4}; // RM 15 RL *
static uint8_t eg_incr_0[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // RM 0 RL *

/* [WIP] attach envelope curve table */
/* The following patterns are observed on real YM2413 chip, being different from OPL. */
static uint8_t eg_ar_curve_table[] = {96, 72, 54, 40, 28, 20, 13, 9, 5, 1, 0, 0, 0, 0, 0, 0};

/* empty instrument */
static OPLL_PATCH null_patch = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* ROM instruments */
static OPLL_PATCH default_patch[OPLL_TONE_NUM][(16 + 3) * 2];

/* KSL + TL Table */
static uint32_t tll_table[16][8][1 << TL_BITS][4];
static int32_t rks_table[2][8][2];

/***************************************************

                  Create tables

****************************************************/
static inline int32_t min(int32_t i, int32_t j) {
  if (i < j)
    return i;
  else
    return j;
}

static void makeExpTable(void) {
  int x;
  for (x = 0; x < 256; x++) {
    exp_table[x] = round((pow(2, (double)x / 256) - 1) * 1024);
  }
}

static void makeSinTable(void) {
  for (int x = 0; x < PG_WIDTH / 4; x++) {
    fullsin_table[x] = (int16_t)(-log2(sin((x + 0.5) * PI / (PG_WIDTH / 4) / 2)) * 256) + 1;
    // +1 to distinguish positive and negative zero.
  }

  for (int x = 0; x < PG_WIDTH / 4; x++) {
    fullsin_table[PG_WIDTH / 4 + x] = fullsin_table[PG_WIDTH / 4 - x - 1];
  }

  for (int x = 0; x < PG_WIDTH / 2; x++) {
    fullsin_table[PG_WIDTH / 2 + x] = -fullsin_table[x];
  }

  for (int x = 0; x < PG_WIDTH / 2; x++)
    halfsin_table[x] = fullsin_table[x];

  for (int x = PG_WIDTH / 2; x < PG_WIDTH; x++)
    halfsin_table[x] = fullsin_table[0];
}

static void makeDphaseTable() {
  uint32_t fnum, block, ML;
  uint32_t mltable[16] = {1,     1 * 2, 2 * 2,  3 * 2,  4 * 2,  5 * 2,  6 * 2,  7 * 2,
                          8 * 2, 9 * 2, 10 * 2, 10 * 2, 12 * 2, 12 * 2, 15 * 2, 15 * 2};

  for (fnum = 0; fnum < 512; fnum++)
    for (block = 0; block < 8; block++)
      for (ML = 0; ML < 16; ML++)
        dphase_table[(block << 9) | fnum][ML] = ((fnum * mltable[ML]) << block) >> (20 - DP_BITS);
}

static void makeTllTable(void) {
#define dB2(x) ((x)*2)

  static double kltable[16] = {dB2(0.000),  dB2(9.000),  dB2(12.000), dB2(13.875), dB2(15.000), dB2(16.125),
                               dB2(16.875), dB2(17.625), dB2(18.000), dB2(18.750), dB2(19.125), dB2(19.500),
                               dB2(19.875), dB2(20.250), dB2(20.625), dB2(21.000)};

  int32_t tmp;
  int32_t fnum, block, TL, KL;

  for (fnum = 0; fnum < 16; fnum++)
    for (block = 0; block < 8; block++)
      for (TL = 0; TL < 64; TL++)
        for (KL = 0; KL < 4; KL++) {
          if (KL == 0) {
            tll_table[fnum][block][TL][KL] = TL2EG(TL);
          } else {
            tmp = (int32_t)(kltable[fnum] - dB2(3.000) * (7 - block));
            if (tmp <= 0)
              tll_table[fnum][block][TL][KL] = TL2EG(TL);
            else
              tll_table[fnum][block][TL][KL] = (uint32_t)((tmp >> (3 - KL)) / EG_STEP) + TL2EG(TL);
          }
        }
}

static void makeRksTable(void) {
  for (int fnum8 = 0; fnum8 < 2; fnum8++)
    for (int block = 0; block < 8; block++) {
      rks_table[fnum8][block][1] = (block << 1) + fnum8;
      rks_table[fnum8][block][0] = block >> 1;
    }
}

static void makeDefaultPatch() {
  for (int i = 0; i < OPLL_TONE_NUM; i++)
    for (int j = 0; j < 19; j++)
      OPLL_getDefaultPatch(i, j, &default_patch[i][j * 2]);
}

static uint8_t table_initialized = 0;

static void initializeTables() {
  makeExpTable();
  makeTllTable();
  makeRksTable();
  makeSinTable();
  makeDefaultPatch();
  makeDphaseTable();
  table_initialized = 1;
}

/*********************************************************

                      Synthesizing

*********************************************************/
#define SLOT_BD1 12
#define SLOT_BD2 13
#define SLOT_HH 14
#define SLOT_SD 15
#define SLOT_TOM 16
#define SLOT_CYM 17

/* utility macros */
#define MOD(o, x) (&(o)->slot[(x) << 1])
#define CAR(o, x) (&(o)->slot[((x) << 1) | 1])
#define BIT(s, b) (((s) >> (b)) & 1)

static inline int get_eg_rate(OPLL_SLOT *slot) {
  switch (slot->eg_state) {
  case ATTACK:
    return slot->patch->AR;
  case DECAY:
    return slot->patch->DR;
  case SUSHOLD:
    return 0;
  case SUSTINE:
    return slot->patch->RR;
  case RELEASE:
    if (slot->sustine) {
      return 5;
    } else if (slot->patch->EG) {
      return slot->patch->RR;
    } else {
      return 7;
    }
  case SETTLE:
    return 14;
  case FINISH:
    return 0;
  default:
    return 0;
  }
}

static inline void update_eg_incr(OPLL_SLOT *slot) {
  const int rate = get_eg_rate(slot);
  if (rate == 0) {
    slot->eg_incr_table = eg_incr_0;
    slot->eg_dphase = 0;
    return;
  }

  const int RM = rate + (slot->rks >> 2);
  const int RL = slot->rks & 3;

  if (RM < 13) {
    slot->eg_incr_table = eg_incr_map[0][RL];
    slot->eg_dphase = 1 << (RM + 2);
  } else if (RM == 13) {
    slot->eg_incr_table = eg_incr_map[1][RL];
    slot->eg_dphase = 1 << 15;
  } else if (RM == 14) {
    slot->eg_incr_table = eg_incr_map[2][RL];
    slot->eg_dphase = 1 << 15;
  } else {
    slot->eg_incr_table = eg_incr_15;
    slot->eg_dphase = 1 << 15;
  }
}

#define UPDATE_TLL(S)                                                                                                  \
  (((S)->type == 0) ? ((S)->tll = tll_table[((S)->fnum) >> 5][(S)->block][(S)->patch->TL][(S)->patch->KL])             \
                    : ((S)->tll = tll_table[((S)->fnum) >> 5][(S)->block][(S)->volume][(S)->patch->KL]))
#define UPDATE_RKS(S) (S)->rks = rks_table[((S)->fnum) >> 8][(S)->block][(S)->patch->KR]
#define UPDATE_WF(S) (S)->wave_table = wave_table_map[(S)->patch->WF]
#define UPDATE_EG(S) update_eg_incr(S)
#define UPDATE_ALL(S)                                                                                                  \
  UPDATE_TLL(S);                                                                                                       \
  UPDATE_RKS(S);                                                                                                       \
  UPDATE_WF(S);                                                                                                        \
  UPDATE_EG(S) /* EG should be updated last. */

static void reset_slot(OPLL_SLOT *slot, int number) {
  slot->number = number;
  slot->type = number % 2;
  slot->pg_keep = 0;
  slot->wave_table = wave_table_map[0];
  slot->pg_phase = 0;
  slot->output[0] = 0;
  slot->output[1] = 0;
  slot->feedback = 0;
  slot->eg_incr_index = 0;
  slot->eg_incr_table = eg_incr_0;
  slot->eg_state = FINISH;
  slot->eg_phase = 0;
  slot->eg_dphase = 0;
  slot->rks = 0;
  slot->tll = 0;
  slot->sustine = 0;
  slot->fnum = 0;
  slot->block = 0;
  slot->volume = 0;
  slot->pg_out = 0;
  slot->eg_out = EG_MUTE;
  slot->patch = &null_patch;
}

static inline void slotOn(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  slot->eg_state = SETTLE;
  UPDATE_EG(slot);
}

static inline void slotOff(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  if (slot->type == 1) {
    slot->eg_state = RELEASE;
    UPDATE_EG(slot);
  }
}

static inline void update_key_status(OPLL *opll) {
  const uint8_t r14 = opll->reg[0x0e];
  const uint8_t rhythm_mode = BIT(r14, 5);
  uint32_t new_slot_key_status = 0;

  for (int ch = 0; ch < 9; ch++)
    if (opll->reg[0x20 + ch] & 0x10)
      new_slot_key_status |= 3 << (ch * 2);

  if (rhythm_mode) {
    if (r14 & 0x10)
      new_slot_key_status |= 3 << SLOT_BD1;

    if (r14 & 0x01)
      new_slot_key_status |= 1 << SLOT_HH;

    if (r14 & 0x08)
      new_slot_key_status |= 1 << SLOT_SD;

    if (r14 & 0x04)
      new_slot_key_status |= 1 << SLOT_TOM;

    if (r14 & 0x02)
      new_slot_key_status |= 1 << SLOT_CYM;
  }

  uint32_t updated_status = opll->slot_key_status ^ new_slot_key_status;

  if (updated_status) {
    for (int i = 0; i < 18; i++)
      if (BIT(updated_status, i)) {
        if (BIT(new_slot_key_status, i))
          slotOn(opll, i);
        else
          slotOff(opll, i);
      }
  }

  opll->slot_key_status = new_slot_key_status;
}

static inline void setPatch(OPLL *opll, int32_t i, int32_t num) {
  opll->patch_number[i] = num;
  MOD(opll, i)->patch = &opll->patch[num * 2 + 0];
  CAR(opll, i)->patch = &opll->patch[num * 2 + 1];
}

static inline void setSlotPatch(OPLL_SLOT *slot, OPLL_PATCH *patch) { slot->patch = patch; }

/* Set sustine parameter */
static inline void setSustine(OPLL *opll, int32_t c, int32_t sustine) {
  CAR(opll, c)->sustine = sustine;
  if (MOD(opll, c)->type)
    MOD(opll, c)->sustine = sustine;
}

/* Volume : 6bit ( Volume register << 2 ) */
static inline void setVolume(OPLL *opll, int32_t c, int32_t volume) { CAR(opll, c)->volume = volume; }

static inline void setSlotVolume(OPLL_SLOT *slot, int32_t volume) { slot->volume = volume; }

/* Set F-Number ( fnum : 9bit ) */
static inline void setFnumber(OPLL *opll, int32_t c, int32_t fnum) {
  CAR(opll, c)->fnum = fnum;
  MOD(opll, c)->fnum = fnum;
}

/* Set Block data (block : 3bit ) */
static inline void setBlock(OPLL *opll, int32_t c, int32_t block) {
  CAR(opll, c)->block = block;
  MOD(opll, c)->block = block;
}

static inline void update_rhythm_mode(OPLL *opll) {
  const uint8_t new_rhythm_mode = (opll->reg[0x0e] >> 5) & 1;
  const uint32_t slot_key_status = opll->slot_key_status;

  if (opll->patch_number[6] & 0x10) {
    if (!(BIT(slot_key_status, SLOT_BD2) | new_rhythm_mode)) {
      opll->slot[SLOT_BD1].eg_state = FINISH;
      opll->slot[SLOT_BD2].eg_state = FINISH;
      setPatch(opll, 6, opll->reg[0x36] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[6] = 16;
    opll->slot[SLOT_BD1].eg_state = FINISH;
    opll->slot[SLOT_BD2].eg_state = FINISH;
    setSlotPatch(&opll->slot[SLOT_BD1], &opll->patch[16 * 2 + 0]);
    setSlotPatch(&opll->slot[SLOT_BD2], &opll->patch[16 * 2 + 1]);
  }

  if (opll->patch_number[7] & 0x10) {
    if (!((BIT(slot_key_status, SLOT_HH) && BIT(slot_key_status, SLOT_SD)) | new_rhythm_mode)) {
      opll->slot[SLOT_HH].type = 0;
      opll->slot[SLOT_HH].pg_keep = 0;
      opll->slot[SLOT_HH].eg_state = FINISH;
      opll->slot[SLOT_SD].eg_state = FINISH;
      setPatch(opll, 7, opll->reg[0x37] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[7] = 17;
    opll->slot[SLOT_HH].type = 1;
    opll->slot[SLOT_HH].pg_keep = 1;
    opll->slot[SLOT_HH].eg_state = FINISH;
    opll->slot[SLOT_SD].eg_state = FINISH;
    setSlotPatch(&opll->slot[SLOT_HH], &opll->patch[17 * 2 + 0]);
    setSlotPatch(&opll->slot[SLOT_SD], &opll->patch[17 * 2 + 1]);
  }

  if (opll->patch_number[8] & 0x10) {
    if (!((BIT(slot_key_status, SLOT_CYM) && BIT(slot_key_status, SLOT_TOM)) | new_rhythm_mode)) {
      opll->slot[SLOT_TOM].type = 0;
      opll->slot[SLOT_CYM].pg_keep = 0;
      opll->slot[SLOT_TOM].eg_state = FINISH;
      opll->slot[SLOT_CYM].eg_state = FINISH;
      setPatch(opll, 8, opll->reg[0x38] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[8] = 18;
    opll->slot[SLOT_TOM].type = 1;
    opll->slot[SLOT_CYM].pg_keep = 1;
    opll->slot[SLOT_TOM].eg_state = FINISH;
    opll->slot[SLOT_CYM].eg_state = FINISH;
    setSlotPatch(&opll->slot[SLOT_TOM], &opll->patch[18 * 2 + 0]);
    setSlotPatch(&opll->slot[SLOT_CYM], &opll->patch[18 * 2 + 1]);
  }

  opll->rhythm_mode = new_rhythm_mode;
}

static void update_ampm(OPLL *opll) {
  opll->pm_phase = (opll->pm_phase + opll->pm_dphase) & (PM_DP_WIDTH - 1);
  opll->lfo_am = am_table[(opll->am_phase >> 6) % sizeof(am_table)] << 4;
  opll->am_phase++;
}

static void update_noise(OPLL *opll) {
  if (opll->noise_seed & 1)
    opll->noise_seed ^= 0x8003020;
  opll->noise_seed >>= 1;
  opll->noise = opll->noise_seed & 1;
}

static void update_short_noise(OPLL *opll) {
  const uint32_t pg_hh = opll->slot[SLOT_HH].pg_out;
  const uint32_t pg_cym = opll->slot[SLOT_CYM].pg_out;

  const uint8_t h_bit2 = BIT(pg_hh, PG_BITS - 8);
  const uint8_t h_bit7 = BIT(pg_hh, PG_BITS - 3);
  const uint8_t h_bit3 = BIT(pg_hh, PG_BITS - 7);

  const uint8_t c_bit3 = BIT(pg_cym, PG_BITS - 7);
  const uint8_t c_bit5 = BIT(pg_cym, PG_BITS - 5);

  opll->short_noise = ((h_bit2 ^ h_bit7) | h_bit3) | (c_bit3 ^ c_bit5);
}

static inline void calc_phase(OPLL_SLOT *slot, int32_t pm_phase) {
  const int16_t blk_fnum = ((slot->block << 9) | slot->fnum);
  if (slot->patch->PM) {
    const int8_t pm = pm_table[slot->fnum >> 7][pm_phase >> (PM_DP_BITS - PM_PG_BITS)];
    slot->pg_phase += dphase_table[min(0xfff, blk_fnum + pm)][slot->patch->ML];
  } else {
    slot->pg_phase += dphase_table[blk_fnum][slot->patch->ML];
  }

  slot->pg_phase &= (DP_WIDTH - 1);

  slot->pg_out = slot->pg_phase >> DP_BASE_BITS;
}

static void calc_envelope_cycle(OPLL_SLOT *slot, OPLL_SLOT *slave_slot) {

  slot->eg_incr_index++;

  switch (slot->eg_state) {
  case ATTACK:
    slot->eg_ar_out += slot->eg_incr_table[slot->eg_incr_index % 8];
    slot->eg_out = eg_ar_curve_table[slot->eg_ar_out];
    if ((slot->patch->AR == 15) || slot->eg_out == 0) {
      slot->eg_state = DECAY;
      slot->eg_out = 0;
      UPDATE_EG(slot);
    }
    break;

  case DECAY:
    slot->eg_out += slot->eg_incr_table[slot->eg_incr_index % 8];
    if (slot->eg_out >= SL2EG(slot->patch->SL)) {
      slot->eg_state = slot->patch->EG ? SUSHOLD : SUSTINE;
      slot->eg_out = SL2EG(slot->patch->SL);
      UPDATE_EG(slot);
    }
    break;

  case SUSHOLD:
    if (slot->patch->EG == 0) {
      slot->eg_state = SUSTINE;
      UPDATE_EG(slot);
    }
    break;

  case SUSTINE:
  case RELEASE:
    slot->eg_out += slot->eg_incr_table[slot->eg_incr_index % 8];
    if (slot->eg_out >= EG_MUTE) {
      slot->eg_state = FINISH;
      slot->eg_out = EG_MUTE;
      UPDATE_EG(slot);
    }
    break;

  case SETTLE:
    slot->eg_out += slot->eg_incr_table[slot->eg_incr_index % 8];
    if (slot->eg_out >= EG_MUTE) {
      if (slot->type == 1) {
        slot->eg_state = ATTACK;
        slot->eg_incr_index = 0;
        slot->eg_ar_out = 0;
        slot->eg_out = EG_MUTE;
        slot->pg_phase = slot->pg_keep ? slot->pg_phase : 0;
        UPDATE_EG(slot);

        if (slave_slot) {
          slave_slot->eg_state = ATTACK;
          slave_slot->eg_incr_index = 0;
          slave_slot->eg_ar_out = 0;
          slave_slot->eg_out = EG_MUTE;
          slave_slot->pg_phase = slave_slot->pg_keep ? slave_slot->pg_phase : 0;
          UPDATE_EG(slot);
        }
      }
    }
    break;

  case FINISH:
    slot->eg_out = EG_MUTE;
    break;

  default:
    slot->eg_out = EG_MUTE;
    break;
  }
}

static void calc_envelope(OPLL *opll, OPLL_SLOT *slot, OPLL_SLOT *slave_slot) {
  // assert(slot->eg_dphase <= (1 << EG_DP_BITS));

  slot->eg_phase += slot->eg_dphase;
  if (slot->eg_phase >= (1 << EG_DP_BITS)) {
    calc_envelope_cycle(slot, slave_slot);
    slot->eg_phase -= (1 << EG_DP_BITS);
  }
}

inline static int32_t exp_tbl_ex(int32_t x) {
  const int BASE = 4096; // 4096, 3072, 2304, 2048

  x = BASE - min(x, BASE - 1);

  // int32_t output = (int32_t)round((pow(2, (double)x / 256) - 1) * 1024);
  int32_t output = ((exp_table[x & 0xff] + 1024) << (x >> 8)) - 1024;

  // when BASE = 4096, maximum output is 65344 * 1024.
  return output >> 10;

  // when BASE = 3072, maximum output is 4095 * 1024.
  // return output >> 6;

  // when BASE = 2304, maximum output is 511 * 1024.
  // return output >> 3;

  // when BASE = 2048, maxiumu output is 255 * 1024.
  // return output >> 2;
}

static int32_t to_linear(int32_t h, OPLL_SLOT *slot, uint32_t am) {
  uint32_t value = EG2XB(slot->eg_out + slot->tll) + am;
  if (h >= 0)
    return exp_tbl_ex(h + value);
  else
    return -exp_tbl_ex(-h + value);
}

static inline int32_t calc_slot_car(OPLL *opll, int ch, int32_t fm) {
  OPLL_SLOT *slot = CAR(opll, ch);

  if (slot->eg_state == FINISH)
    return 0;

  int32_t am = slot->patch->AM ? opll->lfo_am : 0;

  return to_linear(slot->wave_table[(slot->pg_out + wave2_8pi(fm)) & (PG_WIDTH - 1)], slot, am);
}

static inline int32_t calc_slot_mod(OPLL *opll, int ch) {
  OPLL_SLOT *slot = MOD(opll, ch);

  if (slot->eg_state == FINISH)
    return 0;

  int32_t fm = slot->patch->FB > 0 ? wave2_4pi(slot->feedback) >> (7 - slot->patch->FB) : 0;
  int32_t am = slot->patch->AM ? opll->lfo_am : 0;

  slot->output[1] = slot->output[0];
  slot->output[0] = to_linear(slot->wave_table[(slot->pg_out + fm) & (PG_WIDTH - 1)], slot, am);
  slot->feedback = (slot->output[1] + slot->output[0]) >> 1;

  return slot->feedback;
}

static inline int32_t calc_slot_tom(OPLL *opll) {
  OPLL_SLOT *slot = MOD(opll, 8);

  if (slot->eg_state == FINISH)
    return 0;

  return to_linear(slot->wave_table[slot->pg_out], slot, 0);
}

/* Specify phase offset directly based on 10-bit (1024-length) sine table */
#define _PD(phase) ((PG_BITS < 10) ? (phase >> (10 - PG_BITS)) : (phase << (PG_BITS - 10)))

static inline int32_t calc_slot_snare(OPLL *opll) {
  OPLL_SLOT *slot = CAR(opll, 7);

  if (slot->eg_state == FINISH)
    return 0;

  uint32_t phase;

  if (BIT(slot->pg_out, PG_BITS - 2))
    phase = opll->noise ? _PD(0x300) : _PD(0x200);
  else
    phase = opll->noise ? _PD(0x0) : _PD(0x100);

  return to_linear(slot->wave_table[phase], slot, 0);
}

static inline int32_t calc_slot_cym(OPLL *opll) {
  OPLL_SLOT *slot = CAR(opll, 8);

  if (slot->eg_state == FINISH)
    return 0;

  uint32_t phase = opll->short_noise ? _PD(0x300) : _PD(0x100);

  return to_linear(slot->wave_table[phase], slot, 0);
}

static inline int32_t calc_slot_hat(OPLL *opll) {
  OPLL_SLOT *slot = MOD(opll, 7);

  if (slot->eg_state == FINISH)
    return 0;

  uint32_t phase;
  if (opll->short_noise)
    phase = opll->noise ? _PD(0x2d0) : _PD(0x234);
  else
    phase = opll->noise ? _PD(0x34) : _PD(0xd0);

  return to_linear(slot->wave_table[phase], slot, 0);
}

#define _MO(x) (x >> 5)
#define _RO(x) (x >> 4)

static void update_output(OPLL *opll) {

  update_ampm(opll);
  update_noise(opll);
  update_short_noise(opll);

  /** update slots */
  for (int i = 0; i < 18; i++) {
    calc_phase(&opll->slot[i], opll->pm_phase);
    if (i < 14 || (opll->reg[0xe] & 32) == 0) {
      calc_envelope(opll, &opll->slot[i], &opll->slot[i - 1]);
    } else {
      calc_envelope(opll, &opll->slot[i], NULL);
    }
  }

  /* CH1-6 */
  for (int i = 0; i < 6; i++)
    if (!(opll->mask & OPLL_MASK_CH(i)))
      opll->ch_out[i] += _MO(calc_slot_car(opll, i, calc_slot_mod(opll, i)));

  /* CH7 */
  if (opll->patch_number[6] <= 15) {
    if (!(opll->mask & OPLL_MASK_CH(6)))
      opll->ch_out[6] += _MO(calc_slot_car(opll, 6, calc_slot_mod(opll, 6)));
  } else {
    if (!(opll->mask & OPLL_MASK_BD))
      opll->ch_out[9] += _RO(calc_slot_car(opll, 6, calc_slot_mod(opll, 6)));
  }

  /* CH8 */
  if (opll->patch_number[7] <= 15) {
    if (!(opll->mask & OPLL_MASK_CH(7)))
      opll->ch_out[7] += _MO(calc_slot_car(opll, 7, calc_slot_mod(opll, 7)));
  } else {
    if (!(opll->mask & OPLL_MASK_HH))
      opll->ch_out[10] += _RO(calc_slot_hat(opll));
    if (!(opll->mask & OPLL_MASK_SD))
      opll->ch_out[11] += _RO(calc_slot_snare(opll));
  }

  /* CH9 */
  if (opll->patch_number[8] <= 15) {
    if (!(opll->mask & OPLL_MASK_CH(8)))
      opll->ch_out[8] += _MO(calc_slot_car(opll, 8, calc_slot_mod(opll, 8)));
  } else {
    if (!(opll->mask & OPLL_MASK_TOM))
      opll->ch_out[12] += _RO(calc_slot_tom(opll));
    if (!(opll->mask & OPLL_MASK_CYM))
      opll->ch_out[13] += _RO(calc_slot_cym(opll));
  }
}

inline static void flush_output(OPLL *opll) {
  int i;
  for (i = 0; i < 15; i++)
    opll->ch_out[i] = 0;
}

inline static void divide_output(OPLL *opll, int div) {
  int i;
  for (i = 0; i < 15; i++)
    opll->ch_out[i] /= div;
}

inline static int32_t mix_output(OPLL *opll) {
  int32_t out = 0;
  int i;

  for (i = 0; i < 15; i++)
    out += opll->ch_out[i];
  return out;
}

inline static void mix_output_stereo(OPLL *opll, int32_t out[2]) {
  int i;
  out[0] = out[1] = 0;
  for (i = 0; i < 15; i++) {
    if (opll->pan[i] & 1)
      out[1] += opll->ch_out[i];
    if (opll->pan[i] & 2)
      out[0] += opll->ch_out[i];
  }
}

inline int16_t interporate_output(OPLL *opll, int16_t next, int16_t prev) {
  return ((double)next * (opll->opll_step - opll->opll_time) + (double)prev * opll->opll_time) / opll->opll_step;
}

/***********************************************************

                   External Interfaces

***********************************************************/

OPLL *OPLL_new(uint32_t clk, uint32_t rate) {
  if (!table_initialized) {
    initializeTables();
  }

  OPLL *opll = (OPLL *)calloc(sizeof(OPLL), 1);
  if (opll == NULL)
    return NULL;

  for (int i = 0; i < 19 * 2; i++)
    memcpy(&opll->patch[i], &null_patch, sizeof(OPLL_PATCH));

  opll->clk = clk;
  opll->rate = rate;
  opll->mask = 0;

  OPLL_reset(opll);
  OPLL_reset_patch(opll, 0);

  return opll;
}

void OPLL_delete(OPLL *opll) { free(opll); }

static void reset_rate_conversion_params(OPLL *opll) {
  opll->real_step = (uint32_t)((1 << 31) / opll->rate);
  opll->opll_step = (uint32_t)((1 << 31) / (opll->clk / 72));
  opll->opll_time = 0;
}

void OPLL_reset(OPLL *opll) {
  if (!opll)
    return;

  opll->adr = 0;
  opll->out = 0;

  opll->pm_phase = 0;
  opll->am_phase = 0;

  opll->noise_seed = 0xffff;
  opll->mask = 0;

  opll->rhythm_mode = 0;
  opll->slot_key_status = 0;

  reset_rate_conversion_params(opll);

  for (int i = 0; i < 18; i++)
    reset_slot(&opll->slot[i], i);

  for (int i = 0; i < 9; i++) {
    setPatch(opll, i, 0);
  }

  for (int i = 0; i < 0x40; i++)
    OPLL_writeReg(opll, i, 0);

  opll->pm_dphase = PM_DP_WIDTH / (1024 * 8);

  for (int i = 0; i < 14; i++)
    opll->pan[i] = 2;

  for (int i = 0; i < 15; i++)
    opll->ch_out[i] = 0;
}

/* Force Refresh (When external program changes some parameters). */
void OPLL_forceRefresh(OPLL *opll) {
  if (opll == NULL)
    return;

  for (int i = 0; i < 9; i++)
    setPatch(opll, i, opll->patch_number[i]);

  for (int i = 0; i < 18; i++) {
    UPDATE_RKS(&opll->slot[i]);
    UPDATE_TLL(&opll->slot[i]);
    UPDATE_WF(&opll->slot[i]);
    UPDATE_EG(&opll->slot[i]);
  }
}

void OPLL_setRate(OPLL *opll, uint32_t rate) {
  opll->rate = rate;
  reset_rate_conversion_params(opll);
}

void OPLL_setQuality(OPLL *opll, uint8_t q) {
  // No Effects.
  // This module always synthesizes output at the same rate with real chip (clock/72).
}

void OPLL_setChipMode(OPLL *opll, uint8_t mode) { opll->vrc7_mode = mode; }

void OPLL_writeReg(OPLL *opll, uint32_t reg, uint32_t data) {
  int32_t i, v, ch;

  data = data & 0xff;
  reg = reg & 0x3f;
  opll->reg[reg] = (uint8_t)data;

  switch (reg) {
  case 0x00:
    opll->patch[0].AM = (data >> 7) & 1;
    opll->patch[0].PM = (data >> 6) & 1;
    opll->patch[0].EG = (data >> 5) & 1;
    opll->patch[0].KR = (data >> 4) & 1;
    opll->patch[0].ML = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_RKS(MOD(opll, i));
        UPDATE_EG(MOD(opll, i));
      }
    }
    break;

  case 0x01:
    opll->patch[1].AM = (data >> 7) & 1;
    opll->patch[1].PM = (data >> 6) & 1;
    opll->patch[1].EG = (data >> 5) & 1;
    opll->patch[1].KR = (data >> 4) & 1;
    opll->patch[1].ML = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_RKS(CAR(opll, i));
        UPDATE_EG(CAR(opll, i));
      }
    }
    break;

  case 0x02:
    opll->patch[0].KL = (data >> 6) & 3;
    opll->patch[0].TL = (data)&63;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_TLL(MOD(opll, i));
      }
    }
    break;

  case 0x03:
    opll->patch[1].KL = (data >> 6) & 3;
    opll->patch[1].WF = (data >> 4) & 1;
    opll->patch[0].WF = (data >> 3) & 1;
    opll->patch[0].FB = (data)&7;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_WF(MOD(opll, i));
        UPDATE_WF(CAR(opll, i));
      }
    }
    break;

  case 0x04:
    opll->patch[0].AR = (data >> 4) & 15;
    opll->patch[0].DR = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_EG(MOD(opll, i));
      }
    }
    break;

  case 0x05:
    opll->patch[1].AR = (data >> 4) & 15;
    opll->patch[1].DR = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_EG(CAR(opll, i));
      }
    }
    break;

  case 0x06:
    opll->patch[0].SL = (data >> 4) & 15;
    opll->patch[0].RR = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_EG(MOD(opll, i));
      }
    }
    break;

  case 0x07:
    opll->patch[1].SL = (data >> 4) & 15;
    opll->patch[1].RR = (data)&15;
    for (i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        UPDATE_EG(CAR(opll, i));
      }
    }
    break;

  case 0x0e:
    if (opll->vrc7_mode)
      break;

    update_rhythm_mode(opll);
    update_key_status(opll);

    UPDATE_ALL(MOD(opll, 6));
    UPDATE_ALL(CAR(opll, 6));
    UPDATE_ALL(MOD(opll, 7));
    UPDATE_ALL(CAR(opll, 7));
    UPDATE_ALL(MOD(opll, 8));
    UPDATE_ALL(CAR(opll, 8));

    break;

  case 0x0f:
    break;

  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
    ch = reg - 0x10;
    setFnumber(opll, ch, data + ((opll->reg[0x20 + ch] & 1) << 8));
    UPDATE_ALL(MOD(opll, ch));
    UPDATE_ALL(CAR(opll, ch));
    break;

  case 0x20:
  case 0x21:
  case 0x22:
  case 0x23:
  case 0x24:
  case 0x25:
  case 0x26:
  case 0x27:
  case 0x28:
    ch = reg - 0x20;
    setFnumber(opll, ch, ((data & 1) << 8) + opll->reg[0x10 + ch]);
    setBlock(opll, ch, (data >> 1) & 7);
    setSustine(opll, ch, (data >> 5) & 1);
    UPDATE_ALL(MOD(opll, ch));
    UPDATE_ALL(CAR(opll, ch));
    update_key_status(opll);
    update_rhythm_mode(opll);
    break;

  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
  case 0x38:
    i = (data >> 4) & 15;
    v = data & 15;
    if ((opll->reg[0x0e] & 32) && (reg >= 0x36)) {
      switch (reg) {
      case 0x37:
        setSlotVolume(MOD(opll, 7), i << 2);
        break;
      case 0x38:
        setSlotVolume(MOD(opll, 8), i << 2);
        break;
      default:
        break;
      }
    } else {
      setPatch(opll, reg - 0x30, i);
    }
    setVolume(opll, reg - 0x30, v << 2);
    UPDATE_ALL(MOD(opll, reg - 0x30));
    UPDATE_ALL(CAR(opll, reg - 0x30));
    break;

  default:
    break;
  }
}

void OPLL_writeIO(OPLL *opll, uint32_t adr, uint32_t val) {
  if (adr & 1)
    OPLL_writeReg(opll, opll->adr, val);
  else
    opll->adr = val;
}

void OPLL_setPan(OPLL *opll, uint32_t ch, uint32_t pan) { opll->pan[ch & 15] = pan & 3; }

void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch) {
  patch[0].AM = (dump[0] >> 7) & 1;
  patch[1].AM = (dump[1] >> 7) & 1;
  patch[0].PM = (dump[0] >> 6) & 1;
  patch[1].PM = (dump[1] >> 6) & 1;
  patch[0].EG = (dump[0] >> 5) & 1;
  patch[1].EG = (dump[1] >> 5) & 1;
  patch[0].KR = (dump[0] >> 4) & 1;
  patch[1].KR = (dump[1] >> 4) & 1;
  patch[0].ML = (dump[0]) & 15;
  patch[1].ML = (dump[1]) & 15;
  patch[0].KL = (dump[2] >> 6) & 3;
  patch[1].KL = (dump[3] >> 6) & 3;
  patch[0].TL = (dump[2]) & 63;
  patch[0].FB = (dump[3]) & 7;
  patch[0].WF = (dump[3] >> 3) & 1;
  patch[1].WF = (dump[3] >> 4) & 1;
  patch[0].AR = (dump[4] >> 4) & 15;
  patch[1].AR = (dump[5] >> 4) & 15;
  patch[0].DR = (dump[4]) & 15;
  patch[1].DR = (dump[5]) & 15;
  patch[0].SL = (dump[6] >> 4) & 15;
  patch[1].SL = (dump[7] >> 4) & 15;
  patch[0].RR = (dump[6]) & 15;
  patch[1].RR = (dump[7]) & 15;
}

void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *patch) {
  OPLL_dump2patch(default_inst[type] + num * 8, patch);
}

void OPLL_setPatch(OPLL *opll, const uint8_t *dump) {
  OPLL_PATCH patch[2];
  for (int i = 0; i < 19; i++) {
    OPLL_dump2patch(dump + i * 8, patch);
    memcpy(&opll->patch[i * 2 + 0], &patch[0], sizeof(OPLL_PATCH));
    memcpy(&opll->patch[i * 2 + 1], &patch[1], sizeof(OPLL_PATCH));
  }
}

void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump) {
  dump[0] = (uint8_t)((patch[0].AM << 7) + (patch[0].PM << 6) + (patch[0].EG << 5) + (patch[0].KR << 4) + patch[0].ML);
  dump[1] = (uint8_t)((patch[1].AM << 7) + (patch[1].PM << 6) + (patch[1].EG << 5) + (patch[1].KR << 4) + patch[1].ML);
  dump[2] = (uint8_t)((patch[0].KL << 6) + patch[0].TL);
  dump[3] = (uint8_t)((patch[1].KL << 6) + (patch[1].WF << 4) + (patch[0].WF << 3) + patch[0].FB);
  dump[4] = (uint8_t)((patch[0].AR << 4) + patch[0].DR);
  dump[5] = (uint8_t)((patch[1].AR << 4) + patch[1].DR);
  dump[6] = (uint8_t)((patch[0].SL << 4) + patch[0].RR);
  dump[7] = (uint8_t)((patch[1].SL << 4) + patch[1].RR);
}

void OPLL_copyPatch(OPLL *opll, int32_t num, OPLL_PATCH *patch) {
  memcpy(&opll->patch[num], patch, sizeof(OPLL_PATCH));
}

void OPLL_resetPatch(OPLL *opll, int32_t type) {
  for (int i = 0; i < 19 * 2; i++)
    OPLL_copyPatch(opll, i, &default_patch[type % OPLL_TONE_NUM][i]);
}

int16_t OPLL_calc(OPLL *opll) {
  int count = 0;
  int16_t next = 0, prev = 0;

  prev = mix_output(opll);

  if (opll->real_step > opll->opll_time) {
    flush_output(opll);
    while (opll->real_step > opll->opll_time) {
      opll->opll_time += opll->opll_step;
      update_output(opll);
      count++;
    }
    divide_output(opll, count);
  }

  opll->opll_time -= opll->real_step;
  next = mix_output(opll);
  opll->out = interporate_output(opll, next, prev);
  return opll->out;
}

void OPLL_calcStereo(OPLL *opll, int32_t out[2]) {
  int count = 0;
  int32_t prev[2], next[2];

  mix_output_stereo(opll, prev);

  if (opll->real_step > opll->opll_time) {
    flush_output(opll);
    while (opll->real_step > opll->opll_time) {
      opll->opll_time += opll->opll_step;
      update_output(opll);
      count++;
    }
    divide_output(opll, count);
  }
  opll->opll_time -= opll->real_step;

  mix_output_stereo(opll, next);
  out[0] = interporate_output(opll, next[0], prev[0]);
  out[1] = interporate_output(opll, next[1], prev[1]);
}

uint32_t OPLL_setMask(OPLL *opll, uint32_t mask) {
  uint32_t ret;

  if (opll) {
    ret = opll->mask;
    opll->mask = mask;
    return ret;
  } else
    return 0;
}

uint32_t OPLL_toggleMask(OPLL *opll, uint32_t mask) {
  uint32_t ret;

  if (opll) {
    ret = opll->mask;
    opll->mask ^= mask;
    return ret;
  } else
    return 0;
}
