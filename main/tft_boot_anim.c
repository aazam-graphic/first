#include "tft_boot_anim.h"
#include "tft_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define W TFT_H_RES
#define H TFT_V_RES

// #define RGB565(r,g,b) - use tft_display.h (uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))
#define C_BLACK  RGB565(0,0,0)
#define C_WHITE  RGB565(255,255,255)
#define C_DARK   RGB565(4,8,14)
#define C_GRID   RGB565(15,50,66)
#define C_GREY   RGB565(82,98,116)
#define C_DGREY  RGB565(26,38,51)
#define C_TEAL   RGB565(0,232,202)
#define C_CYAN   RGB565(0,190,255)
#define C_BLUE   RGB565(32,78,250)
#define C_GREEN  RGB565(10,250,112)
#define C_YELLOW RGB565(255,208,0)
#define C_ORANGE RGB565(255,108,0)
#define C_RED    RGB565(255,30,26)

static uint32_t now_ms(void){ return (uint32_t)(esp_timer_get_time()/1000ULL); }
static int clampi(int v,int lo,int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
static float clampf(float v,float lo,float hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
static float smoothstep(float e0,float e1,float v){ float t=clampf((v-e0)/(e1-e0),0,1); return t*t*(3-2*t); }
static uint16_t blend(uint16_t a,uint16_t b,uint8_t m){
    uint32_t ar=(a>>11)&0x1F, ag=(a>>5)&0x3F, ab=a&0x1F;
    uint32_t br=(b>>11)&0x1F, bg=(b>>5)&0x3F, bb=b&0x1F;
    uint32_t r=(ar*(255-m)+br*m)/255, g=(ag*(255-m)+bg*m)/255, bl=(ab*(255-m)+bb*m)/255;
    return (r<<11)|(g<<5)|bl;
}

static uint16_t *fb(void){ return tft_get_fb(); }
static inline void px(int x,int y,uint16_t c){ if((unsigned)x>=W||(unsigned)y>=H) return; fb()[y*W+x]=c; }
static void clear_fb(uint16_t c){ uint16_t *f=fb(); if(!f) return; for(int i=0;i<W*H;i++) f[i]=c; }
static void fill_rect(int x,int y,int w,int h,uint16_t c){
    if(!fb()||w<=0||h<=0) { return; } // fixed
    int x0=clampi(x,0,W), y0=clampi(y,0,H), x1=clampi(x+w,0,W), y1=clampi(y+h,0,H);
    for(int yy=y0;yy<y1;yy++){ uint16_t *row=&fb()[yy*W]; for(int xx=x0;xx<x1;xx++) row[xx]=c; }
}
static void line_aa(int x0,int y0,int x1,int y1,uint16_t c){
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
    while(1){ px(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=err<<1; if(e2>=dy){err+=dy; x0+=sx;} if(e2<=dx){err+=dx; y0+=sy;}}
}
static void rect(int x,int y,int w,int h,uint16_t c){ line_aa(x,y,x+w-1,y,c); line_aa(x,y,x,y+h-1,c); line_aa(x+w-1,y,x+w-1,y+h-1,c); line_aa(x,y+h-1,x+w-1,y+h-1,c); }
static void circle(int cx,int cy,int r,uint16_t c){
    int x=r,y=0,err=1-r; while(x>=y){ px(cx+x,cy+y,c); px(cx+y,cy+x,c); px(cx-y,cy+x,c); px(cx-x,cy+y,c); px(cx-x,cy-y,c); px(cx-y,cy-x,c); px(cx+y,cy-x,c); px(cx+x,cy-y,c); y++; if(err<0) err+=2*y+1; else {x--; err+=2*(y-x)+1;}}
}
static void fill_circle(int cx,int cy,int r,uint16_t c){ for(int y=-r;y<=r;y++){ int x=(int)sqrtf((float)(r*r-y*y)); fill_rect(cx-x,cy+y,x*2+1,1,c);} }

static const uint8_t font5x7[95][5]={
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
    {0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},
    {0x0C,0x52,0x52,0x52,0x3E},{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},
    {0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08}
};
static void char_big(int x,int y,char ch,int sc,uint16_t col){ if(ch<32||ch>126) ch='?'; const uint8_t *g=font5x7[ch-32]; for(int gx=0;gx<5;gx++) for(int gy=0;gy<7;gy++) if(g[gx]&(1U<<gy)) fill_rect(x+gx*sc,y+gy*sc,sc,sc,col); }
static int text_w(const char *s,int sc){ int n=0; while(*s++) n++; return n*6*sc; }
static void text_big(int x,int y,const char *s,int sc,uint16_t col){ while(*s){ char_big(x,y,*s++,sc,col); x+=6*sc; } }
static void text_center(int y,const char *s,int sc,uint16_t col){ int x=(W - text_w(s,sc))/2; text_big(x,y,s,sc,col); }

static void draw_bg(uint32_t ms,bool alive){
    clear_fb(alive? blend(C_DARK, C_GRID, 90) : C_DARK);
    uint16_t gh = blend(C_DARK, C_GRID, alive?100:55), gv=blend(C_DARK,C_GRID, alive?72:34);
    for(int y=25;y<H;y+=18) line_aa(0,y,W-1,y,gh);
    for(int x=0;x<W;x+=20) line_aa(x,24,x,H-1,gv);
    fill_rect(0,0,W,24,C_BLACK); fill_rect(0,23,W,1, alive?C_TEAL:C_DGREY);
    text_big(10,6,"AZAM",2, alive?C_TEAL:C_GREY);
    text_big(237,6,"ESP32-S3",1, alive?C_CYAN:C_GREY);
    int sx=(int)(ms*0.18f)%W; fill_rect(sx,24,2,H-24, alive?C_TEAL:C_DGREY);
}
static void draw_car(int ox,int oy,int sc,uint16_t body,uint16_t edge,bool light,bool alive){
    int x=ox,y=oy,s=sc;
    uint16_t glow=alive?blend(C_DARK,C_TEAL,155):C_DGREY;
    fill_rect(x+24*s,y+70*s,138*s,3*s,glow); fill_rect(x+40*s,y+74*s,107*s,2*s,glow);
    fill_circle(x+43*s,y+62*s,16*s,C_BLACK); circle(x+43*s,y+62*s,16*s,edge); fill_circle(x+43*s,y+62*s,8*s,C_DGREY); circle(x+43*s,y+62*s,8*s, alive?C_TEAL:C_GREY);
    fill_circle(x+143*s,y+62*s,16*s,C_BLACK); circle(x+143*s,y+62*s,16*s,edge); fill_circle(x+143*s,y+62*s,8*s,C_DGREY); circle(x+143*s,y+62*s,8*s, alive?C_TEAL:C_GREY);
    fill_rect(x+18*s,y+40*s,150*s,23*s,body); line_aa(x+18*s,y+40*s,x+168*s,y+40*s,edge); line_aa(x+18*s,y+63*s,x+168*s,y+63*s,edge);
    fill_rect(x+165*s,y+44*s,17*s,14*s,body); line_aa(x+182*s,y+44*s,x+182*s,y+58*s,edge);
    fill_rect(x+6*s,y+46*s,15*s,12*s,body);
    for(int i=0;i<31*s;i++){ int yy=y+39*s-i, l=x+55*s+i/2, r=x+130*s-i/3; if(r>l) line_aa(l,yy,r,yy,body); }
    uint16_t glass=alive?blend(C_BLACK,C_CYAN,92):blend(C_BLACK,C_GREY,80);
    for(int i=0;i<20*s;i++){ int yy=y+36*s-i, l=x+67*s+i/2, r=x+118*s-i/3; if(r>l) line_aa(l,yy,r,yy,glass); }
    line_aa(x+99*s,y+18*s,x+99*s,y+39*s,edge);
    fill_rect(x+93*s,y+5*s,14*s,10*s,body); fill_rect(x+104*s,y+7*s,38*s,5*s,body); line_aa(x+104*s,y+7*s,x+142*s,y+7*s,edge);
    line_aa(x+8*s,y+36*s,x+25*s,y+28*s,edge); line_aa(x+3*s,y+28*s,x+28*s,y+28*s,edge);
    fill_rect(x+170*s,y+45*s,8*s,5*s, light?C_WHITE:C_DGREY); fill_rect(x+8*s,y+48*s,5*s,8*s, light?C_RED:C_DGREY);
    line_aa(x+78*s,y+44*s,x+158*s,y+44*s,edge); line_aa(x+116*s,y+58*s,x+155*s,y+58*s,edge);
}
static void draw_rpm(int pct,bool alive){
    int x=24,y=204,w=272,h=16; pct=clampi(pct,0,100); rect(x,y,w,h,C_GREY); fill_rect(x+2,y+2,w-4,h-4,C_BLACK);
    int fw=pct*(w-4)/100; uint16_t col=pct<45?C_GREEN:pct<75?C_YELLOW:C_RED; if(fw>0) fill_rect(x+2,y+2,fw,h-4,col);
    for(int i=0;i<=10;i++){ int mx=x+2+i*(w-4)/10; fill_rect(mx,y+h+3,2,5, alive?C_TEAL:C_GREY); }
}
static void draw_ready(uint32_t ms){
    const char *labs[4]={"IMU","US","AUDIO","READY"};
    for(int i=0;i<4;i++){ uint32_t at=1840+i*130; if(ms<at) continue; int x=13+i*77; fill_rect(x,177,68,16,C_BLACK); rect(x,177,68,16,C_GREEN); fill_circle(x+8,185,3,C_GREEN); text_big(x+16,181,labs[i],1,C_WHITE); }
}
static void boot_frame(uint32_t el){
    // Sync with engine sound 10s: first crank fail 2-26% (200-2600ms) screen OFF, second 48-89% (4800-8900ms) screen ON
    bool p1=el>=200&&el<2600, p2=el>=4800&&el<8900, pCatch=el>=8900&&el<9500, pReady=el>=9500;
    bool alive=pCatch||pReady;
    int shake=0;
    if(p1) shake=((el/48)&1)?2:-2;
    if(p2) shake=((el/25)&1)?4:-4;
    draw_bg(el,alive);
    if(p1||p2){
        uint16_t col=p2?C_ORANGE:C_RED;
        int prog=p1?(int)((el-200)*100/2400):(int)((el-4800)*100/4100);
        for(int i=0;i<8;i++){ int ht=12+((prog+i*13)%46); int xx=12+i*39; fill_rect(xx,170-ht,22,ht, blend(C_DARK,col,128)); fill_rect(xx,170-ht,22,2,col); }
        text_center(34, p1?"IGNITION 1":"IGNITION 2",3,col);
    }
    if(pCatch){
        float fade=smoothstep(8900,9300,(float)el);
        if(el<9300){ uint8_t m=(uint8_t)((1-fade)*230); clear_fb(blend(C_DARK,C_WHITE,m)); }
        int r=15+(int)((el-8900)*0.22f), r2=r-42;
        if(r>0) circle(160,113,r,C_TEAL);
        if(r2>0) circle(160,113,r2,C_CYAN);
        for(int a=0;a<360;a+=20){ float rad=a*3.1415926f/180; int x0=160+(int)(cosf(rad)*28), y0=112+(int)(sinf(rad)*28), x1=160+(int)(cosf(rad)*r), y1=112+(int)(sinf(rad)*r); line_aa(x0,y0,x1,y1,C_CYAN); }
    }
    uint16_t body=C_DGREY, edge=C_GREY;
    if(p2){ body=blend(C_DGREY,C_ORANGE,120); edge=C_ORANGE; }
    if(alive){ body=blend(C_BLUE,C_TEAL,105); edge=C_TEAL; }
    // First crank (p1) is FAIL - screen OFF, no car
    if(p2||pCatch||pReady){
        int cx=51+shake, cy=72+(p2?shake/2:0);
        draw_car(cx,cy,1,body,edge,alive,alive);
    } else if(p1){
        // p1 fail: keep screen dark, no car
        fill_rect(0,60,W,H-60,C_BLACK);
    }
    if(p1||p2){ uint16_t c=p2?C_ORANGE:C_RED; fill_circle(160+shake,110,p2?14:9,c); fill_circle(160+shake,110,p2?6:4,C_WHITE); }
    if(pReady){
        float ap=smoothstep(1750,1950,(float)el);
        uint16_t sc=blend(C_DARK,C_TEAL,(uint8_t)(ap*255));
        text_center(31,"SYSTEM",4,sc); text_center(67,"READY",5,C_WHITE);
        text_center(156,"MANUAL PILOT MODE",2,C_TEAL); draw_ready(el);
    }
    int rpm=0;
    if(p1) rpm=10+(int)((el-200)*20/2400);
    else if(p2) rpm=30+(int)((el-4800)*50/4100);
    else if(pCatch){ float r=smoothstep(1150,1420,(float)el); rpm=80+(int)(20*r); }
    else { float s=smoothstep(1750,2450,(float)el); rpm=100-(int)(72*s); }
    draw_rpm(rpm,alive);
    if(p1) text_center(184,"CRANKING...",2,C_RED);
    else if(p2) text_center(184,"ENGINE START...",2,C_ORANGE);
    else if(pCatch) text_center(184,"POWER ONLINE",2,C_TEAL);
}

void tft_boot_anim(void){
    if(!tft_is_ready() || !fb()) return;
    uint32_t total=10000, frame_delay=40, start=now_ms(); // 10s to match engine sound
    while((uint32_t)(now_ms()-start) < total){
        uint32_t el=now_ms()-start;
        boot_frame(el);
        tft_push_fb(fb());
        vTaskDelay(pdMS_TO_TICKS(frame_delay));
    }
    clear_fb(C_DARK);
    tft_push_fb(fb());
}
