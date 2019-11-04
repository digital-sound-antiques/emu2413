#ifndef _EMU2413_H_
#define _EMU2413_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI 3.14159265358979323846

typedef enum __OPLL_EG_STATE { ATTACK, DECAY, SUSHOLD, SUSTINE, RELEASE, FINISH, SETTLE } OPLL_EG_STATE;

enum OPLL_TONE_ENUM { OPLL_2413_TONE = 0, OPLL_VRC7_TONE = 1, OPLL_281B_TONE = 2 };

/* voice data */
typedef struct __OPLL_PATCH {
  uint32_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WF;
} OPLL_PATCH;

/* slot */
typedef struct __OPLL_SLOT {

  OPLL_PATCH *patch;
  uint8_t number;
  int32_t type; /* 0 : modulator 1 : carrier */

  /* OUTPUT */
  int32_t feedback;
  int32_t output[2]; /* Output value of slot */

  /* for Phase Generator (PG) */
  int16_t *wave_table; /* Wavetable */
  uint32_t pg_phase;   /* Phase */
  uint32_t pg_out;     /* output */
  uint8_t pg_keep;     /* preserve phase when key off */
  int32_t fnum;        /* F-Number */
  int32_t block;       /* Block */

  /* for Envelope Generator (EG) */
  OPLL_EG_STATE eg_state; /* Current state */
  int32_t volume;         /* Current volume */
  uint8_t sustine;        /* Sustine 1 = ON, 0 = OFF */
  uint32_t tll;           /* Total Level + Key scale level*/
  uint32_t rks;           /* Key scale offset (Rks) */
  uint32_t eg_phase;      /* Phase */
  uint32_t eg_dphase;     /* Phase increment amount */
  uint8_t eg_incr_index;
  uint8_t *eg_incr_table;
  uint8_t eg_ar_out;
  uint32_t eg_out; /* output */

} OPLL_SLOT;

/* Mask */
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

  uint8_t vrc7_mode;

  uint32_t adr;
  int32_t out;

  uint32_t real_step;
  uint32_t opll_time;
  uint32_t opll_step;

  uint32_t pan[16];

  /* Register */
  uint8_t reg[0x40];
  uint32_t slot_key_status;
  uint8_t rhythm_mode;

  /* Pitch Modulator */
  uint32_t pm_phase;
  uint32_t pm_dphase;

  /* Amp Modulator */
  int32_t am_phase;
  int32_t am_dphase;
  int32_t lfo_am;

  /* Noise Generator */
  uint32_t noise_seed;
  uint8_t noise;
  uint8_t short_noise;

  /* Channel Data */
  int32_t patch_number[9];

  /* Slot */
  OPLL_SLOT slot[18];

  /* Voice Data */
  OPLL_PATCH patch[19 * 2];
  int32_t patch_update[2]; /* flag for check patch update */

  uint32_t mask;

  /* Output of each channels
   * 0-8:TONE
   * 9:BD 10:HH 11:SD, 12:TOM, 13:CYM
   * 14:Reserved for DAC
   */
  int16_t ch_out[15];

} OPLL;

/* Create Object */
OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);

/* Setup */
void OPLL_reset(OPLL *);
void OPLL_resetPatch(OPLL *, int32_t);
void OPLL_setRate(OPLL *opll, uint32_t r);
void OPLL_setQuality(OPLL *opll, uint8_t q);
void OPLL_setPan(OPLL *, uint32_t ch, uint32_t pan);
void OPLL_setChipMode(OPLL *opll, uint8_t mode);

/* Port/Register access */
void OPLL_writeIO(OPLL *, uint32_t reg, uint32_t val);
void OPLL_writeReg(OPLL *, uint32_t reg, uint32_t val);

/* Synthsize */
int16_t OPLL_calc(OPLL *);
void OPLL_calcStereo(OPLL *, int32_t out[2]);

/* Misc */
void OPLL_setPatch(OPLL *, const uint8_t *dump);
void OPLL_copyPatch(OPLL *, int32_t, OPLL_PATCH *);
void OPLL_forceRefresh(OPLL *);

/* Utility */
void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch);
void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump);
void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *);

/* Channel Mask */
uint32_t OPLL_setMask(OPLL *, uint32_t mask);
uint32_t OPLL_toggleMask(OPLL *, uint32_t mask);

/* For compatibility */
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
