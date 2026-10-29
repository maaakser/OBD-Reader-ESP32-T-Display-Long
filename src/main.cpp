#include <Arduino.h>
#include <Wire.h>
#include "AXS15231B.h"
#include "hyn_core.h"
#include "lvgl.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>

LV_FONT_DECLARE(font_montserrat_14_pl);

// Adres MAC adaptera Veepeak (adres publiczny)
static BLEAddress adapterMAC("8c:de:52:da:70:3a");

// Nordic UART Service (Veepeak BLE)
static BLEUUID serviceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID charRxUUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"); // Write
static BLEUUID charTxUUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"); // Notify

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf;
static lv_color_t *buf1;
struct hyn_ts_data *ts_data;

// Elementy interfejsu LVGL
static lv_obj_t *panel_coolant = NULL;
static lv_obj_t *panel_oil = NULL;

static lv_obj_t *ui_coolant_val = NULL;
static lv_obj_t *ui_coolant_bar = NULL;
static lv_obj_t *ui_oil_val = NULL;
static lv_obj_t *ui_oil_bar = NULL;

static BLERemoteCharacteristic* pRemoteWrite = nullptr;
static BLERemoteCharacteristic* pRemoteNotify = nullptr;
static BLEAdvertisedDevice* targetDevice = nullptr;

static bool doConnect = false;
static bool connected = false;

// Zmienne przechowujące odczyty
static int coolant_temp = -999;
static int oil_temp = -999;
static bool new_data_flag = false;
static SemaphoreHandle_t data_mutex = NULL;

// Funkcja parsująca ramki OBD-II (zarówno 41 05 jak i 41 5C)
void parse_obd_response(const char *raw)
{
  String s = raw;
  s.replace(" ", "");
  s.trim();

  // Temperatura płynu chłodzącego: 41 05 XX
  int idx_coolant = s.indexOf("4105");
  if (idx_coolant >= 0 && s.length() >= (size_t)(idx_coolant + 6))
  {
    String hexVal = s.substring(idx_coolant + 4, idx_coolant + 6);
    long val = strtol(hexVal.c_str(), NULL, 16);
    coolant_temp = (int)(val - 40);
    new_data_flag = true;
  }

  // Temperatura oleju: 41 5C XX
  int idx_oil = s.indexOf("415C");
  if (idx_oil >= 0 && s.length() >= (size_t)(idx_oil + 6))
  {
    String hexVal = s.substring(idx_oil + 4, idx_oil + 6);
    long val = strtol(hexVal.c_str(), NULL, 16);
    oil_temp = (int)(val - 40);
    new_data_flag = true;
  }
}

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify)
{
  if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    char tmp[64];
    size_t copy_len = (length < sizeof(tmp) - 1) ? length : (sizeof(tmp) - 1);
    memcpy(tmp, pData, copy_len);
    tmp[copy_len] = '\0';

    parse_obd_response(tmp);
    xSemaphoreGive(data_mutex);
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    connected = true;
  }
  void onDisconnect(BLEClient* pclient) {
    connected = false;
  }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String devName = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "";

    // Tylko mój adapter Veepeak: zgodny adres MAC i nazwa "VEEPEAK"
    if (advertisedDevice.getAddress().equals(adapterMAC) && devName == "VEEPEAK") {
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#ifdef LCD_SPI_DMA
  char i = 0;
  while (get_lcd_spi_dma_write())
  {
    i = i >> 1;
    lcd_PushColors(0, 0, 0, 0, NULL);
  }
#endif
  lcd_PushColors(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);

#ifdef LCD_SPI_DMA
#else
  lv_disp_flush_ready(disp);
#endif
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  ts_data = get_touch_data();
  if (ts_data != NULL && ts_data->rp_buf.rep_num > 0) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = ts_data->rp_buf.pos_info[0].pos_x;
    data->point.y = ts_data->rp_buf.pos_info[0].pos_y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void setup_ui(void)
{
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  // Ekran podzielony na 2 połowy po 320 px: lewa - płyn, prawa - olej
  panel_coolant = lv_obj_create(scr);
  lv_obj_set_size(panel_coolant, 320, 180);
  lv_obj_set_pos(panel_coolant, 0, 0);
  lv_obj_set_style_bg_color(panel_coolant, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel_coolant, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(panel_coolant, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel_coolant, 0, LV_PART_MAIN);
  lv_obj_clear_flag(panel_coolant, LV_OBJ_FLAG_SCROLLABLE);

  panel_oil = lv_obj_create(scr);
  lv_obj_set_size(panel_oil, 320, 180);
  lv_obj_set_pos(panel_oil, 320, 0);
  lv_obj_set_style_bg_color(panel_oil, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_radius(panel_oil, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel_oil, 0, LV_PART_MAIN);
  lv_obj_clear_flag(panel_oil, LV_OBJ_FLAG_SCROLLABLE);
  // Pionowa linia oddzielająca połowy
  lv_obj_set_style_border_width(panel_oil, 2, LV_PART_MAIN);
  lv_obj_set_style_border_side(panel_oil, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel_oil, lv_color_hex(0x333333), LV_PART_MAIN);

  // LEWA POŁOWA: TEMPERATURA PŁYNU
  lv_obj_t *badge1 = lv_label_create(panel_coolant);
  lv_obj_align(badge1, LV_ALIGN_TOP_LEFT, 20, 12);
  lv_obj_set_style_text_color(badge1, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_text_font(badge1, &font_montserrat_14_pl, LV_PART_MAIN);
  lv_label_set_text(badge1, "PŁYN CHŁODZĄCY");

  ui_coolant_val = lv_label_create(panel_coolant);
  lv_obj_align(ui_coolant_val, LV_ALIGN_LEFT_MID, 20, -5);
  lv_obj_set_style_text_color(ui_coolant_val, lv_color_hex(0x00E5FF), LV_PART_MAIN);
  lv_obj_set_style_text_font(ui_coolant_val, &font_montserrat_14_pl, LV_PART_MAIN);
  lv_label_set_text(ui_coolant_val, "-- °C");

  ui_coolant_bar = lv_bar_create(panel_coolant);
  lv_obj_set_size(ui_coolant_bar, 280, 26);
  lv_obj_align(ui_coolant_bar, LV_ALIGN_BOTTOM_MID, 0, -25);
  lv_bar_set_range(ui_coolant_bar, 40, 120);
  lv_bar_set_value(ui_coolant_bar, 40, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ui_coolant_bar, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui_coolant_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);

  // PRAWA POŁOWA: TEMPERATURA OLEJU
  lv_obj_t *badge2 = lv_label_create(panel_oil);
  lv_obj_align(badge2, LV_ALIGN_TOP_LEFT, 20, 12);
  lv_obj_set_style_text_color(badge2, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_text_font(badge2, &font_montserrat_14_pl, LV_PART_MAIN);
  lv_label_set_text(badge2, "OLEJ SILNIKOWY");

  ui_oil_val = lv_label_create(panel_oil);
  lv_obj_align(ui_oil_val, LV_ALIGN_LEFT_MID, 20, -5);
  lv_obj_set_style_text_color(ui_oil_val, lv_color_hex(0xFF9900), LV_PART_MAIN);
  lv_obj_set_style_text_font(ui_oil_val, &font_montserrat_14_pl, LV_PART_MAIN);
  lv_label_set_text(ui_oil_val, "-- °C");

  ui_oil_bar = lv_bar_create(panel_oil);
  lv_obj_set_size(ui_oil_bar, 280, 26);
  lv_obj_align(ui_oil_bar, LV_ALIGN_BOTTOM_MID, 0, -25);
  lv_bar_set_range(ui_oil_bar, 40, 140);
  lv_bar_set_value(ui_oil_bar, 40, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ui_oil_bar, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui_oil_bar, lv_color_hex(0xFF9900), LV_PART_INDICATOR);
}

bool connectToServer() {
  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(targetDevice)) return false;

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    pClient->disconnect();
    return false;
  }

  pRemoteWrite = pRemoteService->getCharacteristic(charRxUUID);
  pRemoteNotify = pRemoteService->getCharacteristic(charTxUUID);

  if (pRemoteWrite == nullptr || pRemoteNotify == nullptr) {
    pClient->disconnect();
    return false;
  }

  if (pRemoteNotify->canNotify()) {
    pRemoteNotify->registerForNotify(notifyCallback);
  }

  return true;
}

void ble_comm_task(void *param)
{
  BLEDevice::init("T-Display-S3");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->setActiveScan(true);

  int toggle_pid = 0;

  while (1)
  {
    if (!connected && !doConnect) {
      pBLEScan->start(5, false);
      pBLEScan->clearResults();
    }

    if (doConnect) {
      if (connectToServer()) {
        doConnect = false;
      } else {
        doConnect = false;
      }
    }

    // Naprzemienne zapytania co 500 ms: raz płyn (0105), raz olej (015C)
    if (connected && pRemoteWrite != nullptr) {
      if (toggle_pid == 0) {
        const char* cmd = "0105\r";
        pRemoteWrite->writeValue((uint8_t*)cmd, strlen(cmd), false);
        toggle_pid = 1;
      } else {
        const char* cmd = "015C\r";
        pRemoteWrite->writeValue((uint8_t*)cmd, strlen(cmd), false);
        toggle_pid = 0;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup()
{
  Serial.begin(115200);
  data_mutex = xSemaphoreCreateMutex();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  axs15231_init();
  touch_init(TP_I2C_SDA, TP_I2C_SCL, TP_IRQ, TP_RST);

  lv_init();

  size_t buffer_size = sizeof(lv_color_t) * EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES;
  buf = (lv_color_t *)ps_malloc(buffer_size);
  buf1 = (lv_color_t *)ps_malloc(buffer_size);

  lv_disp_draw_buf_init(&draw_buf, buf, buf1, buffer_size);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.sw_rotate = 1;
  disp_drv.rotated = LV_DISP_ROT_90;
  disp_drv.full_refresh = 1;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  setup_ui();

  xTaskCreatePinnedToCore(ble_comm_task, "ble_comm", 10240, NULL, 1, NULL, 0);
}

extern uint32_t transfer_num;
extern size_t lcd_PushColors_len;

void loop()
{
  delay(2);

  if (new_data_flag) {
    if (xSemaphoreTake(data_mutex, 0) == pdTRUE) {
      
      // Aktualizacja Karty 1 (Płyn chłodzący)
      if (coolant_temp != -999) {
        char buf_c[32];
        snprintf(buf_c, sizeof(buf_c), "%d °C", coolant_temp);
        lv_label_set_text(ui_coolant_val, buf_c);
        lv_bar_set_value(ui_coolant_bar, coolant_temp, LV_ANIM_ON);

        if (coolant_temp < 75) {
          lv_obj_set_style_text_color(ui_coolant_val, lv_color_hex(0x00E5FF), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_coolant_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
        } else if (coolant_temp <= 100) {
          lv_obj_set_style_text_color(ui_coolant_val, lv_color_hex(0x00FF66), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_coolant_bar, lv_color_hex(0x00FF66), LV_PART_INDICATOR);
        } else {
          lv_obj_set_style_text_color(ui_coolant_val, lv_color_hex(0xFF3333), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_coolant_bar, lv_color_hex(0xFF3333), LV_PART_INDICATOR);
        }
      }

      // Aktualizacja Karty 2 (Olej silnikowy)
      if (oil_temp != -999) {
        char buf_o[32];
        snprintf(buf_o, sizeof(buf_o), "%d °C", oil_temp);
        lv_label_set_text(ui_oil_val, buf_o);
        lv_bar_set_value(ui_oil_bar, oil_temp, LV_ANIM_ON);

        if (oil_temp < 75) {
          lv_obj_set_style_text_color(ui_oil_val, lv_color_hex(0x00E5FF), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_oil_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
        } else if (oil_temp <= 110) {
          lv_obj_set_style_text_color(ui_oil_val, lv_color_hex(0x00FF66), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_oil_bar, lv_color_hex(0x00FF66), LV_PART_INDICATOR);
        } else {
          lv_obj_set_style_text_color(ui_oil_val, lv_color_hex(0xFF3333), LV_PART_MAIN);
          lv_obj_set_style_bg_color(ui_oil_bar, lv_color_hex(0xFF3333), LV_PART_INDICATOR);
        }
      }

      new_data_flag = false;
      xSemaphoreGive(data_mutex);
    }
  }

  if (transfer_num <= 0 && lcd_PushColors_len <= 0)
    lv_timer_handler();

  if (transfer_num <= 1 && lcd_PushColors_len > 0) {
    lcd_PushColors(0, 0, 0, 0, NULL);
  }
}