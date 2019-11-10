#ifndef _EMU2413_H_
#define _EMU2413_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum __OPLL_EG_STATE { ATTACK, DECAY, SUSTAIN, RELEASE, DAMP } OPLL_EG_STATE;

enum OPLL_TONE_ENUM { OPLL_2413_TONE = 0, OPLL_VRC7_TONE = 1, OPLL_281B_TONE = 2 };

/* voice data */
typedef struct __OPLL_PATCH {
  uint32_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WF;
} OPLL_PATCH;

/* slot */
typedef struct __OPLL_SLOT {
  uint8_t number;
  int32_t type; /* 0:modulator 1:carrier */

  OPLL_PATCH *patch; /* voice parameter */

  /* slot output */
  int32_t feedback;  /* amount of feedback */
  int32_t output[2]; /* output value, latest and previous. */

  /* phase generator (pg) */
  uint16_t *wave_table; /* wave table */
  uint32_t pg_phase;    /* pg phase */
  uint32_t pg_out;      /* pg output, as index of wave table */
  uint8_t pg_keep;      /* if 1, pg_phase is preserved when key-on */
  uint16_t blk_fnum;    /* (block << 9) | f-number */
  uint16_t fnum;        /* f-number (9 bits) */
  uint8_t blk;          /* block (3 bits) */

  /* envelope generator (eg) */
  uint8_t eg_state;         /* current state */
  int32_t volume;           /* current volume */
  uint8_t sus_flag;         /* key-sus option 1:on 0:off */
  uint16_t tll;             /* total level + key scale level*/
  uint8_t rks;              /* key scale offset (rks) for eg speed */
  uint8_t eg_rate_h;        /* eg speed rate high 4bits */
  uint8_t eg_rate_l;        /* eg speed rate low 2bits */
  uint32_t eg_shift;        /* shift for eg global counter, controls envelope speed */
  uint32_t eg_out;          /* eg output */

  uint8_t last_eg_state;

  uint32_t update_requests; /* flags to debounce update */
} OPLL_SLOT;

/* mask */
#define OPLL_MASK_CH(x) (1 << (x))
#define OPLL_MASK_HH (1 << (9))
#define OPLL_MASK_CYM (1 << (10))
#define OPLL_MASK_TOM (1 << (11))
#define OPLL_MASK_SD (1 << (12))
#define OPLL_MASK_BD (1 << (13))
#define OPLL_MASK_RHYTHM (OPLL_MASK_HH | OPLL_MASK_CYM | OPLL_MASK_TOM | OPLL_MASK_SD | OPLL_MASK_BD)

/* opll */
typedef struct __OPLL {
  uint32_t clk;
  uint32_t rate;

  uint8_t chip_mode;

  uint32_t adr;
  int32_t out;

  uint32_t real_step;
  uint32_t opll_time;
  uint32_t opll_step;

  uint8_t reg[0x40];
  uint32_t slot_key_status;
  uint8_t rhythm_mode;

  uint32_t eg_counter; /* global counter for envelope */

  uint32_t pm_phase;
  uint32_t pm_dphase;

  int32_t am_phase;
  int32_t am_dphase;
  uint8_t lfo_am;

  uint32_t noise_seed;
  uint8_t noise;
  uint8_t short_noise;

  int32_t patch_number[9];
  OPLL_SLOT slot[18];
  OPLL_PATCH patch[19 * 2];
  int32_t patch_update[2]; /* flag for check patch update */

  uint32_t pan[16];
  uint32_t mask;

  /* output of each channels
   * 0-8:TONE
   * 9:BD 10:HH 11:SD, 12:TOM, 13:CYM
   * 14:reserved for dac
   */
  int32_t ch_out[15];

} OPLL;

OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);

void OPLL_reset(OPLL *);
void OPLL_resetPatch(OPLL *, int32_t);
void OPLL_setRate(OPLL *opll, uint32_t r);
void OPLL_setQuality(OPLL *opll, uint8_t q);
void OPLL_setPan(OPLL *, uint32_t ch, uint32_t pan);
void OPLL_setChipMode(OPLL *opll, uint8_t mode); /* mode:0 ym2413, mode:1 vrc7 */

void OPLL_writeIO(OPLL *, uint32_t reg, uint32_t val);
void OPLL_writeReg(OPLL *, uint32_t reg, uint32_t val);

int16_t OPLL_calc(OPLL *);
void OPLL_calcStereo(OPLL *, int32_t out[2]);

void OPLL_setPatch(OPLL *, const uint8_t *dump);
void OPLL_copyPatch(OPLL *, int32_t, OPLL_PATCH *);
void OPLL_forceRefresh(OPLL *);
void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch);
void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump);
void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *);

uint32_t OPLL_setMask(OPLL *, uint32_t mask);
uint32_t OPLL_toggleMask(OPLL *, uint32_t mask);

/* for compatibility */
#define OPLL_set_rate OPLL_setRate
#define OPLL_set_quality OPLL_setQuality
#define OPLL_set_pan OPLL_setPan
#define OPLL_calc_stereo OPLL_calcStereo
#define OPLL_reset_patch OPLL_resetPatch
#define OPLL_dump2patch OPLL_dumpToPatch
#define OPLL_patch2dump OPLL_patchToDump

#ifdef __cplusplus
}
#endif

#endif
