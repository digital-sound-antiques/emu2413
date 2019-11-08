/**
 * emu2413 v1.0.0-alpha4
 * https://github.com/digital-sound-antiques/emu2413
 * Copyright (C) 2019 Mitsutaka Okazaki
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
#define DP_BITS 19
#define DP_WIDTH (1 << DP_BITS)
#define DP_BASE_BITS (DP_BITS - PG_BITS)

#define wave2_2pi(e) (e >> 2)
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

/* damper speed before key-on. key-scale affects. */
#define DAMPER_RATE 12

#define TL2EG(d) ((d) << (EG_BITS - TL_BITS))
#define SL2EG(d) ((d) << (EG_BITS - SL_BITS))

/* envelope phase counter size */
#define EG_DP_BITS 15

/* sine table */
#define PG_BITS 10 /* 2^10 = 1024 length sine table */
#define PG_WIDTH (1 << PG_BITS)
static uint16_t fullsin_table[PG_WIDTH];
static uint16_t halfsin_table[PG_WIDTH];
static uint16_t *wave_table_map[2] = {fullsin_table, halfsin_table};

/* log to linear exponential table (similar to OPLx) */
static uint16_t exp_table[256];

/* pitch modulator */
#define PM_PG_BITS 3
#define PM_PG_WIDTH (1 << PM_PG_BITS)
#define PM_DP_BITS 22
#define PM_DP_WIDTH (1 << PM_DP_BITS)

/* offset to fnum, rough approximation of 14 cents depth. */
static int8_t pm_table[8][PM_PG_WIDTH] = {
    {0, 0, 0, 0, 0, 0, 0, 0},    // fnum = 000xxxxx
    {0, 0, 1, 0, 0, 0, -1, 0},   // fnum = 001xxxxx
    {0, 1, 2, 1, 0, -1, -2, -1}, // fnum = 010xxxxx
    {0, 1, 3, 1, 0, -1, -3, -1}, // fnum = 011xxxxx
    {0, 2, 4, 2, 0, -2, -4, -2}, // fnum = 100xxxxx
    {0, 2, 5, 2, 0, -2, -5, -2}, // fnum = 101xxxxx
    {0, 3, 6, 3, 0, -3, -6, -3}, // fnum = 110xxxxx
    {0, 3, 7, 3, 0, -3, -7, -3}, // fnum = 111xxxxx
};

/* amplitude lfo table */
/* The following envelop pattern is verified on real YM2413. */
/* each element repeates 64 cycles */
static uint8_t am_table[210] = {0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  //
                                2,  2,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,  3,  //
                                4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  //
                                6,  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  //
                                8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  //
                                10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, //
                                12, 12, 12, 12, 12, 12, 12, 12,                                 //
                                13, 13, 13,                                                     //
                                12, 12, 12, 12, 12, 12, 12, 12,                                 //
                                11, 11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, //
                                9,  9,  9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  8,  8,  //
                                7,  7,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  //
                                5,  5,  5,  5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  //
                                3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  2,  2,  2,  2,  2,  2,  //
                                1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0};

/* envelope decay increment step table */
/* based on andete's research */
static uint8_t eg_step_tables[4][16] = {
    {0, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 0, 1, 1, 1, 0, 1, 0},
    {0, 1, 1, 1, 0, 1, 1, 1, 0},
    {0, 1, 1, 1, 1, 1, 1, 1, 0},
};

static uint32_t ml_table[16] = {1,     1 * 2, 2 * 2,  3 * 2,  4 * 2,  5 * 2,  6 * 2,  7 * 2,
                                8 * 2, 9 * 2, 10 * 2, 10 * 2, 12 * 2, 12 * 2, 15 * 2, 15 * 2};

#define dB2(x) ((x)*2)
static double kl_table[16] = {dB2(0.000),  dB2(9.000),  dB2(12.000), dB2(13.875), dB2(15.000), dB2(16.125),
                              dB2(16.875), dB2(17.625), dB2(18.000), dB2(18.750), dB2(19.125), dB2(19.500),
                              dB2(19.875), dB2(20.250), dB2(20.625), dB2(21.000)};

static uint32_t tll_table[8 * 16][1 << TL_BITS][4];
static int32_t rks_table[8 * 2][2];

static OPLL_PATCH null_patch = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static OPLL_PATCH default_patch[OPLL_TONE_NUM][(16 + 3) * 2];

/***************************************************

                  Create tables

****************************************************/
#define min(i, j) (((i) < (j)) ? (i) : (j))
#define max(i, j) (((i) > (j)) ? (i) : (j))

static void makeExpTable(void) {
  int x;
  for (x = 0; x < 256; x++) {
    exp_table[x] = round((exp2((double)x / 256.0) - 1) * 1024);
  }
}

static void makeSinTable(void) {
  for (int x = 0; x < PG_WIDTH / 4; x++) {
    fullsin_table[x] = (uint16_t)(-log2(sin((x + 0.5) * M_PI / (PG_WIDTH / 4) / 2)) * 256);
  }

  for (int x = 0; x < PG_WIDTH / 4; x++) {
    fullsin_table[PG_WIDTH / 4 + x] = fullsin_table[PG_WIDTH / 4 - x - 1];
  }

  for (int x = 0; x < PG_WIDTH / 2; x++) {
    fullsin_table[PG_WIDTH / 2 + x] = 0x8000 | fullsin_table[x];
  }

  for (int x = 0; x < PG_WIDTH / 2; x++)
    halfsin_table[x] = 0xfff;

  for (int x = PG_WIDTH / 2; x < PG_WIDTH; x++)
    halfsin_table[x] = fullsin_table[x];
}

static void makeTllTable(void) {

  int32_t tmp;
  int32_t fnum, block, TL, KL;

  for (fnum = 0; fnum < 16; fnum++) {
    for (block = 0; block < 8; block++) {
      for (TL = 0; TL < 64; TL++) {
        for (KL = 0; KL < 4; KL++) {
          if (KL == 0) {
            tll_table[(block << 4) | fnum][TL][KL] = TL2EG(TL);
          } else {
            tmp = (int32_t)(kl_table[fnum] - dB2(3.000) * (7 - block));
            if (tmp <= 0)
              tll_table[(block << 4) | fnum][TL][KL] = TL2EG(TL);
            else
              tll_table[(block << 4) | fnum][TL][KL] = (uint32_t)((tmp >> (3 - KL)) / EG_STEP) + TL2EG(TL);
          }
        }
      }
    }
  }
}

static void makeRksTable(void) {
  for (int fnum8 = 0; fnum8 < 2; fnum8++)
    for (int block = 0; block < 8; block++) {
      rks_table[(block << 1) | fnum8][1] = (block << 1) + fnum8;
      rks_table[(block << 1) | fnum8][0] = block >> 1;
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

#define OPLL_DEBUG 0

#if OPLL_DEBUG
static void _debug_print_patch(OPLL_SLOT *slot) {
  OPLL_PATCH *p = slot->patch;
  printf("[slot#%d am:%d pm:%d eg:%d kr:%d ml:%d kl:%d tl:%d wf:%d fb:%d A:%d D:%d S:%d R:%d]\n", slot->number, //
         p->AM, p->PM, p->EG, p->KR, p->ML,                                                                     //
         p->KL, p->TL, p->WF, p->FB,                                                                            //
         p->AR, p->DR, p->SL, p->RR);
}

static char *_debug_eg_state_name(OPLL_SLOT *slot) {
  switch (slot->eg_state) {
  case ATTACK:
    return "attack";
  case DECAY:
    return "decay";
  case SUSTAIN:
    return "sustain";
  case RELEASE:
    return "release";
  case DAMP:
    return "damp";
  default:
    return "unknown";
  }
}

static inline void _debug_print_slot_info(OPLL_SLOT *slot) {
  char *name = _debug_eg_state_name(slot);
  printf("[slot#%d state:%s fnum:%03x rate:%d]\n", slot->number, name, slot->blk_fnum, slot->eg_rate);
  _debug_print_patch(slot);
  fflush(stdout);
}
#endif

static inline int get_parameter_rate(OPLL_SLOT *slot) {
  switch (slot->eg_state) {
  case ATTACK:
    return slot->patch->AR;
  case DECAY:
    return slot->patch->DR;
  case SUSTAIN:
    return slot->patch->EG ? 0 : slot->patch->RR;
  case RELEASE:
    if (slot->sus_flag) {
      return 5;
    } else if (slot->patch->EG) {
      return slot->patch->RR;
    } else {
      return 7;
    }
  case DAMP:
    return DAMPER_RATE;
  default:
    return 0;
  }
}

enum SLOT_UPDATE_FLAG {
  UPDATE_WF = 1,
  UPDATE_TLL = 2,
  UPDATE_RKS = 4,
  UPDATE_EG = 8,
  UPDATE_ALL = 255,
};

static inline void request_update(OPLL_SLOT *slot, int flag) { slot->update_requests |= flag; }

static void commit_slot_update(OPLL_SLOT *slot) {

#if OPLL_DEBUG
  if (slot->last_eg_state != slot->eg_state) {
    _debug_print_slot_info(slot);
    slot->last_eg_state = slot->eg_state;
  }
#endif

  if (slot->update_requests & UPDATE_WF) {
    slot->wave_table = wave_table_map[slot->patch->WF];
  }

  if (slot->update_requests & UPDATE_TLL) {
    if (slot->type == 0) {
      slot->tll = tll_table[slot->blk_fnum >> 5][slot->patch->TL][slot->patch->KL];
    } else {
      slot->tll = tll_table[slot->blk_fnum >> 5][slot->volume][slot->patch->KL];
    }
  }

  if (slot->update_requests & UPDATE_RKS) {
    slot->rks = rks_table[slot->blk_fnum >> 8][slot->patch->KR];
  }

  if (slot->update_requests & (UPDATE_RKS | UPDATE_EG)) {
    int p_rate = get_parameter_rate(slot);
    slot->eg_rate = (0 < p_rate) ? min(63, (p_rate << 2) + slot->rks) : 0;
    const int RM = slot->eg_rate >> 2;
    if (slot->eg_state == ATTACK) {
      slot->eg_shift = (0 < RM && RM < 12) ? (13 - RM) : 0;
    } else {
      slot->eg_shift = (RM < 13) ? (13 - RM) : 0;
    }
  }

  slot->update_requests = 0;
}

static void reset_slot(OPLL_SLOT *slot, int number) {
  slot->number = number;
  slot->type = number % 2;
  slot->pg_keep = 0;
  slot->wave_table = wave_table_map[0];
  slot->pg_phase = 0;
  slot->output[0] = 0;
  slot->output[1] = 0;
  slot->feedback = 0;
  slot->eg_state = RELEASE;
  slot->eg_shift = 0;
  slot->rks = 0;
  slot->tll = 0;
  slot->sus_flag = 0;
  slot->blk_fnum = 0;
  slot->blk = 0;
  slot->fnum = 0;
  slot->volume = 0;
  slot->pg_out = 0;
  slot->eg_out = EG_MUTE;
  slot->patch = &null_patch;
}

static inline void slotOn(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  slot->eg_state = DAMP;
  request_update(slot, UPDATE_EG);
}

static inline void slotOff(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  if (slot->type == 1) {
    slot->eg_state = RELEASE;
    request_update(slot, UPDATE_EG);
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
        if (BIT(new_slot_key_status, i)) {
          slotOn(opll, i);
        } else {
          slotOff(opll, i);
        }
      }
  }

  opll->slot_key_status = new_slot_key_status;
}

static inline void set_patch(OPLL *opll, int32_t ch, int32_t num) {
  opll->patch_number[ch] = num;
  MOD(opll, ch)->patch = &opll->patch[num * 2 + 0];
  CAR(opll, ch)->patch = &opll->patch[num * 2 + 1];
  request_update(MOD(opll, ch), UPDATE_ALL);
  request_update(CAR(opll, ch), UPDATE_ALL);
}

static inline void set_slot_patch(OPLL_SLOT *slot, OPLL_PATCH *patch) {
  slot->patch = patch;
  request_update(slot, UPDATE_ALL);
}

static inline void set_sus_flag(OPLL *opll, int ch, int flag) {
  CAR(opll, ch)->sus_flag = flag;
  request_update(CAR(opll, ch), UPDATE_EG);
  if (MOD(opll, ch)->type) {
    MOD(opll, ch)->sus_flag = flag;
    request_update(MOD(opll, ch), UPDATE_EG);
  }
}

/* set volume ( volume : 6bit, register value << 2 ) */
static inline void set_volume(OPLL *opll, int ch, int volume) {
  CAR(opll, ch)->volume = volume;
  request_update(CAR(opll, ch), UPDATE_TLL);
}

static inline void set_slot_volume(OPLL_SLOT *slot, int volume) {
  slot->volume = volume;
  request_update(slot, UPDATE_TLL);
}

/* set f-Nnmber ( fnum : 9bit ) */
static inline void set_fnumber(OPLL *opll, int ch, int fnum) {
  OPLL_SLOT *car = CAR(opll, ch);
  OPLL_SLOT *mod = MOD(opll, ch);
  car->fnum = fnum;
  car->blk_fnum = (car->blk_fnum & 0xe00) | (fnum & 0x1ff);
  mod->fnum = fnum;
  mod->blk_fnum = (mod->blk_fnum & 0xe00) | (fnum & 0x1ff);
  request_update(car, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  request_update(mod, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
}

/* set block data (blk : 3bit ) */
static inline void set_block(OPLL *opll, int ch, int blk) {
  OPLL_SLOT *car = CAR(opll, ch);
  OPLL_SLOT *mod = MOD(opll, ch);
  car->blk = blk;
  car->blk_fnum = ((blk & 7) << 9) | (car->blk_fnum & 0x1ff);
  mod->blk = blk;
  mod->blk_fnum = ((blk & 7) << 9) | (mod->blk_fnum & 0x1ff);
  request_update(car, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  request_update(mod, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
}

static inline void update_rhythm_mode(OPLL *opll) {
  const uint8_t new_rhythm_mode = (opll->reg[0x0e] >> 5) & 1;
  const uint32_t slot_key_status = opll->slot_key_status;

  if (opll->patch_number[6] & 0x10) {
    if (!(BIT(slot_key_status, SLOT_BD2) | new_rhythm_mode)) {
      opll->slot[SLOT_BD1].eg_state = RELEASE;
      opll->slot[SLOT_BD1].eg_out = EG_MUTE;
      opll->slot[SLOT_BD2].eg_state = RELEASE;
      opll->slot[SLOT_BD2].eg_out = EG_MUTE;
      set_patch(opll, 6, opll->reg[0x36] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[6] = 16;
    opll->slot[SLOT_BD1].eg_state = RELEASE;
    opll->slot[SLOT_BD1].eg_out = EG_MUTE;
    opll->slot[SLOT_BD2].eg_state = RELEASE;
    opll->slot[SLOT_BD2].eg_out = EG_MUTE;
    set_slot_patch(&opll->slot[SLOT_BD1], &opll->patch[16 * 2 + 0]);
    set_slot_patch(&opll->slot[SLOT_BD2], &opll->patch[16 * 2 + 1]);
  }

  if (opll->patch_number[7] & 0x10) {
    if (!((BIT(slot_key_status, SLOT_HH) && BIT(slot_key_status, SLOT_SD)) | new_rhythm_mode)) {
      opll->slot[SLOT_HH].type = 0;
      opll->slot[SLOT_HH].pg_keep = 0;
      opll->slot[SLOT_HH].eg_state = RELEASE;
      opll->slot[SLOT_HH].eg_out = EG_MUTE;
      opll->slot[SLOT_SD].eg_state = RELEASE;
      opll->slot[SLOT_SD].eg_out = EG_MUTE;
      set_patch(opll, 7, opll->reg[0x37] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[7] = 17;
    opll->slot[SLOT_HH].type = 1;
    opll->slot[SLOT_HH].pg_keep = 1;
    opll->slot[SLOT_HH].eg_state = RELEASE;
    opll->slot[SLOT_HH].eg_out = EG_MUTE;
    opll->slot[SLOT_SD].eg_state = RELEASE;
    opll->slot[SLOT_SD].eg_out = EG_MUTE;
    set_slot_patch(&opll->slot[SLOT_HH], &opll->patch[17 * 2 + 0]);
    set_slot_patch(&opll->slot[SLOT_SD], &opll->patch[17 * 2 + 1]);
  }

  if (opll->patch_number[8] & 0x10) {
    if (!((BIT(slot_key_status, SLOT_CYM) && BIT(slot_key_status, SLOT_TOM)) | new_rhythm_mode)) {
      opll->slot[SLOT_TOM].type = 0;
      opll->slot[SLOT_CYM].pg_keep = 0;
      opll->slot[SLOT_TOM].eg_state = RELEASE;
      opll->slot[SLOT_TOM].eg_out = EG_MUTE;
      opll->slot[SLOT_CYM].eg_state = RELEASE;
      opll->slot[SLOT_CYM].eg_out = EG_MUTE;
      set_patch(opll, 8, opll->reg[0x38] >> 4);
    }
  } else if (new_rhythm_mode) {
    opll->patch_number[8] = 18;
    opll->slot[SLOT_TOM].type = 1;
    opll->slot[SLOT_CYM].pg_keep = 1;
    opll->slot[SLOT_TOM].eg_state = RELEASE;
    opll->slot[SLOT_TOM].eg_out = EG_MUTE;
    opll->slot[SLOT_CYM].eg_state = RELEASE;
    opll->slot[SLOT_CYM].eg_out = EG_MUTE;
    set_slot_patch(&opll->slot[SLOT_TOM], &opll->patch[18 * 2 + 0]);
    set_slot_patch(&opll->slot[SLOT_CYM], &opll->patch[18 * 2 + 1]);
  }

  opll->rhythm_mode = new_rhythm_mode;
}

static void update_ampm(OPLL *opll) {
  opll->pm_phase = (opll->pm_phase + opll->pm_dphase) & (PM_DP_WIDTH - 1);
  opll->lfo_am = am_table[(opll->am_phase >> 6) % sizeof(am_table)];
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

  const uint8_t h_bit1 = BIT(pg_hh, PG_BITS - 8);
  const uint8_t h_bit6 = BIT(pg_hh, PG_BITS - 3);
  const uint8_t h_bit2 = BIT(pg_hh, PG_BITS - 7);

  const uint8_t c_bit2 = BIT(pg_cym, PG_BITS - 7);
  const uint8_t c_bit4 = BIT(pg_cym, PG_BITS - 5);

  opll->short_noise = ((h_bit1 ^ h_bit6) | h_bit2) | (c_bit2 ^ c_bit4);
}

static inline void calc_phase(OPLL_SLOT *slot, int32_t pm_phase) {
  const int8_t pm = slot->patch->PM ? pm_table[(slot->fnum >> 6) & 7][pm_phase >> (PM_DP_BITS - PM_PG_BITS)] : 0;
  slot->pg_phase += (((slot->fnum & 0x1ff) * 2 + pm) * ml_table[slot->patch->ML]) << slot->blk >> 2;
  slot->pg_phase &= (DP_WIDTH - 1);
  slot->pg_out = slot->pg_phase >> DP_BASE_BITS;
}

static inline uint8_t lookup_attack_step(OPLL_SLOT *slot, uint32_t counter) {
  int index;

  switch (slot->eg_rate >> 2) {
  case 12:
    index = (counter & 0xc) >> 1;
    return 4 - eg_step_tables[slot->eg_rate & 3][index];
  case 13:
    index = (counter & 0xc) >> 1;
    return 3 - eg_step_tables[slot->eg_rate & 3][index];
  case 14:
    index = (counter & 0xc) >> 1;
    return 2 - eg_step_tables[slot->eg_rate & 3][index];
  case 0:
  case 15:
    return 0;
  default:
    index = counter >> slot->eg_shift;
    return eg_step_tables[slot->eg_rate & 3][index & 7] ? 4 : 0;
  }
}

static inline uint8_t lookup_decay_step(OPLL_SLOT *slot, uint32_t counter) {
  int index;

  switch (slot->eg_rate >> 2) {
  case 13:
    index = ((counter & 0xc) >> 1) | (counter & 1);
    return eg_step_tables[slot->eg_rate & 3][index];
  case 14:
    index = ((counter & 0xc) >> 1);
    return eg_step_tables[slot->eg_rate & 3][index] + 1;
  case 15:
    return 2;
  default:
    index = counter >> slot->eg_shift;
    return eg_step_tables[slot->eg_rate & 3][index & 7];
  }
}

static inline void calc_envelope(OPLL_SLOT *slot, OPLL_SLOT *slave_slot, uint16_t eg_counter) {

  uint32_t mask = (1 << slot->eg_shift) - 1;
  uint8_t s;

  switch (slot->eg_state) {
  case ATTACK:
    if ((eg_counter & mask & ~3) == 0) {
      s = lookup_attack_step(slot, eg_counter);
      if (0 < s) {
        slot->eg_out = max(0, ((int)slot->eg_out - (slot->eg_out >> s) - 1));
      }
    }
    if (((slot->eg_rate >> 2) == 15) || slot->eg_out == 0) {
      slot->eg_state = DECAY;
      slot->eg_out = 0;
      request_update(slot, UPDATE_EG);
    }
    break;

  case DECAY:
    if ((eg_counter & mask) == 0) {
      slot->eg_out += lookup_decay_step(slot, eg_counter);
    }
    if (slot->eg_out >= SL2EG(slot->patch->SL)) {
      slot->eg_state = SUSTAIN;
      slot->eg_out = SL2EG(slot->patch->SL);
      request_update(slot, UPDATE_EG);
    }
    break;

  case SUSTAIN:
  case RELEASE:
    if (slot->eg_rate == 0 || slot->eg_out >= EG_MUTE) {
      break; // avoid useless calculation for performance
    }
    if ((eg_counter & mask) == 0) {
      slot->eg_out += lookup_decay_step(slot, eg_counter);
    }
    if (slot->eg_out > EG_MUTE) {
      slot->eg_out = EG_MUTE;
    }
    break;

  case DAMP:
    if (slot->eg_out < EG_MUTE) {
      if ((eg_counter & mask) == 0) {
        slot->eg_out += lookup_decay_step(slot, eg_counter);
      }
    }

    if (slot->eg_out >= EG_MUTE) {
      slot->eg_out = EG_MUTE;
      if (slot->type == 1) {
        slot->eg_state = ATTACK;
        slot->pg_phase = slot->pg_keep ? slot->pg_phase : 0;
        request_update(slot, UPDATE_EG);
        if (slave_slot) {
          slave_slot->eg_state = ATTACK;
          slave_slot->eg_out = EG_MUTE;
          slave_slot->pg_phase = slave_slot->pg_keep ? slave_slot->pg_phase : 0;
          request_update(slave_slot, UPDATE_EG);
        }
      }
    }
    break;

  default:
    slot->eg_out = EG_MUTE;
    break;
  }
}

static void update_slots(OPLL *opll) {

  opll->eg_counter++;

  for (int i = 0; i < 18; i++) {
    OPLL_SLOT *slot = &opll->slot[i];
    if (slot->update_requests) {
      commit_slot_update(slot);
    }
    calc_phase(slot, opll->pm_phase);

    OPLL_SLOT *slave_slot = (i < 14 || (opll->reg[0xe] & 32) == 0) ? &opll->slot[i - 1] : NULL;
    calc_envelope(slot, slave_slot, opll->eg_counter);
  }
}

/* output: -4095...4095 */
static inline int16_t lookup_exp_table(uint16_t i) {
  /* from andete's expressoin */
  int16_t t = (exp_table[(i & 0xff) ^ 0xff] + 1024);
  int16_t res = t >> ((i & 0x7f00) >> 8);
  return ((i & 0x8000) ? ~res : res) << 1;
}

static inline int16_t to_linear(uint16_t h, OPLL_SLOT *slot, int16_t am) {
  uint16_t att = min(127, (slot->eg_out + slot->tll + am)) << 4;
  return lookup_exp_table(h + att);
}

static inline int16_t calc_slot_car(OPLL *opll, int ch, int16_t fm) {
  OPLL_SLOT *slot = CAR(opll, ch);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;

  return to_linear(slot->wave_table[(slot->pg_out + wave2_8pi(fm)) & (PG_WIDTH - 1)], slot, am);
}

static inline int16_t calc_slot_mod(OPLL *opll, int ch) {
  OPLL_SLOT *slot = MOD(opll, ch);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  int16_t fm = slot->patch->FB > 0 ? wave2_4pi(slot->feedback) >> (7 - slot->patch->FB) : 0;
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;

  slot->output[1] = slot->output[0];
  slot->output[0] = to_linear(slot->wave_table[(slot->pg_out + fm) & (PG_WIDTH - 1)], slot, am);
  slot->feedback = (slot->output[1] + slot->output[0]) >> 1;

  return slot->feedback;
}

static inline int16_t calc_slot_tom(OPLL *opll) {
  OPLL_SLOT *slot = MOD(opll, 8);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  return to_linear(slot->wave_table[slot->pg_out], slot, 0);
}

/* Specify phase offset directly based on 10-bit (1024-length) sine table */
#define _PD(phase) ((PG_BITS < 10) ? (phase >> (10 - PG_BITS)) : (phase << (PG_BITS - 10)))

static inline int16_t calc_slot_snare(OPLL *opll) {
  OPLL_SLOT *slot = CAR(opll, 7);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  uint32_t phase;

  if (BIT(slot->pg_out, PG_BITS - 2))
    phase = opll->noise ? _PD(0x300) : _PD(0x200);
  else
    phase = opll->noise ? _PD(0x0) : _PD(0x100);

  return to_linear(slot->wave_table[phase], slot, 0);
}

static inline int16_t calc_slot_cym(OPLL *opll) {
  OPLL_SLOT *slot = CAR(opll, 8);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  uint32_t phase = opll->short_noise ? _PD(0x300) : _PD(0x100);

  return to_linear(slot->wave_table[phase], slot, 0);
}

static inline int16_t calc_slot_hat(OPLL *opll) {
  OPLL_SLOT *slot = MOD(opll, 7);

  if (slot->eg_out >= EG_MUTE) {
    return 0;
  }

  uint32_t phase;
  if (opll->short_noise)
    phase = opll->noise ? _PD(0x2d0) : _PD(0x234);
  else
    phase = opll->noise ? _PD(0x34) : _PD(0xd0);

  return to_linear(slot->wave_table[phase], slot, 0);
}

#define _MO(x) (x >> 1)
#define _RO(x) (x)

static void update_output(OPLL *opll) {

  update_ampm(opll);
  update_noise(opll);
  update_short_noise(opll);
  update_slots(opll);

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
  for (int i = 0; i < 15; i++)
    opll->ch_out[i] = 0;
}

inline static void divide_output(OPLL *opll, int div) {
  for (int i = 0; i < 15; i++)
    opll->ch_out[i] /= div;
}

inline static int32_t mix_output(OPLL *opll) {
  int32_t out = 0;
  for (int i = 0; i < 15; i++)
    out += opll->ch_out[i];
  return out;
}

inline static void mix_output_stereo(OPLL *opll, int32_t out[2]) {
  out[0] = out[1] = 0;
  for (int i = 0; i < 15; i++) {
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
  opll->eg_counter = 0;

  reset_rate_conversion_params(opll);

  for (int i = 0; i < 18; i++)
    reset_slot(&opll->slot[i], i);

  for (int i = 0; i < 9; i++) {
    set_patch(opll, i, 0);
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

  for (int i = 0; i < 9; i++) {
    set_patch(opll, i, opll->patch_number[i]);
  }

  for (int i = 0; i < 18; i++) {
    request_update(&opll->slot[i], UPDATE_ALL);
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

/* mode = 0: opll 1: vrc7 */
void OPLL_setChipMode(OPLL *opll, uint8_t mode) { opll->chip_mode = mode; }

void OPLL_writeReg(OPLL *opll, uint32_t reg, uint32_t data) {
  int32_t ch;

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
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(MOD(opll, i), UPDATE_RKS | UPDATE_EG);
      }
    }
    break;

  case 0x01:
    opll->patch[1].AM = (data >> 7) & 1;
    opll->patch[1].PM = (data >> 6) & 1;
    opll->patch[1].EG = (data >> 5) & 1;
    opll->patch[1].KR = (data >> 4) & 1;
    opll->patch[1].ML = (data)&15;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(CAR(opll, i), UPDATE_RKS | UPDATE_EG);
      }
    }
    break;

  case 0x02:
    opll->patch[0].KL = (data >> 6) & 3;
    opll->patch[0].TL = (data)&63;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(MOD(opll, i), UPDATE_TLL);
      }
    }
    break;

  case 0x03:
    opll->patch[1].KL = (data >> 6) & 3;
    opll->patch[1].WF = (data >> 4) & 1;
    opll->patch[0].WF = (data >> 3) & 1;
    opll->patch[0].FB = (data)&7;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(MOD(opll, i), UPDATE_WF);
        request_update(CAR(opll, i), UPDATE_WF | UPDATE_TLL);
      }
    }
    break;

  case 0x04:
    opll->patch[0].AR = (data >> 4) & 15;
    opll->patch[0].DR = (data)&15;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(MOD(opll, i), UPDATE_EG);
      }
    }
    break;

  case 0x05:
    opll->patch[1].AR = (data >> 4) & 15;
    opll->patch[1].DR = (data)&15;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(CAR(opll, i), UPDATE_EG);
      }
    }
    break;

  case 0x06:
    opll->patch[0].SL = (data >> 4) & 15;
    opll->patch[0].RR = (data)&15;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(MOD(opll, i), UPDATE_EG);
      }
    }
    break;

  case 0x07:
    opll->patch[1].SL = (data >> 4) & 15;
    opll->patch[1].RR = (data)&15;
    for (int i = 0; i < 9; i++) {
      if (opll->patch_number[i] == 0) {
        request_update(CAR(opll, i), UPDATE_EG);
      }
    }
    break;

  case 0x0e:
    if (opll->chip_mode == 1)
      break;
    update_rhythm_mode(opll);
    update_key_status(opll);
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
    set_fnumber(opll, ch, data + ((opll->reg[0x20 + ch] & 1) << 8));
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
    set_fnumber(opll, ch, ((data & 1) << 8) + opll->reg[0x10 + ch]);
    set_block(opll, ch, (data >> 1) & 7);
    set_sus_flag(opll, ch, (data >> 5) & 1);
    update_key_status(opll);
    /* update rhythm mode here because key-off of rhythm instrument is deferred until key-on bit is down. */
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
    if ((opll->reg[0x0e] & 32) && (reg >= 0x36)) {
      switch (reg) {
      case 0x37:
        set_slot_volume(MOD(opll, 7), ((data >> 4) & 15) << 2);
        break;
      case 0x38:
        set_slot_volume(MOD(opll, 8), ((data >> 4) & 15) << 2);
        break;
      default:
        break;
      }
    } else {
      set_patch(opll, reg - 0x30, (data >> 4) & 15);
    }
    set_volume(opll, reg - 0x30, (data & 15) << 2);
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
  patch[1].TL = 0;
  patch[0].FB = (dump[3]) & 7;
  patch[1].FB = 0;
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
