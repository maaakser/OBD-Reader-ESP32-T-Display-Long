# PL
# OBD_skaner (planowane)

Wyświetlacz temperatury płynu chłodzącego i oleju (aktualnie) płytka ESP32
Dane pobierane są przez Bluetooth LE z adaptera OBD-II 

## Funkcje

- Automatyczne wyszukiwanie i łączenie tylko z moim adapterem Veepeak (zgodny adres MAC i nazwa `VEEPEAK`, adres ustawiany w `adapterMAC` w `main.cpp`)
- Ekran statusu do momentu połączenia (szukanie, łączenie, inicjalizacja adaptera)
- Odpytywanie sterownika silnika BMW na zmianę (opis zapytań niżej):
  - `2C100005` – temperatura płynu chłodzącego
  - `2C100458` – temperatura oleju silnikowego

## Odczyty BMW (E82, N47, sterownik DDE)

### Inicjalizacja ELM327 
Każda komenda osobno, czekać na `>`:
```
ATD
ATE0
ATS0
ATL0
ATAL
ATAT0
ATH1
ATPB C101           własny protokół: CAN 11-bit, 500 kbit/s
ATCRA 612           odbieraj tylko odpowiedzi z ID 0x612 (DDE)
ATSH 6F1            wysyłaj z ID 0x6F1 (tester)
ATFC SH6F1
ATFCSD 12300002
ATFC SM1
ATCEA 12            adresowanie rozszerzone, cel 0x12 = DDE
ATCM 600
ATCF 600
ATSP B
ATBI
ATAT0
ATST23
```

### Zapytania
Zapytanie `2C10xxxx`, odpowiedź w formacie `612 F1 len 6C 10 A B`, gdzie A i B to bajty danych po `6C10`.

| Parametr | Zapytanie | Wzór | Jednostka | Uwagi |
|---|---|---|---|---|
| Temp. oleju | `2C100458` | `((A<<8)\|B) * 0.01 - 100` | °C | np. `2B20` = 10,4 °C |
| Temp. płynu chłodzącego | `2C100005` | `A - 40` | °C | jeden bajt, np. `3A` = 18 °C |
| Popiół w DPF | `2C1003E9` | `((A<<8)\|B) / 65.536` | g | np. `0317` = 12,1 g |
| Sadza w DPF | `2C1003EA` | `((A<<8)\|B) / 65.536` | g | np. `00DE` = 3,4 g |
| Ciśnienie wsteczne DPF | `2C10012C` | `(((A<<8)\|B) - 32768) * 0.01` | hPa | np. `84B7` = 12,1 hPa |
| Napięcie akumulatora | `2C100426` | `((A<<8)\|B) * 10 / 16384` | V | np. `565B` = 13,5 V |

Wzory do potwierdzenia w szerszym zakresie (ciepły silnik, wyższe ciśnienie DPF).

Zapytanie `1A80` zwraca identyfikację sterownika (niepotrzebne do wyświetlacza).

## Struktura

```
board/        definicja płytki T-Display-Long dla PlatformIO
lib/          LVGL 8.3 (+ lv_conf.h) i sterownik dotyku hyn (CST3530)
src/          main.cpp, sterownik ekranu AXS15231B, piny, font z polskimi znakami
```

## Źródła
Sterowniki itp z repozytorium [Xinyuan-LilyGO/T-Display-S3-Long](https://github.com/Xinyuan-LilyGO/T-Display-S3-Long).
LVGL: [lvgl/lvgl](https://github.com/lvgl/lvgl) (licencja MIT).


# ENG
# OBD_skaner

Coolant and oil temperature display (for now) on an ESP32 board.
Data is read over Bluetooth LE from an OBD-II adapter.

## Features

- Automatic scanning and connection to my Veepeak adapter only (matching MAC address and name `VEEPEAK`; the address is set in `adapterMAC` in `main.cpp`)
- Status screen until connected (searching, connecting, adapter initialization)
- Alternating requests to the BMW engine ECU (requests described below):
  - `2C100005` – engine coolant temperature
  - `2C100458` – engine oil temperature

## BMW readings (E82, N47, DDE engine ECU)

### ELM327 initialization (NOT TESTED)
Send each command separately and wait for `>`:
```
ATD
ATE0
ATS0
ATL0
ATAL
ATAT0
ATH1
ATPB C101           user protocol: CAN 11-bit, 500 kbit/s
ATCRA 612           only accept responses from ID 0x612 (DDE)
ATSH 6F1            send from ID 0x6F1 (tester)
ATFC SH6F1
ATFCSD 12300002
ATFC SM1
ATCEA 12            extended addressing, target 0x12 = DDE
ATCM 600
ATCF 600
ATSP B
ATBI
ATAT0
ATST23
```

### Requests
A request has the form `2C10xxxx` and the response has the form `612 F1 len 6C 10 A B`, where A and B are the data bytes after `6C10`.

| Parameter | Request | Formula | Unit | Notes |
|---|---|---|---|---|
| Oil temperature | `2C100458` | `((A<<8)\|B) * 0.01 - 100` | °C | e.g. `2B20` = 10.4 °C |
| Coolant temperature | `2C100005` | `A - 40` | °C | single byte, e.g. `3A` = 18 °C |
| DPF ash | `2C1003E9` | `((A<<8)\|B) / 65.536` | g | e.g. `0317` = 12.1 g |
| DPF soot | `2C1003EA` | `((A<<8)\|B) / 65.536` | g | e.g. `00DE` = 3.4 g |
| DPF back pressure | `2C10012C` | `(((A<<8)\|B) - 32768) * 0.01` | hPa | e.g. `84B7` = 12.1 hPa |
| Battery voltage | `2C100426` | `((A<<8)\|B) * 10 / 16384` | V | e.g. `565B` = 13.5 V |

The formulas still need confirming over a wider range (warm engine, higher DPF pressure).

The `1A80` request returns the ECU identification (not needed for the display).

## Structure

```
board/        T-Display-Long board definition for PlatformIO
lib/          LVGL 8.3 (+ lv_conf.h) and hyn touch driver (CST3530)
src/          main.cpp, AXS15231B display driver, pin config, font with Polish characters
```

## Credits

Drivers etc. taken from the [Xinyuan-LilyGO/T-Display-S3-Long](https://github.com/Xinyuan-LilyGO/T-Display-S3-Long) repository.
LVGL: [lvgl/lvgl](https://github.com/lvgl/lvgl) (MIT license).
