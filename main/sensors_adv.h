#pragma once
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

// Adapted from AutoRover Sensors.h — pins for our car
#define ADV_TRIG_F PIN_US_TRIG_F
#define ADV_ECHO_F PIN_US_ECHO_F
#define ADV_TRIG_L PIN_US_TRIG_L
#define ADV_ECHO_L PIN_US_ECHO_L
#define ADV_TRIG_R PIN_US_TRIG_R
#define ADV_ECHO_R PIN_US_ECHO_R

enum SensorIdAdv : uint8_t { S_F = 0, S_L = 1, S_R = 2, S_CNT = 3 };

struct UltraAdv {
  gpio_num_t trig, echo;
  volatile int64_t riseUs;
  volatile int64_t widthUs;
  volatile bool ready;
  int64_t pingStartUs;
  bool pending;
  float ring[5];
  uint8_t ringIdx; bool ringFull;
  float distance; float raw;
  uint16_t failStreak; bool healthy;
  int64_t updateUs;
};

static UltraAdv advSensors[S_CNT];
static volatile int8_t advArmed = -1;
static float advTempC = 25.0f;
static inline float advCmPerUs() { return (331.4f + 0.606f * advTempC) / 10000.0f; }

static void IRAM_ATTR advEchoIsr(void* arg) {
  int idx = (int)(intptr_t)arg;
  if (advArmed != idx) return;
  UltraAdv &s = advSensors[idx];
  int lvl = gpio_get_level(s.echo);
  if (lvl) s.riseUs = esp_timer_get_time();
  else if (s.riseUs) {
    int64_t w = esp_timer_get_time() - s.riseUs;
    s.riseUs = 0;
    s.widthUs = w;
    __sync_synchronize();
    s.ready = true;
  }
}

static float advMedian(const float* src, uint8_t n){
  float t[5]; for(int i=0;i<n;i++) t[i]=src[i];
  for(int i=1;i<n;i++){ float k=t[i]; int j=i-1; while(j>=0 && t[j]>k){ t[j+1]=t[j]; j--; } t[j+1]=k; }
  return t[n/2];
}
static void advPush(UltraAdv &s, float cm){
  s.raw=cm;
  s.ring[s.ringIdx]=cm;
  s.ringIdx=(s.ringIdx+1)%5;
  if(s.ringIdx==0) s.ringFull=true;
  uint8_t n=s.ringFull?5:(s.ringIdx?s.ringIdx:1);
  s.distance=advMedian(s.ring,n);
  s.updateUs=esp_timer_get_time();
}
static void advFire(UltraAdv &s, int8_t idx){
  s.ready=false; s.widthUs=0; s.riseUs=0;
  advArmed=idx;
  gpio_set_level(s.trig,0); esp_rom_delay_us(3);
  gpio_set_level(s.trig,1); esp_rom_delay_us(10);
  gpio_set_level(s.trig,0);
  s.pingStartUs=esp_timer_get_time();
  s.pending=true;
}

static const uint8_t ADV_ORDER[4]={S_F,S_L,S_F,S_R};
static uint8_t advOrderIdx=0;
static int64_t advLastFire=0;
static bool advFreshFront=false;

static void advSensorsBegin(){
  struct {gpio_num_t t,e;} map[3]={{(gpio_num_t)ADV_TRIG_F,(gpio_num_t)ADV_ECHO_F},{(gpio_num_t)ADV_TRIG_L,(gpio_num_t)ADV_ECHO_L},{(gpio_num_t)ADV_TRIG_R,(gpio_num_t)ADV_ECHO_R}};
  for(int i=0;i<3;i++){
    UltraAdv &s=advSensors[i];
    s.trig=map[i].t; s.echo=map[i].e;
    gpio_set_level(s.trig,0);
    s.riseUs=0; s.widthUs=0; s.ready=false; s.pending=false; s.pingStartUs=0;
    s.ringIdx=0; s.ringFull=false; for(int k=0;k<5;k++) s.ring[k]=250.0f;
    s.distance=250.0f; s.raw=250.0f; s.failStreak=0; s.healthy=true; s.updateUs=0;
  }
  gpio_install_isr_service(0);
  gpio_isr_handler_add((gpio_num_t)ADV_ECHO_F, advEchoIsr, (void*)(intptr_t)S_F);
  gpio_isr_handler_add((gpio_num_t)ADV_ECHO_L, advEchoIsr, (void*)(intptr_t)S_L);
  gpio_isr_handler_add((gpio_num_t)ADV_ECHO_R, advEchoIsr, (void*)(intptr_t)S_R);
  // ECHO as input with interrupt on change
  for(int i=0;i<3;i++){
    gpio_set_direction(advSensors[i].echo, GPIO_MODE_INPUT);
    gpio_set_pull_mode(advSensors[i].echo, GPIO_FLOATING);
    gpio_set_intr_type(advSensors[i].echo, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(advSensors[i].echo);
  }
}

static void advSensorsService(){
  int64_t now=esp_timer_get_time();
  uint8_t cid=ADV_ORDER[advOrderIdx];
  UltraAdv &cur=advSensors[cid];
  if(cur.pending){
    if(cur.ready){
      int64_t w=cur.widthUs;
      float cm=(w*advCmPerUs())*0.5f;
      if(cm>250.0f) cm=250.0f; else if(cm<2.0f) cm=2.0f;
      advPush(cur,cm);
      cur.failStreak=0; cur.healthy=true; cur.pending=false; advArmed=-1;
      if(cid==S_F) advFreshFront=true;
    } else if(now - cur.pingStartUs > 15200){
      advPush(cur,250.0f);
      cur.pending=false; advArmed=-1;
      if(cur.failStreak<60000) cur.failStreak++;
      if(cur.failStreak>25) cur.healthy=false;
      if(cid==S_F) advFreshFront=true;
    }
  }
  if(!cur.pending && (now - advLastFire >= 14000)){
    advOrderIdx=(advOrderIdx+1)&0x03;
    advLastFire=now;
    uint8_t nid=ADV_ORDER[advOrderIdx];
    advFire(advSensors[nid], nid);
  }
}
static inline float advEff(SensorIdAdv id){ const UltraAdv &s=advSensors[id]; return s.healthy? s.distance : 0.0f; }
