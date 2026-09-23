#pragma once
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// C version — no namespace
#define ADV_BINS 4
#define ADV_N_STATES 64
#define ADV_N_ACTIONS 6
#define ADV_N_ESC_CTX 16
#define ADV_N_ESCAPES 4

enum { ADV_FWD=0, ADV_SOFT_L, ADV_SOFT_R, ADV_PIVOT_L, ADV_PIVOT_R, ADV_BACK };

static float advQ[64][6];
static float advEps = 0.35f;
static uint32_t advLastSaveMs = 0;
static uint16_t advEscTries[16][4];
static uint16_t advEscWins[16][4];
static uint32_t advEscTotal[16];
static float advDBlock = 30.0f;
static uint8_t advSpdCruise = 15;
static uint32_t advStuckFreeze = 1400;
static uint32_t advWinStartMs = 0;
static uint32_t advWinColl = 0, advWinStuck = 0, advWinDistCm = 0;

static inline uint8_t advBinOf(float cm){
  if(cm < 15) return 0;
  if(cm < 30) return 1;
  if(cm < 60) return 2;
  return 3;
}
static inline uint8_t advStateOf(float f, float l, float r){
  uint8_t fb=advBinOf(f), lb=advBinOf(l), rb=advBinOf(r);
  return (fb<<4) | (lb<<2) | rb;
}
static inline bool advSafeMask(uint8_t state, uint8_t act, float f, float l, float r){
  if(act==ADV_SOFT_L || act==ADV_PIVOT_L){ if(l < 12) return false; }
  if(act==ADV_SOFT_R || act==ADV_PIVOT_R){ if(r < 12) return false; }
  if(act==ADV_FWD){ if(f < 15) return false; }
  return true;
}
static inline void advNvsLoad(void){
  nvs_handle_t h;
  if(nvs_open("adv_brain", NVS_READONLY, &h)!=ESP_OK) return;
  size_t sz=sizeof(advQ);
  nvs_get_blob(h,"Q",advQ,&sz);
  sz=sizeof(advEscTries); nvs_get_blob(h,"escT",advEscTries,&sz);
  sz=sizeof(advEscWins); nvs_get_blob(h,"escW",advEscWins,&sz);
  sz=sizeof(advEscTotal); nvs_get_blob(h,"escN",advEscTotal,&sz);
  uint32_t e=0;
  if(nvs_get_u32(h,"eps",&e)==ESP_OK) memcpy(&advEps,&e,4);
  nvs_close(h);
  ESP_LOGI("learn","NVS loaded eps %.3f", advEps);
}
static inline void advNvsSave(uint32_t now){
  if(now - advLastSaveMs < 60000) return;
  advLastSaveMs=now;
  nvs_handle_t h;
  if(nvs_open("adv_brain", NVS_READWRITE, &h)!=ESP_OK) return;
  nvs_set_blob(h,"Q",advQ,sizeof(advQ));
  nvs_set_blob(h,"escT",advEscTries,sizeof(advEscTries));
  nvs_set_blob(h,"escW",advEscWins,sizeof(advEscWins));
  nvs_set_blob(h,"escN",advEscTotal,sizeof(advEscTotal));
  uint32_t e; memcpy(&e,&advEps,4);
  nvs_set_u32(h,"eps",e);
  nvs_commit(h); nvs_close(h);
  ESP_LOGI("learn","NVS saved");
}
static inline void advBegin(void){
  memset(advQ,0,sizeof(advQ));
  memset(advEscTries,0,sizeof(advEscTries));
  memset(advEscWins,0,sizeof(advEscWins));
  memset(advEscTotal,0,sizeof(advEscTotal));
  advEps=0.35f;
  nvs_flash_init();
  advNvsLoad();
  advWinStartMs=0;
}
static inline uint8_t advPickAction(uint8_t state, float f, float l, float r){
  bool explore = ((float)esp_random()/4294967295.0f) < advEps;
  uint8_t best=0; float bestQ=-1e9;
  uint8_t cand[6]; uint8_t cn=0;
  for(uint8_t a=0;a<6;a++) if(advSafeMask(state,a,f,l,r)) cand[cn++]=a;
  if(cn==0) return ADV_BACK;
  if(explore) return cand[esp_random()%cn];
  for(uint8_t i=0;i<cn;i++){ uint8_t a=cand[i]; if(advQ[state][a] > bestQ){ bestQ=advQ[state][a]; best=a; } }
  return best;
}
static inline void advUpdateQ(uint8_t s, uint8_t a, float reward, uint8_t s2){
  float maxQ=-1e9; for(int i=0;i<6;i++) if(advQ[s2][i]>maxQ) maxQ=advQ[s2][i];
  if(maxQ==-1e9) maxQ=0;
  advQ[s][a] += 0.20f * (reward + 0.90f*maxQ - advQ[s][a]);
  if(advEps > 0.05f) advEps *= 0.9993f;
}
static inline void advAdaptiveTick(uint32_t now, uint32_t distCm, bool coll, bool stuck){
  if(!advWinStartMs) advWinStartMs=now;
  advWinDistCm += distCm;
  if(coll) advWinColl++;
  if(stuck) advWinStuck++;
  if(now - advWinStartMs >= 60000){
    if(advWinColl>=2){ advDBlock+=4; if(advDBlock>60) advDBlock=60; if(advSpdCruise>8) advSpdCruise-=2; }
    else     if(advWinColl==0 && advWinStuck==0 && advWinDistCm>300){ if(advSpdCruise<20) advSpdCruise++; advDBlock-=1; if(advDBlock<24) advDBlock=24; }
    if(advWinStuck>=2){ if(advStuckFreeze>900) advStuckFreeze-=200; advDBlock+=3; }
    else if(advWinStuck==0 && advWinDistCm>200){ if(advStuckFreeze<2200) advStuckFreeze+=100; }
    ESP_LOGI("learn","adapt dBlock=%.1f spd=%d freeze=%d coll=%d stuck=%d dist=%d",advDBlock,advSpdCruise,advStuckFreeze,advWinColl,advWinStuck,advWinDistCm);
    advWinStartMs=now; advWinColl=advWinStuck=advWinDistCm=0;
    advNvsSave(now);
  }
}
