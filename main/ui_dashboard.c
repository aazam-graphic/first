#include "ui_dashboard.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG="ui_premium";
static lv_obj_t *lbl_speed, *arc_speed, *lbl_mode, *lbl_batt, *bar_rt, *bar_lt, *lbl_rt, *lbl_lt, *lbl_steer;
static lv_obj_t *lbl_top_title;

void ui_dashboard_init(void){
    ESP_LOGI(TAG,"Premium Cyberpunk Black init");
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Top bar - premium black with neon border
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, 320, 28);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_border_color(top, lv_color_hex(0x00e8cc), 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(top, 4, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_top_title = lv_label_create(top);
    lv_label_set_text(lbl_top_title, "AZAM  CYBERPUNK");
    lv_obj_set_style_text_color(lbl_top_title, lv_color_hex(0x00e8cc), 0);
    lv_obj_set_style_text_font(lbl_top_title, &lv_font_montserrat_14, 0);

    lbl_mode = lv_label_create(top);
    lv_label_set_text(lbl_mode, "ECO");
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_color(lbl_mode, lv_color_hex(0x00e8cc), 0);
    lv_obj_set_style_bg_opa(lbl_mode, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(lbl_mode, 8, 0);
    lv_obj_set_style_pad_ver(lbl_mode, 2, 0);
    lv_obj_set_style_radius(lbl_mode, 4, 0);

    lv_obj_t *batt_box = lv_label_create(top);
    lbl_batt = batt_box;
    lv_label_set_text(lbl_batt, "12.4V 84%");
    lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0x00ff88), 0);

    // Center speed arc - premium neon
    arc_speed = lv_arc_create(scr);
    lv_obj_set_size(arc_speed, 140, 140);
    lv_obj_set_pos(arc_speed, 90, 35);
    lv_arc_set_range(arc_speed, 0, 100);
    lv_arc_set_value(arc_speed, 0);
    lv_arc_set_bg_angles(arc_speed, 135, 45);
    lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0x00e8cc), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_speed, 10, 0);
    lv_obj_set_style_arc_rounded(arc_speed, true, 0);
    lv_obj_remove_style(arc_speed, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc_speed, LV_OPA_TRANSP, 0);

    lbl_speed = lv_label_create(scr);
    lv_label_set_text(lbl_speed, "065");
    lv_obj_set_style_text_font(lbl_speed, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_speed, lv_color_hex(0xffffff), 0);
    // Make it look bigger via scale - use 2x via style?
    lv_obj_set_style_text_font(lbl_speed, &lv_font_montserrat_14, 0);
    lv_obj_align_to(lbl_speed, arc_speed, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *lbl_unit = lv_label_create(scr);
    lv_label_set_text(lbl_unit, "KM/H");
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x8899aa), 0);
    lv_obj_align_to(lbl_unit, arc_speed, LV_ALIGN_CENTER, 0, 14);

    // Steer indicator below arc
    lbl_steer = lv_label_create(scr);
    lv_label_set_text(lbl_steer, "<  0   >");
    lv_obj_set_style_text_color(lbl_steer, lv_color_hex(0x00e8cc), 0);
    lv_obj_align_to(lbl_steer, arc_speed, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    // RT/LT neon bars - vertical premium
    bar_lt = lv_bar_create(scr);
    lv_obj_set_size(bar_lt, 14, 100);
    lv_obj_set_pos(bar_lt, 18, 45);
    lv_bar_set_range(bar_lt, 0, 100);
    lv_obj_set_style_bg_color(bar_lt, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_lt, lv_color_hex(0xff4400), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_lt, 7, 0);
    lbl_lt = lv_label_create(scr); lv_label_set_text(lbl_lt, "LT"); lv_obj_set_pos(lbl_lt, 20, 148); lv_obj_set_style_text_color(lbl_lt, lv_color_hex(0x666666), 0);

    bar_rt = lv_bar_create(scr);
    lv_obj_set_size(bar_rt, 14, 100);
    lv_obj_set_pos(bar_rt, 288, 45);
    lv_bar_set_range(bar_rt, 0, 100);
    lv_obj_set_style_bg_color(bar_rt, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rt, lv_color_hex(0x00ff88), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_rt, 7, 0);
    lbl_rt = lv_label_create(scr); lv_label_set_text(lbl_rt, "RT"); lv_obj_set_pos(lbl_rt, 290, 148); lv_obj_set_style_text_color(lbl_rt, lv_color_hex(0x666666), 0);

    // Bottom info bar - premium
    lv_obj_t *bottom = lv_obj_create(scr);
    lv_obj_set_size(bottom, 320, 42);
    lv_obj_set_pos(bottom, 0, 198);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(0x0a0a0f), 0);
    lv_obj_set_style_border_color(bottom, lv_color_hex(0x00e8cc), 0);
    lv_obj_set_style_border_width(bottom, 1, 0);
    lv_obj_set_style_border_side(bottom, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_l = lv_label_create(bottom); lv_label_set_text(lbl_l, "L: ---"); lv_obj_set_style_text_color(lbl_l, lv_color_hex(0x8899aa), 0);
    lv_obj_t *lbl_r = lv_label_create(bottom); lv_label_set_text(lbl_r, "R: ---"); lv_obj_set_style_text_color(lbl_r, lv_color_hex(0x8899aa), 0);
    lv_obj_t *lbl_f = lv_label_create(bottom); lv_label_set_text(lbl_f, "F: ---"); lv_obj_set_style_text_color(lbl_f, lv_color_hex(0xffffff), 0);

    ESP_LOGI(TAG,"Premium UI created");
    lvgl_port_unlock();
}

void ui_dashboard_update(int speed_kmh, int batt_pct, float batt_v, int rssi, int latency_ms, const char *mode, bool hazard, int rt_pct, int lt_pct, int steer_deg, bool btn_a, bool btn_b, bool btn_x, bool btn_y){
    if(!lvgl_port_lock(0)) return;
    char buf[32];
    snprintf(buf,sizeof(buf), "%.1fV %d%%", batt_v, batt_pct);
    lv_label_set_text(lbl_batt, buf);
    if(batt_pct>50) lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0x00ff88), 0);
    else if(batt_pct>20) lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xffcc00), 0);
    else lv_obj_set_style_text_color(lbl_batt, lv_color_hex(0xff0044), 0);

    lv_label_set_text(lbl_mode, mode);
    if(strcmp(mode,"ECO")==0) lv_obj_set_style_bg_color(lbl_mode, lv_color_hex(0x00e8cc), 0);
    else if(strcmp(mode,"SPORT")==0) lv_obj_set_style_bg_color(lbl_mode, lv_color_hex(0xff0044), 0);
    else lv_obj_set_style_bg_color(lbl_mode, lv_color_hex(0xffaa00), 0);

    snprintf(buf,sizeof(buf), "%03d", speed_kmh);
    lv_label_set_text(lbl_speed, buf);
    lv_arc_set_value(arc_speed, speed_kmh>100?100:speed_kmh);
    // color shift with speed
    if(speed_kmh<30) lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0x00e8cc), LV_PART_INDICATOR);
    else if(speed_kmh<70) lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0xffcc00), LV_PART_INDICATOR);
    else lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0xff0044), LV_PART_INDICATOR);

    char sbuf[32]; snprintf(sbuf,sizeof(sbuf), "%c %2d  %c", steer_deg<-5?'<':' ', abs(steer_deg), steer_deg>5?'>':' ');
    lv_label_set_text(lbl_steer, sbuf);

    lv_bar_set_value(bar_rt, rt_pct, LV_ANIM_OFF);
    lv_bar_set_value(bar_lt, lt_pct, LV_ANIM_OFF);
    // hazard blink
    if(hazard && (lv_tick_get()%1000 < 500)) lv_obj_set_style_text_color(lbl_top_title, lv_color_hex(0xff0044), 0);
    else lv_obj_set_style_text_color(lbl_top_title, lv_color_hex(0x00e8cc), 0);

    lvgl_port_unlock();
}
void ui_dashboard_set_battery(float v){ (void)v; }
