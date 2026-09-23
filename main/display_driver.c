#include "display_driver.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG="disp_drv";

// Use same pins as tft_display (free pins, no conflict with car.c)
// SCLK46 MOSI13 CS45 DC0 RST16 BL18  320x240 landscape, 40MHz DMA
#define LCD_H_RES 320
#define LCD_V_RES 240
#define PIN_SCLK 46
#define PIN_MOSI 13
#define PIN_CS 45
#define PIN_DC 0
#define PIN_RST 16
#define PIN_BL 18
#define LCD_SPI_HOST SPI2_HOST

static lv_disp_t *s_disp = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

extern bool tft_is_ready(void);
extern struct esp_lcd_panel_t* tft_get_panel(void);
extern struct esp_lcd_panel_io_t* tft_get_io(void);

esp_err_t display_driver_init(void){
    ESP_LOGI(TAG,"LVGL display init %dx%d SPI2 SCLK%d MOSI%d", LCD_H_RES, LCD_V_RES, PIN_SCLK, PIN_MOSI);
    // If tft_display already inited (boot anim), reuse its panel/io
    if(tft_is_ready()){
        s_panel = (esp_lcd_panel_handle_t)tft_get_panel();
        s_io = (esp_lcd_panel_io_handle_t)tft_get_io();
        ESP_LOGI(TAG,"Reusing existing TFT panel for LVGL");
    } else {
        // BL
        gpio_config_t bl={.pin_bit_mask=1ULL<<PIN_BL,.mode=GPIO_MODE_OUTPUT};
        gpio_config(&bl); gpio_set_level(PIN_BL,1);
        gpio_config_t rst={.pin_bit_mask=1ULL<<PIN_RST,.mode=GPIO_MODE_OUTPUT};
        gpio_config(&rst); gpio_set_level(PIN_RST,0); vTaskDelay(pdMS_TO_TICKS(30)); gpio_set_level(PIN_RST,1); vTaskDelay(pdMS_TO_TICKS(100));
        spi_bus_config_t buscfg={.mosi_io_num=PIN_MOSI,.miso_io_num=-1,.sclk_io_num=PIN_SCLK,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=LCD_H_RES*40*2+8};
        esp_err_t err=spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if(err!=ESP_OK && err!=ESP_ERR_INVALID_STATE){
            ESP_LOGE(TAG,"spi bus init failed %s", esp_err_to_name(err));
            return err;
        }
        esp_lcd_panel_io_spi_config_t io_cfg={.dc_gpio_num=PIN_DC,.cs_gpio_num=PIN_CS,.pclk_hz=40000000,.lcd_cmd_bits=8,.lcd_param_bits=8,.spi_mode=0,.trans_queue_depth=10};
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));
        esp_lcd_panel_dev_config_t panel_cfg={.reset_gpio_num=PIN_RST,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.bits_per_pixel=16};
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    }

    // LVGL port
    const lvgl_port_cfg_t lvgl_cfg={.task_priority=4,.task_stack=6144,.task_affinity=0,.task_max_sleep_ms=500,.timer_period_ms=5};
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg={
        .io_handle=s_io,.panel_handle=s_panel,.buffer_size=LCD_H_RES*40,
        .double_buffer=true,.hres=LCD_H_RES,.vres=LCD_V_RES,.monochrome=false,
        .rotation={.swap_xy=true,.mirror_x=true,.mirror_y=false},
        .flags={.buff_dma=true,.buff_spiram=false,.sw_rotate=false,.full_refresh=false,.direct_mode=false}
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if(!s_disp){ ESP_LOGE(TAG,"lvgl add_disp failed"); return ESP_FAIL; }
    ESP_LOGI(TAG,"LVGL disp added double_buffer DMA");
    return ESP_OK;
}
void display_driver_deinit(void){}
