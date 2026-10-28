# OBD_skaner (planowane)

Wyświetlacz temperatury płynu chłodzącego i oleju (aktualnie) płytka ESP32
Dane pobierane są przez Bluetooth LE z adaptera OBD-II 

## Funkcje

- Automatyczne wyszukiwanie i łączenie z adapterem OBD (nazwa zawiera `VEEPEAK` / `OBD` lub reklamuje usługę NUS, aktualnie przetestowane tylko z telefonem)
- Odpytywanie PID-ów co 500 ms:
  - `0105` – temperatura płynu chłodzącego
  - `015C` – temperatura oleju silnikowego

## Struktura

```
board/        definicja płytki T-Display-Long dla PlatformIO
lib/          LVGL 8.3 (+ lv_conf.h) i sterownik dotyku hyn (CST3530)
src/          main.cpp, sterownik ekranu AXS15231B, piny, font z polskimi znakami
```

## Źródła
Sterowniki itp z repozytorium [Xinyuan-LilyGO/T-Display-S3-Long](https://github.com/Xinyuan-LilyGO/T-Display-S3-Long).
LVGL: [lvgl/lvgl](https://github.com/lvgl/lvgl) (licencja MIT).
