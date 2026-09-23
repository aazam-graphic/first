#include "tft_game.h"
#include "tft_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG="GAME";

#define W 320
#define H 240
#define ROAD_TOP_W 100
#define ROAD_BOT_W 280
#define ROAD_TOP_Y 20
#define ROAD_BOT_Y 230
#define ROAD_XC (W/2)
#define CAR_W 18
#define CAR_H 28
#define ENEMY_W 18
#define ENEMY_H 28

typedef struct { int x,y; bool alive; uint16_t col; } enemy_t;

static bool s_active=false;
static int s_player_x = W/2;
static int s_speed = 3;
static int s_score=0;
static int s_lives=3;
static int s_frame=0;
static int s_road_anim=0;
static enemy_t s_enemies[4];
static bool s_crashed=false;
static uint32_t s_crash_t=0;

// font for fb (reuse from tft_display - duplicate tiny 5x7)
extern const uint8_t s_font5x7_dummy; // not used, we use tft_draw helpers via fb

bool game_is_active(void){ return s_active; }
void game_toggle(void){
    s_active = !s_active;
    if(s_active){ game_init(); ESP_LOGI(TAG,"GAME START"); }
    else ESP_LOGI(TAG,"GAME EXIT -> CAR MODE");
}
void game_init(void){
    s_player_x = W/2; s_speed=3; s_score=0; s_lives=3; s_frame=0; s_road_anim=0;
    s_crashed=false; s_crash_t=0;
    for(int i=0;i<4;i++){ s_enemies[i].alive=false; s_enemies[i].y=-100; s_enemies[i].x=W/2; }
    s_enemies[0].alive=true; s_enemies[0].y=-60; s_enemies[0].x=ROAD_XC + ((esp_random()%3)-1)*50; s_enemies[0].col=TFT_RED;
}

// framebuffer helpers (draw into s_fb, then single push)
static uint16_t *fb(void){ return tft_get_fb(); }
static inline void fb_set(int x,int y,uint16_t c){
    if(x<0||x>=W||y<0||y>=H) return;
    fb()[y*W + x]=c;
}
static void fb_fill(uint16_t c){
    uint16_t *f=fb(); if(!f) return;
    for(int i=0;i<W*H;i++) f[i]=c;
}
static void fb_rect(int x,int y,int w,int h,uint16_t c){
    if(!fb()) return;
    if(x<0){ w+=x; x=0; }
    if(y<0){ h+=y; y=0; }
    if(x+w>W) w=W-x;
    if(y+h>H) h=H-y;
    if(w<=0||h<=0) return;
    for(int yy=y; yy<y+h; yy++){
        uint16_t *row = fb()+ yy*W + x;
        for(int xx=0; xx<w; xx++) row[xx]=c;
    }
}
static void fb_char(int x,int y,char ch,uint16_t fg,uint16_t bg,int scale){
    if(ch<32||ch>126) ch='?';
    // 5x7 font copied
    static const uint8_t font[][5]={
     {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
     {0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
     {0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
     {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
     {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
     {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},
     {0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
     {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
     {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},
     {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
     {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
     {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},{0x02,0x04,0x08,0x10,0x20},
     {0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},{0x00,0x03,0x05,0x00,0x00},{0x20,0x54,0x54,0x54,0x78},
     {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},
     {0x0C,0x52,0x52,0x52,0x3E},{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
     {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
     {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
     {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},
     {0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},{0x78,0x46,0x46,0x46,0x31}
    };
    const uint8_t *glyph = font[ch-32];
    for(int col=0;col<5;col++){
        uint8_t bits=glyph[col];
        for(int row=0;row<7;row++){
            bool on = bits & (1<<row);
            uint16_t c = on?fg:bg;
            for(int sy=0;sy<scale;sy++) for(int sx=0;sx<scale;sx++) fb_set(x+col*scale+sx, y+row*scale+sy, c);
        }
    }
    for(int sy=0;sy<7*scale;sy++) for(int sx=0;sx<scale;sx++) fb_set(x+5*scale+sx, y+sy, bg);
}
static void fb_text(int x,int y,const char *s,uint16_t fg,uint16_t bg,int scale){
    int cx=x;
    while(*s){
        if(*s=='\n'){ y+=8*scale; cx=x; s++; continue; }
        fb_char(cx,y,*s,fg,bg,scale);
        cx+=6*scale; s++;
    }
}
static void fb_car(int x,int y,uint16_t col){
    int ox=x-CAR_W/2, oy=y-CAR_H/2;
    fb_rect(ox+2, oy+CAR_H-4, CAR_W-4, 4, RGB565(30,30,30));
    fb_rect(ox, oy+6, CAR_W, CAR_H-10, col);
    fb_rect(ox+4, oy+10, CAR_W-8, 8, RGB565(30,80,200));
    fb_rect(ox-2, oy+8, 4, 8, TFT_BLACK);
    fb_rect(ox+CAR_W-2, oy+8, 4, 8, TFT_BLACK);
    fb_rect(ox-2, oy+CAR_H-12, 4, 8, TFT_BLACK);
    fb_rect(ox+CAR_W-2, oy+CAR_H-12, 4, 8, TFT_BLACK);
    fb_rect(ox+4, oy, CAR_W-8, 4, TFT_YELLOW);
    fb_rect(ox+4, oy+CAR_H-3, CAR_W-8, 3, TFT_RED);
}

void game_update(const xbox360_pad_t *pad, uint32_t now_ms){
    if(!s_active) return;
    s_frame++;
    if(s_crashed){
        if(now_ms - s_crash_t > 700){
            s_crashed=false;
            if(s_lives<=0){ game_init(); return; }
        } else return;
    }
    int steer=0;
    float accel=0;
    if(pad && pad->present){
        steer = pad->lx;
        if(abs(steer)<3500) steer=0;
        int dx = steer / 2800; // -11..11
        if(pad->lt > 30 || pad->rt > 30){
            // only move if accelerating/braking
        }
        s_player_x += dx;
        accel = pad->rt/255.0f - pad->lt/255.0f*0.5f;
        s_speed = 2 + (int)(accel*4);
        if(s_speed<1) s_speed=1;
        if(s_speed>6) s_speed=6;
    } else {
        // demo drift
        s_player_x = W/2 + (s_frame%100<50? 8:-8);
        s_speed=3;
    }
    // clamp to road at player y
    {
        float t = (float)(260 - ROAD_TOP_Y)/(ROAD_BOT_Y - ROAD_TOP_Y);
        float w = ROAD_TOP_W + (ROAD_BOT_W-ROAD_TOP_W)*t;
        int left = ROAD_XC - (int)(w/2);
        int right= ROAD_XC + (int)(w/2);
        if(s_player_x < left+CAR_W/2+3) s_player_x = left+CAR_W/2+3;
        if(s_player_x > right-CAR_W/2-3) s_player_x = right-CAR_W/2-3;
    }
    s_road_anim = (s_road_anim + s_speed) % 20;

    // enemies
    for(int i=0;i<4;i++){
        if(!s_enemies[i].alive){
            if((esp_random()%50)==0){
                s_enemies[i].alive=true;
                int lane = (esp_random()%3)-1;
                int base = ROAD_XC + lane*45 + (esp_random()%12)-6;
                s_enemies[i].x = base;
                s_enemies[i].y = -40;
                uint16_t cols[4]={TFT_RED,TFT_YELLOW,TFT_CYAN,TFT_MAGENTA};
                s_enemies[i].col = cols[esp_random()%4];
            }
        } else {
            s_enemies[i].y += s_speed + 2;
            // clamp to road width at that y
            if(s_enemies[i].y >= ROAD_TOP_Y && s_enemies[i].y < ROAD_BOT_Y){
                float t = (float)(s_enemies[i].y - ROAD_TOP_Y)/(ROAD_BOT_Y-ROAD_TOP_Y);
                float w = ROAD_TOP_W + (ROAD_BOT_W-ROAD_TOP_W)*t;
                int left = ROAD_XC - (int)(w/2);
                int right= ROAD_XC + (int)(w/2);
                if(s_enemies[i].x < left+ENEMY_W/2) s_enemies[i].x = left+ENEMY_W/2;
                if(s_enemies[i].x > right-ENEMY_W/2) s_enemies[i].x = right-ENEMY_W/2;
            }
            if(s_enemies[i].y > 340){ s_enemies[i].alive=false; s_score += 10*s_speed; }
            // collision
            int dx = abs(s_enemies[i].x - s_player_x);
            int dy = abs(s_enemies[i].y - 260);
            if(dx < (CAR_W+ENEMY_W)/2 -3 && dy < (CAR_H+ENEMY_H)/2 -4){
                s_lives--; s_crashed=true; s_crash_t=now_ms; s_enemies[i].alive=false;
                if(s_lives<=0) ESP_LOGI(TAG,"GAME OVER %d", s_score);
            }
        }
    }
    s_score += s_speed;
}

void game_draw(void){
    if(!s_active) return;
    if(!fb()){ // fallback old method if fb not allocated
        tft_fill(TFT_BLACK); return;
    }
    fb_fill(TFT_BLACK);
    // road
    for(int y=ROAD_TOP_Y; y<ROAD_BOT_Y; y++){
        float t = (float)(y - ROAD_TOP_Y)/(ROAD_BOT_Y-ROAD_TOP_Y);
        float w = ROAD_TOP_W + (ROAD_BOT_W-ROAD_TOP_W)*t;
        int left = ROAD_XC - (int)(w/2);
        int right= ROAD_XC + (int)(w/2);
        // grass
        uint16_t grass = (y%2==0)? RGB565(20,90,20): RGB565(15,70,15);
        fb_rect(0, y, left, 1, grass);
        fb_rect(right, y, W-right, 1, grass);
        fb_rect(left, y, right-left, 1, RGB565(55,55,55));
        if( ((y + s_road_anim)%20) < 10 ){
            fb_rect(ROAD_XC-1, y, 2, 1, TFT_WHITE);
        }
        fb_rect(left-2, y, 2, 1, TFT_WHITE);
        fb_rect(right, y, 2, 1, TFT_WHITE);
    }
    // enemies
    for(int i=0;i<4;i++) if(s_enemies[i].alive) fb_car(s_enemies[i].x, s_enemies[i].y, s_enemies[i].col);
    if(!s_crashed || (s_frame%4<2)) fb_car(s_player_x, 260, TFT_BLUE);
    // HUD bar
    fb_rect(0,0,W,20, TFT_BLACK);
    char buf[32];
    snprintf(buf,sizeof(buf),"SCORE:%d", s_score);
    fb_text(4,6,buf,TFT_WHITE,TFT_BLACK,1);
    snprintf(buf,sizeof(buf),"SPD:%d", s_speed*18);
    fb_text(90,6,buf,TFT_YELLOW,TFT_BLACK,1);
    for(int i=0;i<3;i++){
        uint16_t c = i < s_lives ? TFT_RED : RGB565(60,0,0);
        fb_rect(170+i*18, 5, 14, 10, c);
        fb_text(172+i*18,6,"*",TFT_WHITE,c,1);
    }
    fb_text(4, H-12, "LX:steer RT:gas LT:brake BACK+START:exit", RGB565(180,180,180), RGB565(0,0,0), 1);
    if(s_lives<=0){
        fb_rect(W/2-62, H/2-18, 124, 36, TFT_BLACK);
        fb_rect(W/2-62, H/2-18, 124, 36, TFT_WHITE); // border
        fb_rect(W/2-60, H/2-16, 120, 32, TFT_BLACK);
        fb_text(W/2-40, H/2-8, "GAME OVER", TFT_RED, TFT_BLACK, 1);
    }
    tft_push_fb(fb());
}
