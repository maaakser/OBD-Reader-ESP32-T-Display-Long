#include <Arduino.h>
#include <Wire.h>
#include "AXS15231B.h"
#include "hyn_core.h"
#include "lvgl.h"

#include <NimBLEDevice.h>

LV_FONT_DECLARE(font_montserrat_14_pl);

// Log na Serial z czasem od startu w ms
#define LOG(fmt, ...) Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__)

// Odpowiedź ELM w jednej linii (\r i \n zamienione na spacje), do logów
static const char *one_line(char *s)
{
  for (char *p = s; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
  return s;
}

// Adres MAC adaptera Veepeak (adres publiczny)
static const char *adapterMAC = "8c:de:52:da:70:3a";

// Usługa ELM w Veepeak (sprawdzone na adapterze): FFF2 zapis, FFF1 notify.
// Adapter ma też usługę Microchip 49535343-..., ale ELM na niej nie odpowiada.
static NimBLEUUID serviceUUID((uint16_t)0xFFF0);
static NimBLEUUID charRxUUID((uint16_t)0xFFF2); // Write
static NimBLEUUID charTxUUID((uint16_t)0xFFF1); // Notify

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

// Ekran statusu połączenia
static lv_obj_t *panel_status = NULL;
static lv_obj_t *ui_status_label = NULL;

static NimBLEClient* pClient = nullptr;
static NimBLERemoteCharacteristic* pRemoteWrite = nullptr;
static NimBLERemoteCharacteristic* pRemoteNotify = nullptr;
static NimBLEAddress targetAddress;
static bool write_with_response = false;

static volatile bool doConnect = false;
static volatile bool connected = false;

// Stan połączenia widoczny dla UI
enum ConnState { CS_SEARCHING, CS_CONNECTING, CS_INIT, CS_READY };
static volatile ConnState conn_state = CS_SEARCHING;
static volatile int scan_attempt = 0;

// Zmienne przechowujące odczyty
static int coolant_temp = -999;
static int oil_temp = -999;
static bool new_data_flag = false;
static SemaphoreHandle_t data_mutex = NULL;

// Bufor odpowiedzi z ELM327, kompletna odpowiedź kończy się znakiem '>'
static char rx_buf[256];
static size_t rx_len = 0;
static SemaphoreHandle_t rx_ready = NULL;

// Zapytania BMW DDE (opis w README)
static const char *REQ_OIL     = "2C100458";
static const char *REQ_COOLANT = "2C100005";

// Inicjalizacja ELM327 pod sterownik silnika BMW
static const char *init_cmds[] = {
  "ATD", "ATE0", "ATS0", "ATL0", "ATAL", "ATAT0", "ATH1",
  "ATPB C101", "ATCRA 612", "ATSH 6F1",
  "ATFC SH6F1", "ATFCSD 12300002", "ATFC SM1",
  "ATCEA 12", "ATCM 600", "ATCF 600", "ATSP B", "ATBI", "ATAT0", "ATST23"
};

static void notifyCallback(NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify)
{
  if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    bool prompt = false;
    for (size_t i = 0; i < length; i++) {
      if (rx_len < sizeof(rx_buf) - 1) rx_buf[rx_len++] = (char)pData[i];
      if (pData[i] == '>') prompt = true;
    }
    rx_buf[rx_len] = '\0';
    xSemaphoreGive(data_mutex);

    if (prompt) xSemaphoreGive(rx_ready);
  }
}

// Wysyła komendę i czeka na znak zachęty '>'. Zwraca false przy przekroczeniu czasu.
static bool send_cmd(const char *cmd, char *out, size_t out_len, uint32_t timeout_ms)
{
  if (!connected || pRemoteWrite == nullptr) return false;

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  rx_len = 0;
  rx_buf[0] = '\0';
  xSemaphoreGive(data_mutex);
  xSemaphoreTake(rx_ready, 0);

  char line[32];
  snprintf(line, sizeof(line), "%s\r", cmd);
  pRemoteWrite->writeValue((uint8_t*)line, strlen(line), write_with_response);

  bool ok = xSemaphoreTake(rx_ready, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

  xSemaphoreTake(data_mutex, portMAX_DELAY);
  strncpy(out, rx_buf, out_len - 1);
  out[out_len - 1] = '\0';
  xSemaphoreGive(data_mutex);
  return ok;
}

// Odpowiedź "612 F1 len 6C 10 A B" (bez spacji, bo ATS0): zwraca bajty danych po "6C10"
static bool parse_6c10(const char *resp, int nbytes, long *value)
{
  const char *p = strstr(resp, "6C10");
  if (p == nullptr) return false;
  p += 4;

  char hex[5] = {0};
  for (int i = 0; i < nbytes * 2; i++) {
    if (!isxdigit((unsigned char)p[i])) return false;
    hex[i] = p[i];
  }
  *value = strtol(hex, NULL, 16);
  return true;
}

class MyClientCallback : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) {
    connected = true;
    LOG("BLE: połączono");
  }
  void onDisconnect(NimBLEClient* pclient) {
    connected = false;
    LOG("BLE: rozłączono");
  }
};

class MyAdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
    String devName = advertisedDevice->haveName() ? advertisedDevice->getName().c_str() : "";

    // Tylko mój adapter Veepeak: zgodny adres MAC i nazwa "VEEPEAK"
    if (advertisedDevice->getAddress().toString() == adapterMAC && devName == "VEEPEAK") {
      LOG("SKAN: znaleziono VEEPEAK, RSSI %d", advertisedDevice->getRSSI());
      NimBLEDevice::getScan()->stop();
      targetAddress = advertisedDevice->getAddress();
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

  // EKRAN STATUSU: przykrywa cały ekran, dopóki adapter nie jest gotowy
  panel_status = lv_obj_create(scr);
  lv_obj_set_size(panel_status, 640, 180);
  lv_obj_set_pos(panel_status, 0, 0);
  lv_obj_set_style_bg_color(panel_status, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel_status, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(panel_status, 0, LV_PART_MAIN);
  lv_obj_clear_flag(panel_status, LV_OBJ_FLAG_SCROLLABLE);

  ui_status_label = lv_label_create(panel_status);
  lv_obj_set_style_text_color(ui_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(ui_status_label, &font_montserrat_14_pl, LV_PART_MAIN);
  lv_obj_set_style_text_align(ui_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(ui_status_label, "Nie połączono\nSzukam adaptera VEEPEAK...");
  lv_obj_center(ui_status_label);
}

// Zmienia napis i widoczność ekranów zależnie od stanu połączenia (wywoływane z loop)
static void update_status_ui(ConnState st, int attempt)
{
  char txt[96];

  switch (st) {
    case CS_SEARCHING:
      snprintf(txt, sizeof(txt), "Nie połączono\nSzukam adaptera VEEPEAK... (próba %d)", attempt);
      break;
    case CS_CONNECTING:
      snprintf(txt, sizeof(txt), "Nie połączono\nŁączenie z adapterem VEEPEAK...");
      break;
    case CS_INIT:
      snprintf(txt, sizeof(txt), "Połączono\nInicjalizacja adaptera...");
      break;
    case CS_READY:
      lv_obj_add_flag(panel_status, LV_OBJ_FLAG_HIDDEN);
      return;
  }

  lv_label_set_text(ui_status_label, txt);
  lv_obj_center(ui_status_label);
  lv_obj_clear_flag(panel_status, LV_OBJ_FLAG_HIDDEN);
}

// Po rozłączeniu kasuje stare odczyty, żeby nie wisiały nieaktualne wartości
static void reset_values_ui(void)
{
  coolant_temp = -999;
  oil_temp = -999;
  lv_label_set_text(ui_coolant_val, "-- °C");
  lv_label_set_text(ui_oil_val, "-- °C");
  lv_bar_set_value(ui_coolant_bar, 40, LV_ANIM_OFF);
  lv_bar_set_value(ui_oil_bar, 40, LV_ANIM_OFF);
}

bool connectToServer() {
  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
  }

  if (!pClient->connect(targetAddress)) {
    LOG("BLE: connect() nieudany");
    return false;
  }

  pRemoteWrite = nullptr;
  pRemoteNotify = nullptr;

  // Najpierw znana usługa FFF0
  NimBLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService != nullptr) {
    pRemoteWrite = pRemoteService->getCharacteristic(charRxUUID);
    pRemoteNotify = pRemoteService->getCharacteristic(charTxUUID);
  }

  // Inaczej: wypisz wszystkie usługi i weź pierwszą, która ma notify i zapis (np. FFF0: FFF1/FFF2)
  if (pRemoteWrite == nullptr || pRemoteNotify == nullptr) {
    std::vector<NimBLERemoteService*> *services = pClient->getServices(true);
    LOG("BLE: brak FFF0, usług na adapterze: %d", services ? (int)services->size() : -1);
    if (services == nullptr) {
      pClient->disconnect();
      return false;
    }

    for (NimBLERemoteService *svc : *services) {
      LOG("  usługa %s", svc->getUUID().toString().c_str());
      NimBLERemoteCharacteristic *w = nullptr, *n = nullptr;
      std::vector<NimBLERemoteCharacteristic*> *chars = svc->getCharacteristics(true);
      if (chars == nullptr) continue;

      for (NimBLERemoteCharacteristic *c : *chars) {
        LOG("    char %s %s%s%s%s%s", c->getUUID().toString().c_str(),
            c->canRead() ? "R " : "", c->canWrite() ? "W " : "",
            c->canWriteNoResponse() ? "WnR " : "", c->canNotify() ? "N " : "",
            c->canIndicate() ? "I " : "");
        if (w == nullptr && (c->canWrite() || c->canWriteNoResponse())) w = c;
        if (n == nullptr && (c->canNotify() || c->canIndicate())) n = c;
      }

      // Usługi standardowe (0x18xx: GAP, GATT, Device Info...) pomijamy
      uint16_t id = svc->getUUID().bitSize() == 16 ? svc->getUUID().getNative()->u16.value : 0;
      bool standard = id >= 0x1800 && id <= 0x18FF && id != 0x18F0;
      if (!standard && w != nullptr && n != nullptr && pRemoteWrite == nullptr) {
        pRemoteWrite = w;
        pRemoteNotify = n;
      }
    }
  }

  if (pRemoteWrite == nullptr || pRemoteNotify == nullptr) {
    LOG("BLE: nie znaleziono charakterystyk do komunikacji z ELM");
    pClient->disconnect();
    return false;
  }

  write_with_response = !pRemoteWrite->canWriteNoResponse();
  LOG("BLE: zapis -> %s%s, odbiór <- %s", pRemoteWrite->getUUID().toString().c_str(),
      write_with_response ? " (z potwierdzeniem)" : "",
      pRemoteNotify->getUUID().toString().c_str());

  if (!pRemoteNotify->subscribe(pRemoteNotify->canNotify(), notifyCallback)) {
    LOG("BLE: nie udało się włączyć notify");
    pClient->disconnect();
    return false;
  }

  return true;
}

void ble_comm_task(void *param)
{
  NimBLEDevice::init("T-Display-S3");
  NimBLEScan* pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->setActiveScan(true);

  bool ask_oil = true;
  char resp[256];

  while (1)
  {
    // 1. Brak połączenia: skanowanie (5 s na próbę)
    if (!connected && !doConnect) {
      conn_state = CS_SEARCHING;
      scan_attempt = scan_attempt + 1;
      LOG("SKAN: próba %d", scan_attempt);
      pBLEScan->start(5, false);
      pBLEScan->clearResults();
    }

    // 2. Adapter znaleziony: połączenie i inicjalizacja ELM327
    if (doConnect) {
      doConnect = false;
      conn_state = CS_CONNECTING;
      LOG("BLE: łączenie...");

      if (connectToServer()) {
        conn_state = CS_INIT;
        LOG("ELM: inicjalizacja");
        for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]) && connected; i++) {
          bool ok = send_cmd(init_cmds[i], resp, sizeof(resp), 2000);
          LOG("ELM: %s -> %s", init_cmds[i], ok ? one_line(resp) : "(timeout)");
          vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (connected) {
          conn_state = CS_READY;
          scan_attempt = 0;
          LOG("ELM: gotowy, start odczytów");
        }
      } else {
        if (pClient != nullptr && pClient->isConnected()) pClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
    }

    // 3. Gotowy: naprzemiennie olej i płyn
    if (connected && conn_state == CS_READY) {
      const char *req = ask_oil ? REQ_OIL : REQ_COOLANT;
      if (send_cmd(req, resp, sizeof(resp), 1000)) {
        long raw;
        one_line(resp);
        if (ask_oil && parse_6c10(resp, 2, &raw)) {
          xSemaphoreTake(data_mutex, portMAX_DELAY);
          oil_temp = (int)lroundf(raw * 0.01f - 100.0f);
          new_data_flag = true;
          xSemaphoreGive(data_mutex);
          LOG("OLEJ: %d C (raw 0x%04lX) | %s", oil_temp, raw, resp);
        } else if (!ask_oil && parse_6c10(resp, 1, &raw)) {
          xSemaphoreTake(data_mutex, portMAX_DELAY);
          coolant_temp = (int)(raw - 40);
          new_data_flag = true;
          xSemaphoreGive(data_mutex);
          LOG("PLYN: %d C (raw 0x%02lX) | %s", coolant_temp, raw, resp);
        } else {
          LOG("BLAD PARSOWANIA %s -> %s", req, resp);
        }
      } else {
        LOG("TIMEOUT %s -> %s", req, one_line(resp));
      }
      ask_oil = !ask_oil;
      vTaskDelay(pdMS_TO_TICKS(250));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void setup()
{
  Serial.begin(115200);
  // Bez laptopa printf nie może blokować taska BLE
  Serial.setTxTimeoutMs(0);
  // Chwila na otwarcie monitora, żeby nie zgubić logów ze startu
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  LOG("START OBD_skaner");

  data_mutex = xSemaphoreCreateMutex();
  rx_ready = xSemaphoreCreateBinary();

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

  // Ekran statusu: odświeżany tylko przy zmianie stanu lub numeru próby
  static ConnState last_state = CS_READY;
  static int last_attempt = -1;
  ConnState st = conn_state;
  int attempt = scan_attempt;
  if (st != last_state || attempt != last_attempt) {
    if (st == CS_SEARCHING && last_state == CS_READY) {
      xSemaphoreTake(data_mutex, portMAX_DELAY);
      reset_values_ui();
      new_data_flag = false;
      xSemaphoreGive(data_mutex);
    }
    update_status_ui(st, attempt);
    last_state = st;
    last_attempt = attempt;
  }

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