#include <math.h>
#include "os_gfx.h"
#include "os_theme.h"
#include "car_global.h"
#include "car_os.h"
#include "imu_driver.h"
#include "esp_heap_caps.h"
#include "ui_motion_radar.h"

#define W 320
#define H 240
#define BG    RGB565(12,20,42)
#define PAN   RGB565(22,36,68)
#define GRID  RGB565(20,64,96)
#define GLO   RGB565(14,40,66)
#define CYN   RGB565(0,242,255)
#define GRN   RGB565(55,255,135)
#define YLW   RGB565(255,222,60)
#define ORG   RGB565(255,145,40)
#define RED   RGB565(255,72,95)
#define GRY   RGB565(95,110,130)
#define WHT   RGB565(238,246,255)
#define CX 160
#define CY 118
#define CW 44
#define CH 26
#define RNG 200
#define R1 46
#define R2 76
#define R3 104

static int ic(int v,int lo,int hi){ if(v<lo)v=lo; if(v>hi)v=hi; return v; }
static int ia(int v){ return v<0?-v:v; }

static uint8_t us_lv(int cm,int ok){
    if(!ok||cm<0) return 4;
    int o=car_get_setting(2); if(o<20)o=20;
    if(cm<20) return 3;
    if(cm<o) return 2;
    if(cm<o*2) return 1;
    return 0;
}
static uint16_t lc(uint8_t lv){
    if(lv==1) return YLW;
    if(lv==2) return ORG;
    if(lv==3) return RED;
    if(lv==4) return GRY;
    return GRN;
}
static void disc(int x,int y,int r,uint16_t c){
    for(int d=-r;d<=r;d++){int w=(int)sqrtf((float)(r*r-d*d));gfx_hline(x-w,y+d,2*w+1,c);}
}
static void circ(int x,int y,int r,uint16_t c){
    int a=0,b=r,dd=3-2*r;
    while(b>=a){
        gfx_px(x+a,y+b,c);gfx_px(x-a,y+b,c);gfx_px(x+a,y-b,c);gfx_px(x-a,y-b,c);
        gfx_px(x+b,y+a,c);gfx_px(x-b,y+a,c);gfx_px(x+b,y-a,c);gfx_px(x-b,y-a,c);
        a++; if(dd<0)dd+=4*a+6; else {b--;dd+=4*(a-b)+10;}
    }
}
void draw_car(int col){
    int x0=CX-CW/2,x1=CX+CW/2,y0=CY-CH/2,y1=CY+CH/2,ch=5;
    gfx_hline(x0+ch,y0,x1-x0-2*ch,col); gfx_hline(x0+ch,y1,x1-x0-2*ch,col);
    gfx_vline(x0,y0+ch,y1-y0-2*ch,col); gfx_vline(x1,y0+ch,y1-y0-2*ch,col);
    gfx_line(x0+ch,y0,x0,y0+ch,col); gfx_line(x1-ch,y0,x1,y0+ch,col);
    gfx_line(x0+ch,y1,x0,y1-ch,col); gfx_line(x1-ch,y1,x1,y1-ch,col);
    gfx_hline(x0+3,y0+3,x1-x0-6,WHT); gfx_hline(x0+3,y1-2,x1-x0-6,WHT); gfx_vline(x0+3,y0+3,y1-y0-6,WHT);
    gfx_rect(CX-9,y0+4,18,6,RGB565(8,14,30)); gfx_hline(CX-7,y0+6,14,GLO);
    gfx_hline(x0-5,y0+5,4,col); gfx_hline(x1+1,y0+5,4,col);
    gfx_line(CX,y0-2,CX-4,y0-7,CYN); gfx_line(CX,y0-2,CX+4,y0-7,CYN); gfx_line(CX-4,y0-7,CX+4,y0-7,CYN);
}
void draw_rings(uint16_t col){
    circ(CX,CY,R3,col); circ(CX,CY,R2,col); circ(CX,CY,R1,col);
    gfx_hline(CX-R3,CY,R3*2,GLO); gfx_vline(CX,CY-R3,R3*2,GLO);
}
static void marker(int x,int y,uint8_t lv,uint32_t t){
    uint16_t c=lc(lv);
    if(lv==3){ disc(x,y,10,c); if((t/400)&1) circ(x,y,14,c); disc(x,y,3,WHT); }
    else if(lv==2){ disc(x,y,7,c); disc(x,y,2,WHT); }
    else if(lv==1){ circ(x,y,7,c); disc(x,y,2,c); }
    else { circ(x,y,5,c); gfx_line(x-2,y-2,x+2,y+2,c); gfx_line(x-2,y+2,x+2,y-2,c); }
}
static void fbeam(uint8_t lv,int dist,uint32_t t){
    if(lv==4){
        uint16_t c=GRY;
        for(int r=CY-CH/2-6;r>CY-R3;r-=7) gfx_rect(CX-2,r,5,4,c);
        gfx_line(CX-7,CY-R3-3,CX+7,CY-R3+3,c); gfx_line(CX-7,CY-R3+3,CX+7,CY-R3-3,c);
        return;
    }
    uint16_t col=(lv==0)?GRN:lc(lv);
    int tip=CY-ic(CH/2+4+(dist*(R3-CH/2-8))/RNG,CH/2+6,R3);
    for(int y=tip;y<CY-CH/2;y++){
        int hf=2+((CY-CH/2-y)*22)/(CY-CH/2-tip+1);
        gfx_hline(CX-hf,y,2*hf,col);
    }
    if(lv>0) marker(CX,tip,lv,t);
}
static void sbeam(uint8_t lv,int dist,uint32_t t,int dir){
    if(lv==4){
        uint16_t c=GRY;
        for(int x=CX+CW/2+6;x<CX+R3;x+=7) gfx_rect(x,CY-2,4,5,c);
        gfx_line(CX+R3+3,CY-7,CX+R3-3,CY+7,c); gfx_line(CX+R3+3,CY+7,CX+R3-3,CY-6,c);
        return;
    }
    uint16_t col=(lv==0)?GRN:lc(lv);
    int tip=CX+dir*ic(CW/2+4+(dist*(R3-CW/2-8))/RNG,CW/2+6,R3);
    if(dir<0){
        for(int x=tip;x<CX+CW/2;x++){ int hf=2+((x-tip)*14)/(CX+CW/2-tip+1); gfx_vline(x,CY-hf,2*hf,col); }
    } else {
        for(int x=CX+CW/2;x<tip;x++){ int hf=2+((x-CX-CW/2)*14)/(tip-CX-CW/2+1); gfx_vline(x,CY-hf,2*hf,col); }
    }
    if(lv>0) marker(tip,CY,lv,t);
}

static void tilt(int x,int y,int w,int h,float p,float r,uint8_t lv){
    uint16_t col=(lv==0)?GRN:lc(lv);
    gfx_rect(x,y,w,h,PAN); gfx_rect_outline(x,y,w,h,col);
    gfx_text_small(x+4,y+2,"TILT",col);
    int fx=x+6,fy=y+16,fw=w-12,fh=h-20;
    gfx_rect_outline(fx,fy,fw,fh,GRID); gfx_hline(fx,fy+fh/2,fw,GRID); gfx_vline(fx+fw/2,fy,fh,GRID);
    int bx=fx+fw/2+(int)(r*((fw/2-8)/30.0f));
    int by=fy+fh/2-(int)(p*((fh/2-8)/30.0f));
    bx=ic(bx,fx+6,fx+fw-6); by=ic(by,fy+6,fy+fh-6);
    disc(bx,by,5,col); disc(bx,by,2,WHT);
    if(lv<3){
        int tc=fx+fw-18,tcy=fy+fh/2; float rr=r*0.01745f;
        int dx=(int)(10*sinf(rr)),dy=(int)(4*cosf(rr));
        gfx_line(tc-dx,tcy-dy,tc+dx,tcy+dy,GLO);
        gfx_line(tc-dx-3,tcy-dy,tc-dx+3,tcy-dy,GLO);
        gfx_line(tc+dx-3,tcy+dy,tc+dx+3,tcy+dy,GLO);
    }
}
static void turn(int x,int y,int w,int h,float yw,uint8_t lv){
    uint16_t col=(lv==0)?GRN:lc(lv);
    gfx_rect(x,y,w,h,PAN); gfx_rect_outline(x,y,w,h,col);
    gfx_text_small(x+4,y+2,"TURN",col);
    int fy=y+18,fh=h-22,cx0=x+w/2;
    disc(cx0,fy+fh/2,3,GRID);
    if(ia((int)yw)<6){ disc(cx0,fy+fh/2,3,GRN); return; }
    int dir=(yw>0)?-1:1;
    int bars=ic((int)(fabsf(yw)/40.0f)+1,1,5);
    for(int i=0;i<bars;i++){
        int bx=cx0+dir*(6+i*9);
        uint16_t c; if(i<2)c=CYN; else if(i<3)c=YLW; else c=(lv==3)?RED:ORG;
        gfx_rect(bx,fy+fh/2-5,7,11,c);
    }
    int ax=cx0+dir*(6+bars*9+6),ay=fy+fh/2,ar=8;
    int a0=(dir<0)?200:280,a1=(dir<0)?280:460;
    for(int a=a0;a<a1;a+=5){ float rad=a*0.0174533f; gfx_px(ax+(int)(ar*cosf(rad)),ay+(int)(ar*sinf(rad)),col); }
    int a2=(dir<0)?200:460; float r2=a2*0.0174533f;
    int hx=ax+(int)(ar*cosf(r2)),hy=ay+(int)(ar*sinf(r2));
    gfx_line(hx,hy,hx+dir*4,hy-4,col); gfx_line(hx,hy,hx+dir*4,hy+4,col);
}
static void shock(int t0,uint32_t now){
    if(t0==0) return;
    uint32_t el=now-t0; if(el>700) return;
    int r1=20+(int)(30.0f*el/700.0f),r2=20+(int)(55.0f*el/700.0f);
    circ(CX,CY,r1,RED); circ(CX,CY,ic(r2,r1+1,140),ORG);
}

void draw_details(void){

    gfx_clear(BG); gfx_rect_outline(0,0,W,H,GRID);
    gfx_text_small(10,8,"DETAILS",WHT); gfx_hline(0,22,W,GRID);
    imu_data_t imu; bool ok=motion_radar_imu_read(&imu);
    char b[48]; int x=10,y=28;
    for(int i=0;i<3;i++){
        int v=(g.dist_avg[i]==65535)?-1:(int)g.dist_avg[i];
        const char*nm=i==0?"LEFT":i==1?"FRONT":"RIGHT";
        if(v>=0) snprintf(b,sizeof(b),"%s  %d cm",nm,v); else snprintf(b,sizeof(b),"%-6s --",nm);
        gfx_text_small(x,y,b,WHT); y+=16;
    }
    y+=4; gfx_hline(10,y-2,300,GRID);
    if(!ok){ gfx_text_small(x,y,"IMU: NO LINK",RED); y+=16; }
    else {
        snprintf(b,sizeof(b),"PITCH %+.*f   ROLL %+.*f",1,(double)imu.pitch_deg,1,(double)imu.roll_deg);
        gfx_text_small(x,y,b,WHT); y+=15;
        snprintf(b,sizeof(b),"YAW RATE %+.*f dps",1,(double)imu.yaw_rate_dps);
        gfx_text_small(x,y,b,WHT); y+=15;
        snprintf(b,sizeof(b),"ACC %+.*f %+.*f %+.*f g",2,(double)imu.accel_xg,2,(double)imu.accel_yg,2,(double)imu.accel_zg);
        gfx_text_small(x,y,b,WHT); y+=15;
        snprintf(b,sizeof(b),"GYRO %+.*f %+.*f %+.*f",1,(double)imu.gyro_xdps,1,(double)imu.gyro_ydps,1,(double)imu.gyro_zdps);
        gfx_text_small(x,y,b,WHT); y+=15;
        snprintf(b,sizeof(b),"WHO 0x%02X  ADDR 0x%02X  BIAS %+.*f",(unsigned)imu.who_am_i,(unsigned)imu.i2c_addr,1,(double)imu_driver_gyro_bias_z());
        gfx_text_small(x,y,b,WHT); y+=15;
        snprintf(b,sizeof(b),"CAL %u%%  TEMP %dC",(unsigned)imu_driver_cal_pct(),(int)imu.temp_c);
        gfx_text_small(x,y,b,WHT); y+=15;
    }
    y+=4; gfx_hline(10,y-2,300,GRID);
    snprintf(b,sizeof(b),"HEAP %u KB",(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024));
    gfx_text_small(x,y,b,WHT);
    const os_perf_t*pf=os_perf(); snprintf(b,sizeof(b),"FPS %u",(unsigned)pf->fps);
    gfx_text_small(x+150,y,b,WHT);
    gfx_text_small(10,H-20,"< BACK",GRY);
}

void ui_motion_radar_draw(uint32_t now,bool details){
    if(details){ draw_details(); return; }
    gfx_clear(BG); gfx_rect_outline(0,0,W,H,GRID);
    gfx_text_small(10,6,"MOTION RADAR",WHT);
    int dl=(g.dist_avg[0]==65535)?-1:(int)g.dist_avg[0];
    int df=(g.dist_avg[1]==65535)?-1:(int)g.dist_avg[1];
    int dr=(g.dist_avg[2]==65535)?-1:(int)g.dist_avg[2];
    uint8_t ll=us_lv(dl,dl>=0), lf=us_lv(df,df>=0), lr=us_lv(dr,dr>=0);
    imu_data_t imu; bool ok=motion_radar_imu_read(&imu);
    uint8_t tlv=3,trv=0; float pit=0,rol=0,yw=0;
    if(ok){
        rol=imu.roll_deg; pit=imu.pitch_deg; yw=imu.yaw_rate_dps;
        float tr=ia((int)rol)>ia((int)pit)?fabsf(rol):fabsf(pit);
        tlv=tr>45?3:tr>25?2:tr>12?1:0;
        float ay=fabsf(yw); trv=ay>150?3:ay>80?2:ay>25?1:0;
    }
    static uint32_t i_t0=0,i_nx=0;
    if(ok){
        float mag=sqrtf(imu.accel_xg*imu.accel_xg+imu.accel_yg*imu.accel_yg+imu.accel_zg*imu.accel_zg);
        if(mag>2.2f && (int32_t)(now-i_nx)>=0){ i_t0=now; i_nx=now+1500; }
    }
    bool imp=(i_t0>0)&&((int32_t)(now-i_t0)<700);
    uint16_t dot=GRN;
    if(g.estop||imp||tlv>=3||lf==3||ll==3||lr==3) dot=RED;
    else if(tlv>=2||lf==2||ll==2||lr==2) dot=ORG;
    else if(tlv>=1||lf==1||ll==1||lr==1) dot=YLW;
    else if(!ok) dot=GRY;
    disc(W-16,10,5,dot);
    if(g.estop){
        bool on=((now/500)&1)==0;
        gfx_rect_outline(2,2,W-4,H-4,on?RED:PAN); gfx_rect_outline(0,0,W,H,RED);
        gfx_rect(CX-18,CY-18,36,36,on?RED:PAN);
        gfx_text_center_box(CX-22,CY-5,44,"STOP",1,WHT);
        gfx_text_small(12,H-22,"[A] CAL   < VIEW >   [B] EXIT",GRY);
        return;
    }
    draw_rings(GRID);
    if(imp) shock(i_t0,now);
    int cc=(tlv>=3||lf==3)?RED:CYN;
    draw_car(cc);
    fbeam(lf,df<0?0:df,now);
    sbeam(ll,dl<0?0:dl,now,-1);
    sbeam(lr,dr<0?0:dr,now,1);
    if(lf==3) gfx_text_center_box(CX-20,CY-R3-22,40,"STOP",1,RED);
    int pw=(W-24)/2,ph=44,py=H-22-ph;
    if(!ok){
        gfx_rect(8,py,pw,ph,PAN); gfx_rect_outline(8,py,pw,ph,GRY);
        gfx_text_small(14,py+ph/2-4,"--",GRY);
        gfx_rect(16+pw,py,pw,ph,PAN); gfx_rect_outline(16+pw,py,pw,ph,GRY);
        gfx_text_small(22+pw,py+ph/2-4,"--",GRY);
    } else {
        tilt(8,py,pw,ph,pit,rol,tlv);
        turn(16+pw,py,pw,ph,yw,trv);
    }
    uint16_t sc=GRN;
    if(imp||tlv>=3) sc=RED; else if(tlv>=2) sc=ORG; else if(tlv>=1) sc=YLW;
    int sx=8+pw-2,sy=py+ph/2;
    gfx_line(sx,sy-6,sx+8,sy-6,sc); gfx_line(sx+8,sy-6,sx+9,sy,sc);
    gfx_line(sx+9,sy,sx,sy+7,sc); gfx_line(sx,sy+7,sx-1,sy,sc); gfx_line(sx-1,sy,sx,sy-6,sc);
    disc(20,H-16,6,GRN); gfx_text_small(14,H-21,"A",BG);
    gfx_text_small(28,H-22,"CALIBRATE",WHT);
    gfx_text_small(128,H-16,"<",CYN); gfx_text_small(138,H-16,"VIEW",WHT); gfx_text_small(178,H-16,">",CYN);
    gfx_text_small(252,H-22,"EXIT",WHT); disc(260,H-16,6,RED); gfx_text_small(246,H-21,"B",BG);
}
