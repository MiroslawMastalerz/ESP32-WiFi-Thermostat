#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
// Dotyk obsluguje CST816D przez I2C. TFT_eSPI nie potrzebuje pinu TOUCH_CS.
#ifndef DISABLE_ALL_LIBRARY_WARNINGS
#define DISABLE_ALL_LIBRARY_WARNINGS
#endif
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "HomeSpan.h"
#include <NimBLEDevice.h>
#include <ESPmDNS.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "SecureLink.h"

// ============================================================
// DIAGNOSTYKA RAM — -D DIAG=1 (domyslnie wylaczona)
// ============================================================
#ifndef DIAG
#define DIAG 0
#endif

// Tylko metadane etapow BLE; bez kluczy, hasel i zawartosci ramek.
#if DIAG
#define BLE_DIAG(...) Serial.printf("[BLE-PROV] " __VA_ARGS__)
#else
#define BLE_DIAG(...) do {} while (0)
#endif

// ============================================================
// SPRZET: NiceMCU-32S-DEV 2.8IPS (ST7789 + CST816D)
// ============================================================
#define SSID_MAX_LEN 33
#define PASS_MAX_LEN 65
#define MAX_NETWORKS 20

// CST816D: piny potwierdzone w board_config.h producenta.
#define TOUCH_SDA 33
#define TOUCH_SCL 32
#define TOUCH_IRQ 36
#define TOUCH_ADDR 0x15
#define TOUCH_RESET 2   // Wspolny reset LCD i CST816D
#define BACKLIGHT_PIN 25

// Orientacja dotyku z natywnego 240x320 na poziomy ekran 320x240.
// TFT_eSPI setRotation(1): standardowo x=y_raw, y=239-x_raw.
// W razie odwroconych osi zmien tylko TOUCH_ROTATION (0..3).
#define TOUCH_ROTATION 1

// Zachowujemy stare polaczenie BME280: osobna magistrala I2C.
#define BME_SDA 21
#define BME_SCL 22
// Korekta temperatury dodawana do odczytu BME280 (w stopniach C).
static constexpr float BME_TEMP_OFFSET_C = -2.4f;

// Paleta UI: standardowe barwy pochodza z TFT_eSPI (TFT_*).
// Tylko odcienie bez odpowiednika w TFT_eSPI maja wlasne nazwy RGB565.
static constexpr uint16_t UI_BG = 0x1082;
static constexpr uint16_t UI_CARD = 0x2104;
static constexpr uint16_t UI_EDGE = 0x4208;
static constexpr uint16_t UI_MUTED_TEXT = 0xAD55;
static constexpr uint16_t UI_ORANGE = 0xFD20;

const int pwmFreq = 5000;
const int pwmResolution = 8;
const int BRIGHTNESS_LIGHT = 255;

// EN/RESET nie jest programowalnym GPIO.
// -1 oznacza brak dodatkowego przycisku.
constexpr int FACTORY_BUTTON_PIN = -1;
static constexpr int WIFI_STATUS_X = 154; // wieksza wartosc = napis bardziej w prawo
static constexpr int WIFI_LIST_TOP = 65;
static constexpr int WIFI_ROW_STEP = 27;
static constexpr int WIFI_ROW_HEIGHT = 25;

// ============================================================
// NAPISY EKRANOWE
// ============================================================
namespace ScreenText {
// Konfiguracja Slave.
constexpr char TXT_KONFIGURACJA_SLAVE[] = "Konfiguracja Slave";
constexpr char TXT_WYMAGANA_OBECNOSC_SLAVE_BLE[] = "Wymagana obecnosc Slave BLE";
constexpr char TXT_SPRAWDZ_SLAVE_I_PONOW[] = "Sprawdz Slave i ponow.";
constexpr char TXT_TRWA_LACZENIE_SLAVE_Z_WIFI[] = "Trwa laczenie Slave z WiFi";
constexpr char TXT_ANULUJ[] = "ANULUJ";
constexpr char TXT_PONOW[] = "PONOW";

// Tryb awaryjny.
constexpr char TXT_AWARIA_PRZEKAZNIK[] = "AWARIA / PRZEKAZNIK";
constexpr char TXT_AKTYWACJA_PRZEKAZNIKA_IN[] = "Aktywacja przekaznika (IN):";
constexpr char TXT_LOW[] = "LOW";
constexpr char TXT_HIGH[] = "HIGH";
constexpr char TXT_TRYB_PO_UTRACIE_TCP[] = "Tryb po utracie TCP:";
constexpr char TXT_ON[] = "ON";
constexpr char TXT_OFF[] = "OFF";
constexpr char TXT_OSTATNI_STAN[] = "OSTATNI STAN";
constexpr char TXT_PO_ILU_MINUTACH[] = "Po ilu minutach:";
constexpr char TXT_MINUS[] = "-";
constexpr char TXT_CZAS_MINUTY[] = "%u min";
constexpr char TXT_PLUS[] = "+";
constexpr char TXT_OCZEKIWANIE_NA_POTWIERDZENIE_SLAVE[] = "Oczekiwanie na potwierdzenie Slave";
constexpr char TXT_OCZEKIWANIE_NA_POLACZENIE_TCP[] = "Oczekiwanie na polaczenie TCP";
constexpr char TXT_ZAPISANO_W_SLAVE[] = "Zapisano w Slave";
constexpr char TXT_ZAPISZ[] = "ZAPISZ";

// Ustawienia i diagnostyka.
constexpr char TXT_USTAWIENIA_SIECI[] = "Ustawienia sieci";
constexpr char TXT_SSID_FORMAT[] = "SSID: %s";
constexpr char TXT_BRAK_DANYCH[] = "--";
constexpr char TXT_IP_SLAVE_FORMAT[] = "Slave IP: %s";
constexpr char TXT_IP_MASTER_FORMAT[] = "Master IP: %s";
constexpr char TXT_ZMIEN_SIEC[] = "ZMIEN SIEC";
constexpr char TXT_KOD_HOMEKIT[] = "KOD HOMEKIT";
constexpr char TXT_FABRYCZNY_2X[] = "FABRYCZNY (2x)";
constexpr char TXT_POWROT[] = "POWROT";
constexpr char TXT_DIAGNOSTYKA_TCP[] = "Diagnostyka TCP";
constexpr char TXT_TCP_STAN_FORMAT[] = "TCP: %s";
constexpr char TXT_POLACZONO[] = "POLACZONO";
constexpr char TXT_ROZLACZONO[] = "ROZLACZONO";
constexpr char TXT_BLE_WYLACZONE_W_TRYBIE_PRACY[] = "BLE: wylaczone w trybie pracy";

// HomeKit i klawiatura.
constexpr char TXT_KOD_PAROWANIA_HK[] = "Kod parowania HK";
constexpr char TXT_NOWY_KOD[] = "Nowy kod:";
constexpr char TXT_RESTART_ZA_3_SEKUNDY[] = "Restart za 3 sekundy...";
constexpr char TXT_AKTUALNY_KOD[] = "Aktualny kod:";
constexpr char TXT_NOWY_KOD_ZOSTANIE_WYGENEROWANY[] = "Nowy kod zostanie wygenerowany";
constexpr char TXT_I_URZADZENIE_SIE_ZRESTARTUJE[] = "i urzadzenie sie zrestartuje.";
constexpr char TXT_GEN_NOWY_KOD[] = "GEN. NOWY KOD";
constexpr char TXT_OKO[] = "OKO";
constexpr char TXT_SPACJA[] = "SPACJA";
constexpr char TXT_SHIFT[] = "SHIFT";
constexpr char TXT_123[] = "123";
constexpr char TXT_USUN[] = "USUN";
constexpr char TXT_OK[] = "OK";

// Lista sieci Wi-Fi.
constexpr char TXT_WYBIERZ_SIEC_WIFI[] = "Wybierz siec WiFi";
constexpr char TXT_KOD_HK_FORMAT[] = "Kod HK: %s";
constexpr char TXT_SKANOWANIE[] = "Skanowanie";
constexpr char TXT_KOD_HK[] = "KOD HK";

// Ekran glowny.
constexpr char TXT_TCP_SYGNAL_FORMAT[] = "TCP:OK %ddBm";
constexpr char TXT_TCP_BRAK_POLACZENIA[] = "TCP: --";
constexpr char TXT_WIFI_SYGNAL_FORMAT[] = "WiFi:OK %ddBm";
constexpr char TXT_WIFI_BRAK_POLACZENIA[] = "WiFi: --";
constexpr char TXT_AKTUALNA[] = "AKTUALNA";
constexpr char TXT_TEMPERATURA[] = "TEMPERATURA";
constexpr char TXT_ZADANA[] = "ZADANA";
constexpr char TXT_HISTEREZA[] = "Histereza";
constexpr char TXT_WILGOTNOSC[] = "Wilgotnosc";
constexpr char TXT_CISNIENIE[] = "Cisnienie";
constexpr char TXT_BRAK_TEMPERATURY[] = "--.-";
constexpr char TXT_TEMPERATURA_FORMAT[] = "%.1f";
constexpr char TXT_STOPNIE_C[] = "°C";
constexpr char TXT_HISTEREZA_FORMAT[] = "%.1f°C";
constexpr char TXT_BRAK_WILGOTNOSCI[] = "--.-%";
constexpr char TXT_WILGOTNOSC_FORMAT[] = "%.1f%%";
constexpr char TXT_BRAK_CISNIENIA[] = "---";
constexpr char TXT_CISNIENIE_FORMAT[] = "%.0f hPa";
constexpr char TXT_ZADANA_TEMPERATURA[] = "ZADANA TEMPERATURA";

// Komunikaty konfiguracji.
constexpr char TXT_SZUKANIE_SLAVE_BLE[] = "Szukanie Slave BLE...";
constexpr char TXT_NIE_MOZNA_URUCHOMIC_BLE[] = "Nie mozna uruchomic BLE";
constexpr char TXT_BRAK_SLAVE_BLE[] = "BRAK SLAVE BLE";
constexpr char TXT_BLAD_AUTORYZACJI_SLAVE[] = "Blad autoryzacji Slave";
constexpr char TXT_SLAVE_BLE_POTWIERDZONY[] = "Slave BLE potwierdzony";
constexpr char TXT_WYSYLANIE_DANYCH_DO_SLAVE[] = "Wysylanie danych do Slave";
constexpr char TXT_SLAVE_NIE_POLACZYL_SIE_Z_WIFI[] = "Slave nie polaczyl sie z WiFi";
constexpr char TXT_GOTOWE_RESTART_MASTER[] = "Gotowe. Restart Master";
constexpr char TXT_UTRACONO_POLACZENIE_BLE[] = "Utracono polaczenie BLE";
constexpr char TXT_KONFIGURUJE_SLAVE_WIFI[] = "Konfiguruje Slave WiFi...";
constexpr char TXT_PONOWNE_SZUKANIE_SLAVE[] = "Ponowne szukanie Slave";
constexpr char TXT_URUCHAMIANIE_BLE_SLAVE[] = "Uruchamianie BLE Slave...";

} // namespace ScreenText

// ============================================================
// TYPY DANYCH
// ============================================================
enum class Trend : int8_t { Unknown, Rising, Falling, Stable };

struct TrendWindow {
    static constexpr uint8_t sampleCount = 6;  // 6 odczytow = 5 zmian.
    float samples[sampleCount] = {};
    uint8_t next = 0;
    uint8_t count = 0;

    Trend update(float value, float tolerance) {
        if (!isfinite(value)) {
            next = 0;
            count = 0;
            return Trend::Unknown;
        }

        samples[next] = value;
        next = (next + 1) % sampleCount;
        if (count < sampleCount) ++count;
        if (count < sampleCount) return Trend::Unknown;

        // Suma pieciu kolejnych zmian: najnowszy odczyt minus najstarszy.
        const float change = value - samples[next];
        if (change >= tolerance) return Trend::Rising;
        if (change <= -tolerance) return Trend::Falling;
        return Trend::Stable;
    }
};

enum ProvisionStage : uint8_t {
    PROV_OFF,
    PROV_SEARCH,
    PROV_SELECT,
    PROV_SENDING,
    PROV_ERROR
};

// ============================================================
// ZMIENNE GLOBALNE I OBIEKTY
// ============================================================
// Sprzet i pamiec ustawien.
TFT_eSPI tft = TFT_eSPI();
Adafruit_BME280 bme;
Preferences preferences;

// I2C(0) = pojemnosciowy dotyk CST816D (GPIO33/32).
// I2C(1) = BME280 (GPIO21/22) - nie zakloca dotyku.
TwoWire bmeWire(1);

// Wyswietlacz i synchronizacja zadan.
static lv_obj_t* lvRoot = nullptr;
static lv_disp_draw_buf_t lvDrawBuf;
static lv_disp_drv_t lvDispDrv;
static lv_color_t lvBuffer[320 * 10];
static uint32_t lvLastTick = 0;
static volatile uint32_t lvFlushCounter = 0;
static bool hkCodeJustGenerated = false;
static bool lvReady = false;

SemaphoreHandle_t tftMux = nullptr;
SemaphoreHandle_t dataMux = nullptr;
SemaphoreHandle_t i2cMux = nullptr;
SemaphoreHandle_t wifiMux = nullptr;

// Pomiary i sterowanie ogrzewaniem.
bool bmeOk = false;

volatile float targetTemp = 21.0;
volatile float hysteresis = 1.0;
volatile float lastTemp = NAN;
volatile float lastHum = NAN;
volatile float lastPress = NAN;

static Trend tempTrend = Trend::Unknown;
static Trend humidityTrend = Trend::Unknown;
static uint16_t humidityColor = TFT_DARKGREY;

static Trend pressureTrend = Trend::Unknown;
volatile int lastSentState = -1;
volatile int demandState = 0;

volatile bool pendingSave = false;
volatile unsigned long lastChangeTime = 0;
const unsigned long saveDelay = 3000;

// Zapisana siec i HomeKit.
char savedSSID[SSID_MAX_LEN] = "";
char savedPass[PASS_MAX_LEN] = "";
char hkCode[9] = {0};

volatile bool wifiReady = false;
bool homeSpanStarted = false;

// Stan ekranow.
volatile bool isWifiMenu = false;
volatile bool isKeyboardMode = false;
volatile bool isMacMenu = false;
volatile bool isHkCodeMenu = false;
volatile bool isSettingsMenu = false;
volatile bool isFailureMenu = false;

// Konfiguracja awaryjna zapisywana w NVS Mastera i synchronizowana z Slave.
// 0=OFF, 1=ON, 2=OSTATNI STAN; relayLow: true=aktywacja LOW.
uint8_t failureMode = 2;
uint8_t failureTimeoutMin = 10;
bool relayLow = false; // domyslna polaryzacja zgodna ze starszym Slave: HIGH=ON
uint8_t editFailureMode = 2;
uint8_t editFailureTimeoutMin = 10;
bool editRelayLow = false;
volatile bool isProvisionMenu = false;
volatile bool editHysteresis = false;

// Skanowanie i klawiatura.
volatile bool requestScanWifi = false;
volatile bool isScanningWifi = false;
volatile uint8_t wifiScanAnimDots = 0;
unsigned long lastWifiScanAnim = 0;

char networkSSIDs[MAX_NETWORKS][SSID_MAX_LEN];
int networkRSSIs[MAX_NETWORKS];
int networkCount = 0;
int selectedNetworkIndex = -1;

char inputPassword[PASS_MAX_LEN] = "";
int keyboardMode = 0;
bool showPassword = false;

unsigned long lastTouchTime = 0;
const unsigned long touchDebounce = 200;

int lastRSSI = 0;
IPAddress lastIP;
int lastThermoState = -2;

// Konfiguracja sieci przez BLE.
volatile ProvisionStage provStage = PROV_OFF;
volatile bool provisionRequested = false;
volatile bool provisionSendRequested = false;
volatile bool cancelProvisionRequested = false;
volatile bool networkUiDirty = false;

volatile bool tcpConnected = false;
volatile int slaveRSSI = -127;  // RSSI Wi-Fi Slave'a (dBm)
volatile bool tcpStateChanged = false;
volatile bool requestResetWiFi = false;

char provisionMessage[96] = "";
char pendingSSID[SSID_MAX_LEN] = "";
char pendingPass[PASS_MAX_LEN] = "";

NimBLEScan* bleScan = nullptr;
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* bleCred = nullptr;
NimBLERemoteCharacteristic* bleStatus = nullptr;
NimBLEAdvertisedDevice* discoveredSlave = nullptr;

uint8_t bleChallenge[16] = {};
volatile bool bleFound = false;
bool bleRunning = false;

// Sesja TCP ze Slave.
WiFiClient slaveTCP;
IPAddress slaveIP;
char slaveIPText[20] = "--";
uint8_t tcpChallenge[16] = {};

bool tcpSession = false;
volatile bool configDirty = true;
bool configSentThisSession = false;
// Komunikat sukcesu wyłącznie po ręcznym ZAPISZ i potwierdzeniu Slave.
volatile bool userConfigPending = false;
volatile uint32_t configAckAt = 0;
bool mdnsStarted = false;

uint32_t tcpSeq = 0;
uint32_t lastTCPAck = 0;
uint32_t lastTCPTry = 0;
uint32_t lastTCPSend = 0;
uint32_t resetRequestAt = 0;

constexpr uint32_t TCP_SEND_INTERVAL_MS = 2000;
constexpr uint32_t TCP_VALID_MS = 5000;

#if DIAG
// Stosy zadan sa mierzone w bajtach w ESP-IDF (Arduino-ESP32 3.x).
// Uchwyty sa ustawiane przez xTaskCreatePinnedToCore().
static TaskHandle_t diagTaskUI = nullptr;
static TaskHandle_t diagTaskThermostat = nullptr;
static TaskHandle_t diagTaskNetwork = nullptr;
static constexpr uint32_t RAM_DIAG_INTERVAL_MS = 10000;
static uint32_t lastRamDiag = 0;
static size_t diagPreviousFree = 0;
#endif

#if DIAG
static uint32_t touchLastErrorLog = 0;
static uint32_t touchLastSampleLog = 0;
static bool touchWasPressed = false;
#endif

// Obsluga przycisku resetu.
uint32_t factoryPressedAt = 0;
bool factoryHandled = false;

// ============================================================
// PROTOTYPY
// ============================================================
static lv_color_t rgb565(uint16_t value);
static void lvFlush(lv_disp_drv_t* disp, const lv_area_t* a, lv_color_t* colors);
static void lvDrawScreen(lv_event_t* e);

static void lvInvalidate();
void drawUI();
void drawTopBar();
void drawSliderSection();
void updateData();
void checkThermostat(float currentTemp);
void saveSettings();

void drawGearIcon(int x, int y, int r, uint16_t color);
void drawBluetoothIcon(int x, int y, int r, uint16_t color);
void drawFlameIcon(int x, int y, int r, uint16_t color);
void drawThermometerIcon(int x, int y, int state,
                         uint16_t colorOff);
void drawCenteredText(const char* text, int x, int y,
                      int w, int h, uint8_t size,
                      uint16_t color, uint16_t bgColor);

void drawWifiMenu(bool startScan = true);
void drawKeyboard();
void drawMacMenu();
void drawSettingsMenu();
void drawFailureMenu();
void saveFailureSettings();
void drawProvisionScreen();
void drawHkCodeMenu();
void redrawCurrentScreen();
void handleTouch(int x, int y);
void connectToWiFi(const char* ssid, const char* pass);
void drawWifiScanningStatus();
int getSignalBars(int rssi);

void _drawTopBarUnlocked();
void _drawSliderSectionUnlocked();
void _updateDataUnlocked();

void taskUI(void*);
void taskThermostat(void*);
void taskNetwork(void*);

void bleStop();
void slaveTCPStop();
void startProvisioning();
bool connectProvisionSlave();
bool sendProvisionCredentials();
void tcpMasterStep();
void factoryReset();

// ============================================================
// ODCZYT DOTYKU
// ============================================================
// Dotyk bez dodatkowej biblioteki i bez alokacji pamieci.
static bool readCST816D(int& x, int& y) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);
    const uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
#if DIAG
        if (millis() - touchLastErrorLog >= 2000) {
            Serial.printf("[TOUCH] I2C ERROR=%u addr=0x%02X\n", err, TOUCH_ADDR);
            touchLastErrorLog = millis();
        }
#endif
        return false;
    }

    const uint8_t received = Wire.requestFrom(uint8_t(TOUCH_ADDR), uint8_t(5));
    if (received != 5) {
#if DIAG
        if (millis() - touchLastErrorLog >= 2000) {
            Serial.printf("[TOUCH] READ ERROR bytes=%u/5\n", received);
            touchLastErrorLog = millis();
        }
#endif
        while (Wire.available()) Wire.read();
        return false;
    }

    const uint8_t count = Wire.read();
    const uint8_t xh = Wire.read();
    const uint8_t xl = Wire.read();
    const uint8_t yh = Wire.read();
    const uint8_t yl = Wire.read();

    if (count == 0 || count > 2) {
#if DIAG
        if (touchWasPressed) Serial.println("[TOUCH] RELEASE");
        touchWasPressed = false;
#endif
        return false;
    }

    const int rawX = ((xh & 0x0F) << 8) | xl;
    const int rawY = ((yh & 0x0F) << 8) | yl;
    if (rawX < 0 || rawX >= 240 || rawY < 0 || rawY >= 320) {
#if DIAG
        if (millis() - touchLastErrorLog >= 2000) {
            Serial.printf("[TOUCH] INVALID RAW X=%d Y=%d count=%u\n", rawX, rawY, count);
            touchLastErrorLog = millis();
        }
#endif
        return false;
    }

#if TOUCH_ROTATION == 0
    x = rawX;
    y = rawY;
#elif TOUCH_ROTATION == 1
    x = rawY;
    y = 239 - rawX;
#elif TOUCH_ROTATION == 2
    x = 239 - rawX;
    y = 319 - rawY;
#else
    x = 319 - rawY;
    y = rawX;
#endif

#if DIAG
    if (!touchWasPressed || millis() - touchLastSampleLog >= 150) {
        Serial.printf("[TOUCH] count=%u RAW=(%d,%d) SCREEN=(%d,%d) IRQ=%d\n",
                      count, rawX, rawY, x, y, digitalRead(TOUCH_IRQ));
        touchLastSampleLog = millis();
    }
    touchWasPressed = true;
#endif
    return x >= 0 && x < 320 && y >= 0 && y < 240;
}

// ============================================================
// OCENA WILGOTNOSCI
// ============================================================
// Przedzialy od suchego do wilgotnego. Margines +/-0,5 pkt proc.
// daje histereze 1 pkt proc. na kazdej granicy.
static uint16_t updateHumidityColor(float humidity, int& band) {
    constexpr float thresholds[] = {30.0f, 40.0f, 50.0f, 60.0f};
    constexpr uint16_t colors[] = {TFT_RED, UI_ORANGE, TFT_GREEN, UI_ORANGE, TFT_RED};
    if (!isfinite(humidity) || humidity < 0.0f || humidity > 100.0f) {
        band = -1;
        return TFT_DARKGREY;
    }

    if (band < 0) {
        band = 0;
        while (band < 4 && (band == 2 ? humidity > thresholds[band]
                                     : humidity >= thresholds[band])) {
            ++band;
        }
    } else {
        while (band < 4 && humidity >= thresholds[band] + 0.5f) ++band;
        while (band > 0 && humidity <= thresholds[band - 1] - 0.5f) --band;
    }
    return colors[band];
}
// ============================================================
// DIAGNOSTYKA PAMIECI
// ============================================================
#if DIAG
static void reportTaskStack(const char* name, TaskHandle_t task, size_t configured) {
    if (!task) {
        Serial.printf("[STACK] %-10s task=NOT_CREATED\n", name);
        return;
    }
    const UBaseType_t minimumFreeBytes = uxTaskGetStackHighWaterMark(task);
    const size_t estimatedMaxUsed =
        configured > size_t(minimumFreeBytes)
            ? configured - size_t(minimumFreeBytes) : 0;
    Serial.printf(
        "[STACK] %-10s alloc=%u minFree=%u maxUsed~=%u headroom=%u\n",
        name,
        unsigned(configured), unsigned(minimumFreeBytes),
        unsigned(estimatedMaxUsed), unsigned(minimumFreeBytes));
}

void reportRamDiagnostics() {
    const uint32_t now = millis();
    if (now - lastRamDiag < RAM_DIAG_INTERVAL_MS) return;
    lastRamDiag = now;

    const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t minHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psramTotal = ESP.getPsramSize();
    const size_t psramFree = psramTotal ? ESP.getFreePsram() : 0;
    const long delta = diagPreviousFree ? long(freeHeap) - long(diagPreviousFree) : 0L;
    diagPreviousFree = freeHeap;
    const unsigned fragmentation = freeHeap
        ? unsigned(100U - (largest * 100U / freeHeap)) : 0U;

    Serial.printf(
        "[RAM] uptime=%lu s free=%u min=%u largest=%u "
        "DMAfree=%u DMAlargest=%u PSRAM=%u/%u "
        "WiFi=%d TCP=%d BLE=%d\n",
        (unsigned long)(now / 1000), unsigned(freeHeap), unsigned(minHeap),
        unsigned(largest), unsigned(dmaFree), unsigned(dmaLargest),
        unsigned(psramFree), unsigned(psramTotal),
        WiFi.status() == WL_CONNECTED ? 1 : 0,
        tcpConnected ? 1 : 0, bleRunning ? 1 : 0);
    Serial.printf(
        "[HEAP] internalFree=%u internalLargest=%u delta10s=%ld "
        "fragmentationApprox=%u%%\n",
        unsigned(internalFree), unsigned(internalLargest), delta, fragmentation);

    reportTaskStack("UI", diagTaskUI, 6144);
    Serial.printf("[LVGL] flush=%lu heap=%u\n",(unsigned long)lvFlushCounter,(unsigned)ESP.getFreeHeap());
    reportTaskStack("Thermostat", diagTaskThermostat, 3072);
    reportTaskStack("Network", diagTaskNetwork, 8192);
    // Uchwyt nullptr oznacza aktualne zadanie (Arduino loopTask).
    Serial.printf("[STACK] %-10s minFree=%u\n",
                  "loopTask", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    Serial.printf("[STATE] WiFiScan=%d BLEScan=%d Provision=%d "
                  "TCPsession=%d RSSI=%ld\n",
                  isScanningWifi ? 1 : 0,
                  bleScan != nullptr ? 1 : 0,
                  int(provStage), tcpSession ? 1 : 0,
                  WiFi.status() == WL_CONNECTED ? long(WiFi.RSSI()) : 0L);
}
#endif

void onWifiConnectionChanged(int status) {
    wifiReady = (WiFi.status() == WL_CONNECTED);
}

void generateAndSaveHkCode() {
    long code = random(10000000, 99999999);
    snprintf(hkCode, sizeof(hkCode), "%08ld", code);

    Preferences p;
    p.begin("settings", false);
    p.putString("hk_code", hkCode);
    p.end();
}

// ============================================================
// HOMESPAN - ORYGINALNY TERMOSTAT
// ============================================================
struct DEV_Thermostat : Service::Thermostat {
    Characteristic::CurrentTemperature* currentTemp;
    Characteristic::TargetTemperature* targetTempChar;
    Characteristic::CurrentRelativeHumidity* currentHum;
    Characteristic::TargetHeatingCoolingState* targetState;
    Characteristic::CurrentHeatingCoolingState* currentState;
    Characteristic::TemperatureDisplayUnits* tempUnits;

    // Ostatnia nastawa przekazana do HomeKit lub odebrana z HomeKit.
    float lastSyncedTarget = 21.0f;

    DEV_Thermostat() : Service::Thermostat() {
        currentTemp = new Characteristic::CurrentTemperature(21.0);
        currentTemp->setRange(-50, 100);
        targetTempChar = new Characteristic::TargetTemperature(21.0);
        targetTempChar->setRange(15, 35);
        currentHum = new Characteristic::CurrentRelativeHumidity(50.0);
        targetState = new Characteristic::TargetHeatingCoolingState(1);
        targetState->setValidValues(1);
        currentState = new Characteristic::CurrentHeatingCoolingState(0);
        tempUnits = new Characteristic::TemperatureDisplayUnits(0);
    }

    bool update() override {
        if (targetTempChar->updated()) {
            float value = targetTempChar->getNewVal<float>();
            if (value < 15.0f || value > 35.0f) return false;

            if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
                targetTemp = value;
                pendingSave = true;
                lastChangeTime = millis();
                lastSyncedTarget = value;
                xSemaphoreGive(dataMux);
            }
        }
        return true;
    }

    void loop() {
        float temp = NAN;
        float hum = NAN;
        float target = 21.0f;
        int sentState = 0;

        if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
            temp = lastTemp;
            hum = lastHum;
            target = targetTemp;
            sentState = lastSentState;
            xSemaphoreGive(dataMux);
        }

        if (fabsf(target - lastSyncedTarget) > 0.049f) {
            targetTempChar->setVal(target);
            lastSyncedTarget = target;
        }

        if (!isnan(temp)) currentTemp->setVal(temp);
        if (!isnan(hum)) currentHum->setVal(hum);
        currentState->setVal(sentState == 1 ? 1 : 0);
    }
};

// ============================================================
// BLE - WYKRYWANIE SLAVE
// ============================================================
class SlaveScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (bleFound) return;

        BLE_DIAG("Advertisement received RSSI=%d dBm\n", dev->getRSSI());
        if (!dev->isAdvertisingService(
                NimBLEUUID(SecureLink::SERVICE_UUID))) {
            return;
        }

        BLE_DIAG("Slave found: service UUID matches; requesting scan stop in network task\n");
        discoveredSlave = new NimBLEAdvertisedDevice(*dev);
        bleFound = true;
        // Scan zatrzymuje taskNetwork, nie callback.
    }
};

static SlaveScanCallbacks slaveScanCallbacks;

#if DIAG
// Obiekt statyczny: klient nie moze go usuwac przy deleteClient().
class ProvisionClientDiagCallbacks : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* client) override {
        BLE_DIAG("onConnect uptime=%lu ms handle=%u MTU=%u\n",
                 (unsigned long)millis(),
                 unsigned(client->getConnHandle()), unsigned(client->getMTU()));
    }

    void onConnectFail(NimBLEClient* client, int reason) override {
        (void)client;
        BLE_DIAG("onConnectFail uptime=%lu ms reason=%d (0x%X)\n",
                 (unsigned long)millis(), reason,
                 static_cast<unsigned int>(reason));
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        (void)client;
        BLE_DIAG("onDisconnect uptime=%lu ms reason=%d (0x%X)\n",
                 (unsigned long)millis(), reason,
                 static_cast<unsigned int>(reason));
    }

    void onMTUChange(NimBLEClient* client, uint16_t mtu) override {
        BLE_DIAG("onMTUChange uptime=%lu ms handle=%u MTU=%u\n",
                 (unsigned long)millis(),
                 unsigned(client->getConnHandle()), unsigned(mtu));
    }
};

static ProvisionClientDiagCallbacks provisionClientDiagCallbacks;
#endif

void bleStop() {
    if (!bleRunning) return;

    BLE_DIAG("BLE cleanup begin\n");
    if (bleScan && bleScan->isScanning()) {
        BLE_DIAG("Scan stop requested\n");
        bleScan->stop();
        BLE_DIAG("Scan stop returned; scanning=%d\n", bleScan->isScanning() ? 1 : 0);
    }

    if (bleClient) {
        if (bleClient->isConnected()) {
            BLE_DIAG("Disconnect requested during cleanup\n");
            bleClient->disconnect();
        }

        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }

    BLE_DIAG("BLE deinit\n");
    NimBLEDevice::deinit(true);

    bleScan = nullptr;
    bleCred = nullptr;
    bleStatus = nullptr;
    bleRunning = false;
    bleFound = false;

    if (discoveredSlave) {
        delete discoveredSlave;
        discoveredSlave = nullptr;
    }
}

void provisionFail(const char* reason) {
    BLE_DIAG("Provisioning failed: %s\n", reason);
    strlcpy(provisionMessage, reason,
            sizeof(provisionMessage));
    provStage = PROV_ERROR;
    networkUiDirty = true;
    bleStop();
}

// Potwierdzenie, ze Slave ma ten sam klucz AES.
// Samo wykrycie reklamy BLE nie wystarcza.
bool connectProvisionSlave() {
    BLE_DIAG("Authorization begin: device=%d BLErunning=%d\n",
             discoveredSlave != nullptr, bleRunning ? 1 : 0);
    if (!discoveredSlave || !bleRunning) {
        BLE_DIAG("ERROR: missing discovered device or BLE not running\n");
        return false;
    }

    bleClient = NimBLEDevice::createClient();

    if (!bleClient) {
        BLE_DIAG("ERROR: createClient returned null\n");
        return false;
    }
    BLE_DIAG("BLE connect attempt\n");
#if DIAG
    bleClient->setClientCallbacks(&provisionClientDiagCallbacks, false);
    const auto bleClientConfig = bleClient->getConfig();
    BLE_DIAG("Client config: async=%u exchangeMTU=%u connectFailRetries=%u\n",
             unsigned(bleClientConfig.asyncConnect),
             unsigned(bleClientConfig.exchangeMTU),
             unsigned(bleClientConfig.connectFailRetries));
    const uint32_t bleConnectStartedAt = millis();
#endif
    if (!bleClient->connect(discoveredSlave)) {
#if DIAG
        // Pobierz blad bezposrednio po connect(), przed sprzataniem BLE.
        // Kod dziesietny i szesnastkowy pozwala rozpoznac warstwe bledu.
        const int bleConnectError = bleClient->getLastError();
        BLE_DIAG("ERROR: BLE connect returned false; error=%d (0x%X); elapsed=%lu ms\n",
                 bleConnectError, static_cast<unsigned int>(bleConnectError),
                 static_cast<unsigned long>(millis() - bleConnectStartedAt));
#endif
        return false;
    }
    BLE_DIAG("BLE connect OK; discovering service\n");

    NimBLERemoteService* service =
        bleClient->getService(SecureLink::SERVICE_UUID);

    if (!service) {
        BLE_DIAG("ERROR: service not found; connected=%d\n", bleClient->isConnected() ? 1 : 0);
        return false;
    }
    BLE_DIAG("Service found; discovering characteristics\n");

    NimBLERemoteCharacteristic* challengeChr =
        service->getCharacteristic(
            SecureLink::CHALLENGE_UUID);

    bleCred = service->getCharacteristic(
        SecureLink::CREDENTIALS_UUID);

    bleStatus = service->getCharacteristic(
        SecureLink::STATUS_UUID);

    BLE_DIAG("Characteristics: challenge=%d credentials=%d status=%d\n",
             challengeChr != nullptr, bleCred != nullptr, bleStatus != nullptr);
    if (!challengeChr || !bleCred || !bleStatus) {
        BLE_DIAG("ERROR: required characteristic missing\n");
        return false;
    }

    BLE_DIAG("Challenge read attempt\n");
    auto value = challengeChr->readValue();
    BLE_DIAG("Challenge read returned bytes=%u; connected=%d\n",
             unsigned(value.size()), bleClient->isConnected() ? 1 : 0);

    if (value.size() != 16) {
        BLE_DIAG("ERROR: challenge length invalid (expected 16); empty read may indicate read failure\n");
        return false;
    }
    BLE_DIAG("Challenge length OK\n");

    memcpy(bleChallenge, value.data(), 16);

    uint8_t proof[17];
    memcpy(proof, bleChallenge, 16);
    proof[16] = 0xA5;

    uint8_t packet[SecureLink::MAX_PACKET];
    size_t packetLen = 0;

    BLE_DIAG("H encryption attempt\n");
    if (!SecureLink::seal(
            'H', proof, sizeof(proof),
            packet, packetLen)) {
        BLE_DIAG("ERROR: H encryption failed\n");
        return false;
    }
    BLE_DIAG("H encryption OK; writing bytes=%u with response\n", unsigned(packetLen));

    if (!bleCred->writeValue(
            packet, packetLen, true)) {
        BLE_DIAG("ERROR: H write returned false; connected=%d\n", bleClient->isConnected() ? 1 : 0);
        return false;
    }

    BLE_DIAG("H write OK; waiting for J (12 attempts)\n");
    for (int i = 0; i < 12; ++i) {
        BLE_DIAG("J read attempt=%d/12; connected=%d\n", i + 1, bleClient->isConnected() ? 1 : 0);
        auto response = bleStatus->readValue();
        BLE_DIAG("J read returned bytes=%u\n", unsigned(response.size()));

        uint8_t plain[SecureLink::MAX_PLAIN];
        size_t plainLen = 0;

        const bool opened = SecureLink::open(
                'J',
                reinterpret_cast<const uint8_t*>(
                    response.data()),
                response.size(),
                plain, plainLen);
        if (opened &&
            plainLen == 17 &&
            plain[16] == 0x5A &&
            memcmp(plain, bleChallenge, 16) == 0) {
            BLE_DIAG("J verified: decryption, length, marker and challenge OK; authorization OK\n");
            return true;
        }
#if DIAG
        if (!opened) {
            BLE_DIAG("J rejected: decrypt/authentication failed (empty, invalid, wrong type or key mismatch possible)\n");
        } else if (plainLen != 17) {
            BLE_DIAG("J rejected: plaintext length=%u expected=17\n", unsigned(plainLen));
        } else if (plain[16] != 0x5A) {
            BLE_DIAG("J rejected: invalid confirmation marker\n");
        } else {
            BLE_DIAG("J rejected: challenge mismatch\n");
        }
#endif

        delay(100);
    }

    BLE_DIAG("ERROR: J verification exhausted all 12 attempts; connected=%d\n", bleClient->isConnected() ? 1 : 0);
    return false;
}

void startProvisioning() {
    BLE_DIAG("Provisioning requested\n");
    bleStop();

    provStage = PROV_SEARCH;
    bleFound = false;

    strlcpy(provisionMessage,
            ScreenText::TXT_SZUKANIE_SLAVE_BLE,
            sizeof(provisionMessage));

    networkUiDirty = true;

    NimBLEDevice::init("Termostat-Master-Setup");
    NimBLEDevice::setMTU(247);

    bleRunning = true;
    bleScan = NimBLEDevice::getScan();

    bleScan->setScanCallbacks(&slaveScanCallbacks);
    bleScan->setActiveScan(true);
    bleScan->setMaxResults(0);

    BLE_DIAG("Active scan start attempt; duration=11000 ms\n");
    if (!bleScan->start(11000, false, true)) {
        BLE_DIAG("ERROR: scan start returned false\n");
        provisionFail(ScreenText::TXT_NIE_MOZNA_URUCHOMIC_BLE);
        return;
    }

    BLE_DIAG("Scan start OK\n");
    uint32_t start = millis();

    while (!bleFound &&
           millis() - start < 12000 &&
           provStage == PROV_SEARCH &&
           !cancelProvisionRequested) {
        delay(50);
    }

    if (bleScan && bleScan->isScanning()) {
        BLE_DIAG("Scan stop requested\n");
        bleScan->stop();
        BLE_DIAG("Scan stop returned; scanning=%d\n", bleScan->isScanning() ? 1 : 0);
    }

    if (cancelProvisionRequested ||
        provStage != PROV_SEARCH) {
        BLE_DIAG("Provisioning interrupted: cancelled=%d stage=%d\n", cancelProvisionRequested ? 1 : 0, int(provStage));
        bleStop();
        return;
    }

    BLE_DIAG("Search ended: found=%d cancelled=%d elapsed=%lu ms\n",
             bleFound ? 1 : 0, cancelProvisionRequested ? 1 : 0,
             (unsigned long)(millis() - start));
    if (!bleFound) {
        BLE_DIAG("ERROR: no matching Slave advertisement within search window\n");
        provisionFail(ScreenText::TXT_BRAK_SLAVE_BLE);
        return;
    }

    if (!connectProvisionSlave()) {
        provisionFail(ScreenText::TXT_BLAD_AUTORYZACJI_SLAVE);
        return;
    }

    if (cancelProvisionRequested ||
        provStage != PROV_SEARCH) {
        BLE_DIAG("Provisioning interrupted: cancelled=%d stage=%d\n", cancelProvisionRequested ? 1 : 0, int(provStage));
        bleStop();
        return;
    }

    if (xSemaphoreTake(wifiMux, portMAX_DELAY)) {
        networkCount = 0;
        xSemaphoreGive(wifiMux);
    }

    provStage = PROV_SELECT;
    strlcpy(provisionMessage,
            ScreenText::TXT_SLAVE_BLE_POTWIERDZONY,
            sizeof(provisionMessage));

    networkUiDirty = true;
}

// ============================================================
// BLE - ZASZYFROWANE DANE WIFI
// ============================================================
bool sendProvisionCredentials() {
    if (!bleClient ||
        !bleClient->isConnected() ||
        !bleCred || !bleStatus) {
        return false;
    }

    size_t ssidLen = strlen(pendingSSID);
    size_t passLen = strlen(pendingPass);

    if (!ssidLen || ssidLen > 32 ||
        passLen > 64) {
        return false;
    }

    uint8_t plain[SecureLink::MAX_PLAIN];

    memcpy(plain, bleChallenge, 16);
    plain[16] = uint8_t(ssidLen);
    memcpy(plain + 17, pendingSSID, ssidLen);
    plain[17 + ssidLen] = uint8_t(passLen);
    memcpy(plain + 18 + ssidLen,
           pendingPass, passLen);

    uint8_t packet[SecureLink::MAX_PACKET];
    size_t packetLen = 0;

    if (!SecureLink::seal(
            'P',
            plain,
            18 + ssidLen + passLen,
            packet,
            packetLen)) {
        return false;
    }

    if (!bleCred->writeValue(
            packet, packetLen, true)) {
        return false;
    }

    uint32_t start = millis();

    while (millis() - start < 27000) {
        if (!bleClient->isConnected()) return false;

        auto response = bleStatus->readValue();

        uint8_t result[SecureLink::MAX_PLAIN];
        size_t resultLen = 0;

        if (SecureLink::open(
                'K',
                reinterpret_cast<const uint8_t*>(
                    response.data()),
                response.size(),
                result,
                resultLen) &&
            resultLen == 21 &&
            memcmp(result, bleChallenge, 16) == 0) {

            if (result[16] != 1) return false;

            slaveIP = IPAddress(
                result[17], result[18],
                result[19], result[20]);

            snprintf(slaveIPText,
                     sizeof(slaveIPText),
                     "%s",
                     slaveIP.toString().c_str());

            return true;
        }

        delay(250);
    }

    return false;
}

// ============================================================
// TCP - BEZPIECZNA TRANSMISJA 0/1
// ============================================================
void slaveTCPStop() {
    slaveTCP.stop();
    tcpSession = false;
    configSentThisSession = false;

    if (tcpConnected) {
        tcpConnected = false;
        tcpStateChanged = true;
    }
}

void tcpMasterStep() {
    if (!wifiReady ||
        (provStage != PROV_OFF &&
         !requestResetWiFi)) {
        slaveTCPStop();
        return;
    }

    if (!slaveTCP.connected() || !tcpSession) {
        slaveTCPStop();

        if (millis() - lastTCPTry < 3000) {
            return;
        }

        lastTCPTry = millis();

        IPAddress address = slaveIP;

        if (address == IPAddress(0, 0, 0, 0) ||
            !slaveTCP.connect(
                address, SecureLink::PORT, 900)) {

            address = MDNS.queryHost(
                "termostat-slave", 1500);

            if (address == IPAddress(0, 0, 0, 0) ||
                !slaveTCP.connect(
                    address, SecureLink::PORT, 900)) {
                return;
            }
        }

        uint8_t greeting[20];

        if (!SecureLink::readExact(
                slaveTCP, greeting, 20, 1500) ||
            memcmp(greeting, "ET2!", 4) != 0) {
            slaveTCPStop();
            return;
        }

        memcpy(tcpChallenge, greeting + 4, 16);

        tcpSession = true;
        configSentThisSession = false;
        tcpSeq = 0;
        lastTCPSend = 0;

        slaveIP = address;

        snprintf(slaveIPText,
                 sizeof(slaveIPText), "%s",
                 address.toString().c_str());

        Preferences p;
        p.begin("settings", false);
        p.putString("slaveip", slaveIPText);
        p.end();

        tcpStateChanged = true;
    }

    if (millis() - lastTCPSend <
        TCP_SEND_INTERVAL_MS) {
        return;
    }

    lastTCPSend = millis();

    // Konfiguracja ma osobna zaszyfrowana komende F; normalny stan 0/1
    // wysylamy dopiero po jej potwierdzeniu przez Slave.
    const bool sendConfig = !configSentThisSession || configDirty;
    uint8_t command[24] = {};
    memcpy(command, tcpChallenge, 16);

    uint32_t seq = ++tcpSeq;
    SecureLink::putU32(command + 16, seq);

    bool reconfigure = requestResetWiFi;
    command[20] = reconfigure ? 'R' : sendConfig ? 'F' : (demandState ? '1' : '0');
    if (sendConfig && !reconfigure) {
        command[21] = failureMode;
        command[22] = failureTimeoutMin;
        command[23] = relayLow ? 1 : 0;
    }
    const size_t commandLen = (sendConfig && !reconfigure) ? 24 : 21;

    if (!SecureLink::sendFrame(
            slaveTCP, 'C',
            command, commandLen)) {
        slaveTCPStop();
        return;
    }

    uint8_t ack[SecureLink::MAX_PLAIN];
    size_t ackLen = 0;

    if (!SecureLink::recvFrame(
            slaveTCP, 'A',
            ack, ackLen, 1500) ||
        ackLen != 26 ||
        memcmp(ack, tcpChallenge, 16) != 0 ||
        SecureLink::getU32(ack + 16) != seq ||
        ack[20] != command[20]) {
        slaveTCPStop();
        return;
    }

    if (sendConfig && !reconfigure) {
        configSentThisSession = true;
        // Po potwierdzeniu ACK: stan konfiguracji pozostaje dirty jesli
        // uzytkownik zmienil nastawy w trakcie transmisji.
        if (command[21] == failureMode && command[22] == failureTimeoutMin &&
            command[23] == (relayLow ? 1 : 0)) {
            configDirty = false;
            // Slave odsyla ACK F dopiero po udanym zapisie NVS.
            if (userConfigPending) {
                userConfigPending = false;
                configAckAt = millis();
                networkUiDirty = true;
            }
        }
    }

    if (reconfigure) {
        requestResetWiFi = false;
        slaveTCPStop();
        if (provStage == PROV_SEARCH && !cancelProvisionRequested) {
            provisionRequested = true;
        }
        return;
    }

    if (!tcpConnected) {
        tcpConnected = true;
        tcpStateChanged = true;
    }

    lastTCPAck = millis();

    // Ostatni bajt ACK zawiera RSSI zmierzone przez Slave.
    int receivedRSSI = static_cast<int8_t>(ack[25]);
    if (slaveRSSI != receivedRSSI) {
        slaveRSSI = receivedRSSI;
        tcpStateChanged = true;
    }

    if (!sendConfig && xSemaphoreTake(dataMux, portMAX_DELAY)) {
        lastSentState = demandState;
        xSemaphoreGive(dataMux);
    }

    IPAddress reportedIP(
        ack[21], ack[22],
        ack[23], ack[24]);

    if (slaveIP != reportedIP) {
        slaveIP = reportedIP;

        snprintf(slaveIPText,
                 sizeof(slaveIPText), "%s",
                 slaveIP.toString().c_str());

        tcpStateChanged = true;
    }
}

// ============================================================
// RESET FABRYCZNY
// ============================================================
void factoryReset() {
    slaveTCPStop();

    nvs_flash_erase();
    ESP.restart();
}

// ============================================================
// SETUP - ORYGINALNA INICJALIZACJA HARDWARE
// ============================================================
void setup() {
#if DIAG
    Serial.begin(115200);
    Serial.println("[RAM] Diagnostyka wlaczona; raport co 10 s");
#endif
    tftMux = xSemaphoreCreateMutex();
    dataMux = xSemaphoreCreateMutex();
    i2cMux = xSemaphoreCreateMutex();
    wifiMux = xSemaphoreCreateMutex();

    if (!tftMux || !dataMux || !i2cMux || !wifiMux) {
        while (1) delay(1000);
    }

    preferences.begin("settings", false);

    String storedCode =
        preferences.getString("hk_code", "");

    if (storedCode.length() == 8) {
        storedCode.toCharArray(hkCode, sizeof(hkCode));
    } else {
        long code = random(10000000, 99999999);
        snprintf(hkCode, sizeof(hkCode), "%08ld", code);
        preferences.putString("hk_code", hkCode);
    }

    preferences.getString("ssid", "")
        .toCharArray(savedSSID, SSID_MAX_LEN);

    preferences.getString("pass", "")
        .toCharArray(savedPass, PASS_MAX_LEN);

    float storedTarget =
        preferences.getFloat("targetTemp", 21.0);

    if (storedTarget < 15.0 || storedTarget > 35.0) {
        storedTarget = 21.0;
    }
    targetTemp = storedTarget;

    float storedHyst =
        preferences.getFloat("hysteresis", 1.0);

    if (storedHyst < 0.1 || storedHyst > 5.0) {
        storedHyst = 1.0;
    }
    hysteresis = storedHyst;

    failureMode = preferences.getUChar("failMode", 2);
    if (failureMode > 2) failureMode = 2;
    failureTimeoutMin = preferences.getUChar("failTime", 10);
    if (failureTimeoutMin < 1 || failureTimeoutMin > 30) failureTimeoutMin = 10;
    relayLow = preferences.getBool("relayLow", false);

    slaveIP.fromString(
        preferences.getString("slaveip", ""));

    if (slaveIP != IPAddress(0, 0, 0, 0)) {
        snprintf(slaveIPText,
                 sizeof(slaveIPText), "%s",
                 slaveIP.toString().c_str());
    }

    preferences.end();

    // HomeSpan zachowany z oryginalu.
    // Bez hasla WiFi nie uruchamiamy jego
    // osobnego kreatora sieci.
    if (savedSSID[0]) {
        homeSpan.setPairingCode(hkCode);
        homeSpan.setLogLevel(0);
        homeSpan.setWifiCredentials(
            savedSSID, savedPass);

        homeSpan.setConnectionCallback(
            onWifiConnectionChanged);

        homeSpan.setConnectionTimes(10, 30, 2);
        homeSpan.begin(
            Category::Thermostats,
            "Termostat pokojowy CO.");

        new SpanAccessory();
        new Service::AccessoryInformation();
        new Characteristic::Name(
            "Termostat pokojowy CO");
        new Characteristic::Manufacturer("@sdero Miroslaw Mastalerz");
        new Characteristic::Model("NiceMCU 2.8IPS Termostat");
        new Characteristic::FirmwareRevision("2.0");
        new Characteristic::Identify();
        new DEV_Thermostat();

        homeSpanStarted = true;
    }

    // Oryginalne sterowanie TFT i podswietleniem.
    // Jasnosc na stale 255; bez czujnika LDR i zadania Light.
    ledcAttach(BACKLIGHT_PIN, pwmFreq, pwmResolution);
    ledcWrite(BACKLIGHT_PIN, 0);

    tft.init();
    tft.setRotation(1);

    // Inwersje ST7789 ustawia TFT_eSPI na podstawie User_Setup.h.
    tft.fillScreen(UI_BG);

    ledcWrite(BACKLIGHT_PIN, BRIGHTNESS_LIGHT);

    // CST816D: TFT_eSPI wykonalo juz reset wspolny GPIO2.
    // GPIO36 jest pinem tylko wejsciowym - dla dotyku wystarczy polling I2C.
    pinMode(TOUCH_IRQ, INPUT);
    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
#if DIAG
    Serial.printf("[TOUCH] CST816D SDA=%d SCL=%d IRQ=%d RESET=%d\n",
                  TOUCH_SDA, TOUCH_SCL, TOUCH_IRQ, TOUCH_RESET);
    Wire.beginTransmission(TOUCH_ADDR);
    const uint8_t touchStatus = Wire.endTransmission();
    Serial.printf("[TOUCH] I2C 0x%02X: %s (status=%u)\n",
                  TOUCH_ADDR, touchStatus == 0 ? "DETECTED" : "NOT FOUND", touchStatus);
#endif

    // Zewnetrzny BME280: SDA GPIO21, SCL GPIO22.
    bmeWire.begin(BME_SDA, BME_SCL, 100000);
    bmeOk = bme.begin(0x76, &bmeWire);

    if (!savedSSID[0]) {
        WiFi.mode(WIFI_STA);
        wifiReady = false;
    }

    // LVGL 8.4 bez zmian; sprzet TFT_eSPI ST7789 + CST816D.
    lv_init();
    lv_disp_draw_buf_init(&lvDrawBuf, lvBuffer, nullptr, 320 * 10);
    lv_disp_drv_init(&lvDispDrv);
    lvDispDrv.hor_res=320;
    lvDispDrv.ver_res=240;
    lvDispDrv.draw_buf=&lvDrawBuf;
    lvDispDrv.flush_cb=lvFlush;
    lv_disp_drv_register(&lvDispDrv);
    lvRoot=lv_scr_act();
    lv_obj_set_style_bg_color(lvRoot, rgb565(UI_BG), 0);
    lv_obj_set_style_bg_opa(lvRoot, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(lvRoot, lvDrawScreen, LV_EVENT_DRAW_MAIN, nullptr);
    lvReady=true;
    lvLastTick=millis();
#if DIAG
    Serial.printf("[LVGL] READY depth=%d buffer=%u heap=%u\n",LV_COLOR_DEPTH, unsigned(sizeof(lvBuffer)), unsigned(ESP.getFreeHeap()));
#endif
    // Nie uruchamiamy BLE w pracy normalnej.
    drawUI();
    updateData();

#if DIAG
    // Zmniejszone stosy Thermostat (3072 B) i Network (8192 B); rdzenie bez zmian.
    BaseType_t uiCreated = xTaskCreatePinnedToCore(
        taskUI, "UI", 6144, NULL, 3, &diagTaskUI, 1);
    BaseType_t thCreated = xTaskCreatePinnedToCore(
        taskThermostat, "Thermostat", 3072, NULL, 2,
        &diagTaskThermostat, 1);
    BaseType_t netCreated = xTaskCreatePinnedToCore(
        taskNetwork, "Network", 8192, NULL, 2,
        &diagTaskNetwork, 0);
    Serial.printf("[TASK-CREATE] UI=%ld Thermostat=%ld Network=%ld "
                  "(1=PASS)\n",
                  long(uiCreated == pdPASS), long(thCreated == pdPASS),
                  long(netCreated == pdPASS));
#else
    xTaskCreatePinnedToCore(
        taskUI, "UI", 6144, NULL, 3, NULL, 1);

    xTaskCreatePinnedToCore(
        taskThermostat, "Thermostat",
        3072, NULL, 2, NULL, 1);

    xTaskCreatePinnedToCore(
        taskNetwork, "Network",
        8192, NULL, 2, NULL, 0);

#endif
}

void loop() {
    if (homeSpanStarted) {
        homeSpan.poll();
    }
#if DIAG
    reportRamDiagnostics();
#endif
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ============================================================
// TASK UI
// ============================================================
void taskUI(void*) {
    unsigned long lastUpdate = 0;
    unsigned long lastWifiCheck = 0;
    wl_status_t lastWifiStatus = WL_IDLE_STATUS;
    bool touchContactActive = false;
    bool touchRepeatAllowed = false;

    for (;;) {
        const uint32_t t=millis();
        lv_tick_inc(t-lvLastTick);
        lvLastTick=t;
        lv_timer_handler();
        if (isFailureMenu && configAckAt != 0 &&
            millis() - configAckAt >= 3000UL) {
            configAckAt = 0;
            lvInvalidate();
        }
        if (networkUiDirty && isFailureMenu) {
            networkUiDirty = false;
            lvInvalidate();
        }
        if (networkUiDirty) {
            networkUiDirty = false;

            if (provStage == PROV_SELECT &&
                isProvisionMenu) {
                isProvisionMenu = false;
                isSettingsMenu = false;
                drawWifiMenu(false);
                requestScanWifi = true;
            } else if (isProvisionMenu ||
                       provStage == PROV_ERROR ||
                       provStage == PROV_SENDING) {
                isProvisionMenu = true;
                isWifiMenu = false;
                isKeyboardMode = false;
                drawProvisionScreen();
            } else if (isWifiMenu && !isKeyboardMode) {
                drawWifiMenu(false);
            }
        }

        if (isScanningWifi &&
            isWifiMenu && !isKeyboardMode &&
            !isHkCodeMenu) {
            if (millis() - lastWifiScanAnim > 300) {
                lastWifiScanAnim = millis();
                wifiScanAnimDots =
                    (wifiScanAnimDots + 1) % 4;
                drawWifiScanningStatus();
            }
        } else {
            wifiScanAnimDots = 0;
        }

        bool homeScreen =
            !isWifiMenu && !isKeyboardMode &&
            !isMacMenu && !isHkCodeMenu &&
            !isSettingsMenu && !isProvisionMenu && !isFailureMenu;

        if (homeScreen &&
            millis() - lastUpdate > 1000) {
            lastUpdate = millis();
            updateData();
        }

        if (tcpStateChanged) {
            tcpStateChanged = false;

            if (homeScreen) drawTopBar();
            if (isMacMenu) drawMacMenu();
            if (isSettingsMenu) drawSettingsMenu();
        }

        if (WiFi.status() != lastWifiStatus) {
            lastWifiStatus = WiFi.status();
            if (homeScreen) drawTopBar();
        }

        if (homeScreen &&
            millis() - lastWifiCheck > 5000) {
            lastWifiCheck = millis();

            if (WiFi.status() == WL_CONNECTED) {
                int rssi = WiFi.RSSI();
                IPAddress ip = WiFi.localIP();

                if (rssi != lastRSSI || ip != lastIP) {
                    lastRSSI = rssi;
                    lastIP = ip;
                    drawTopBar();
                }
            }
        }

        if (pendingSave &&
            millis() - lastChangeTime > saveDelay) {
            unsigned long saveTime = lastChangeTime;
            saveSettings();
            if (saveTime == lastChangeTime) {
                pendingSave = false;
            }
        }

        // CST816D: natywne wspolrzedne, bez skalowania XPT2046.
        int touchX = 0, touchY = 0;
        if (readCST816D(touchX, touchY)) {
            const uint32_t touchNow = millis();
            const bool newPress = !touchContactActive;
            if (newPress) {
                touchContactActive = true;
                // Powtarzanie przy przytrzymaniu tylko dla nastaw na ekranie glownym.
                // Przyciski menu i klawiatury: jedna akcja na kontakt palca.
                const bool onHomeScreen =
                    !isWifiMenu && !isKeyboardMode && !isMacMenu &&
                    !isHkCodeMenu && !isSettingsMenu &&
                    !isProvisionMenu && !isFailureMenu;
                const bool onStepButton = touchY >= 195 && touchY <= 239 &&
                    ((touchX >= 0 && touchX < 60) ||
                     (touchX >= 260 && touchX < 320));
                const bool onSlider = touchX >= 60 && touchX < 260 &&
                    touchY >= 195 && touchY <= 239;
                touchRepeatAllowed = onHomeScreen && (onStepButton || onSlider);
            }
            if (newPress ||
                (touchRepeatAllowed && touchNow - lastTouchTime >= touchDebounce)) {
                lastTouchTime = touchNow;
#if DIAG
                Serial.printf("[TOUCH] ACTION x=%d y=%d newPress=%d repeat=%d\n",
                              touchX, touchY, newPress ? 1 : 0,
                              newPress ? 0 : 1);
#endif
                handleTouch(touchX, touchY);
            }
        } else {
            touchContactActive = false;
            touchRepeatAllowed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// TASK TERMOSTATU
// ============================================================
void taskThermostat(void*) {
    TrendWindow temperatureHistory;
    TrendWindow humidityHistory;
    int humidityBand = -1;
    TrendWindow pressureHistory;
    for (;;) {
        if (bmeOk) {
            float temp = NAN;
            float hum = NAN;
            float press = NAN;

            if (xSemaphoreTake(i2cMux, portMAX_DELAY)) {
                temp = bme.readTemperature() + BME_TEMP_OFFSET_C;
                hum = bme.readHumidity();
                press = bme.readPressure() / 100.0F;
                xSemaphoreGive(i2cMux);
            }

            if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
                lastTemp = temp;
                lastHum = hum;
                humidityColor = updateHumidityColor(hum, humidityBand);
                lastPress = press;
                tempTrend = temperatureHistory.update(temp, 0.1f);        // °C
                humidityTrend = humidityHistory.update(hum, 0.2f);        // pkt proc.
                pressureTrend = pressureHistory.update(press, 0.2f);      // hPa
                xSemaphoreGive(dataMux);
            }

            checkThermostat(temp);
        } else {
            demandState = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void checkThermostat(float currentTemp) {
    if (isnan(currentTemp)) {
        demandState = 0;
        return;
    }

    float target;
    float hyst;

    if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
        target = targetTemp;
        hyst = hysteresis;
        xSemaphoreGive(dataMux);
    } else {
        return;
    }

    int state = demandState;

    if (currentTemp <= target - hyst) {
        state = 1;
    } else if (currentTemp >= target + hyst) {
        state = 0;
    } else if (lastSentState == -1) {
        state = currentTemp < target ? 1 : 0;
    }

    demandState = state;
}

// ============================================================
// TASK SIECI - BLE TYLKO DO PROVISIONINGU
// ============================================================
void taskNetwork(void*) {
    if (FACTORY_BUTTON_PIN >= 0) {
        pinMode(FACTORY_BUTTON_PIN, INPUT_PULLUP);
    }

    for (;;) {
        if (FACTORY_BUTTON_PIN >= 0) {
            if (digitalRead(FACTORY_BUTTON_PIN) == LOW) {
                if (!factoryPressedAt) {
                    factoryPressedAt = millis();
                }

                if (!factoryHandled &&
                    millis() - factoryPressedAt > 5000) {
                    factoryHandled = true;
                    factoryReset();
                }
            } else {
                factoryPressedAt = 0;
                factoryHandled = false;
            }
        }

        bool connected =
            WiFi.status() == WL_CONNECTED;

        if (connected != wifiReady) {
            wifiReady = connected;
            tcpStateChanged = true;

            if (!connected) slaveTCPStop();
        }

        if (!wifiReady && mdnsStarted) {
            MDNS.end();
            mdnsStarted = false;
        }

        if (wifiReady && !mdnsStarted) {
            mdnsStarted =
                MDNS.begin("termostat-master");
        }

        if (requestResetWiFi &&
            millis() - resetRequestAt > 7500) {
            requestResetWiFi = false;
            if (provStage == PROV_SEARCH && !cancelProvisionRequested) {
                provisionRequested = true;
            }
        }

        if (cancelProvisionRequested) {
            cancelProvisionRequested = false;
            requestResetWiFi = false;
            provisionSendRequested = false;
            bleStop();

            if (!provisionRequested) {
                provStage = PROV_OFF;
                lastTCPTry = 0;
                BLE_DIAG("Provisioning cancelled; resuming TCP reconnect\n");
            }
        }

        if (provisionRequested) {
            provisionRequested = false;
            slaveTCPStop();
            startProvisioning();
        }

        if (provisionSendRequested) {
            provisionSendRequested = false;

            // KRYTYCZNA POPRAWKA:
            // connectToWiFi() ustawia PROV_SENDING.
            if (provStage == PROV_SENDING) {
                strlcpy(provisionMessage,
                        ScreenText::TXT_WYSYLANIE_DANYCH_DO_SLAVE,
                        sizeof(provisionMessage));
                networkUiDirty = true;

                bool ok = sendProvisionCredentials();

                if (!ok) {
                    provisionFail(
                        ScreenText::TXT_SLAVE_NIE_POLACZYL_SIE_Z_WIFI);
                } else {
                    Preferences p;
                    p.begin("settings", false);
                    p.putString("ssid", pendingSSID);
                    p.putString("pass", pendingPass);
                    p.putString("slaveip", slaveIPText);
                    p.end();

                    bleStop();

                    strlcpy(provisionMessage,
                            ScreenText::TXT_GOTOWE_RESTART_MASTER,
                            sizeof(provisionMessage));

                    networkUiDirty = true;
                    delay(400);
                    ESP.restart();
                }
            }
        }

        if (isScanningWifi) {
            int n = WiFi.scanComplete();

            if (n >= 0) {
                if (xSemaphoreTake(
                        wifiMux, portMAX_DELAY)) {
                    networkCount = min(n, MAX_NETWORKS);

                    for (int i = 0; i < networkCount; ++i) {
                        WiFi.SSID(i).toCharArray(
                            networkSSIDs[i], SSID_MAX_LEN);
                        networkRSSIs[i] = WiFi.RSSI(i);
                    }

                    xSemaphoreGive(wifiMux);
                }

                WiFi.scanDelete();
                isScanningWifi = false;

                // Nie przerzucaj ekranu podczas
                // wpisywania hasla.
                if (!isKeyboardMode) {
                    networkUiDirty = true;
                }
            } else if (n == WIFI_SCAN_FAILED) {
                isScanningWifi = false;
                if (!isKeyboardMode) {
                    networkUiDirty = true;
                }
            }
        }

        if (requestScanWifi &&
            !isScanningWifi &&
            !isKeyboardMode &&
            provStage == PROV_SELECT) {
            requestScanWifi = false;
            isScanningWifi = true;
            WiFi.scanNetworks(true);
        }

        if (wifiReady &&
            provStage == PROV_OFF &&
            !requestResetWiFi) {
            tcpMasterStep();
        } else if (requestResetWiFi && tcpSession) {
            tcpMasterStep();
        }

        if (tcpConnected &&
            millis() - lastTCPAck > TCP_VALID_MS) {
            tcpConnected = false;
            tcpStateChanged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

// ============================================================
// ZAPIS USTAWIEN TERMOSTATU
// ============================================================
void saveSettings() {
    float target;
    float hyst;

    if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
        target = targetTemp;
        hyst = hysteresis;
        xSemaphoreGive(dataMux);
    } else {
        return;
    }

    Preferences p;
    p.begin("settings", false);
    p.putFloat("targetTemp", target);
    p.putFloat("hysteresis", hyst);
    p.end();
}

void saveFailureSettings() {
    // Bez zmiany nastaw nie ma zapisu NVS ani komunikatu sukcesu.
    if (editFailureMode == failureMode &&
        editFailureTimeoutMin == failureTimeoutMin &&
        editRelayLow == relayLow) return;

    Preferences p;
    if (!p.begin("settings", false)) return;
    // NVS Mastera: zapis tylko zmienionych wartosci.
    bool ok = true;
    if (editFailureMode != failureMode)
        ok = (p.putUChar("failMode", editFailureMode) == 1) && ok;
    if (editFailureTimeoutMin != failureTimeoutMin)
        ok = (p.putUChar("failTime", editFailureTimeoutMin) == 1) && ok;
    if (editRelayLow != relayLow)
        ok = (p.putBool("relayLow", editRelayLow) == 1) && ok;
    p.end();
    if (!ok) return;

    failureMode = editFailureMode;
    failureTimeoutMin = editFailureTimeoutMin;
    relayLow = editRelayLow;
    configAckAt = 0;
    userConfigPending = true;
    configDirty = true;
}

// ==========================================================================
// LVGL 8.4: RYSOWANIE WYŁĄCZNIE W LV_EVENT_DRAW_MAIN
// Jeden obiekt LVGL; brak framebuffera 320x240 w RAM.
// Wszystkie współrzędne i obsługa dotyku pochodzą z oryginału.
// ==========================================================================
static lv_color_t rgb565(uint16_t value) {
    // Kolory TFT_* i paleta UI sa w RGB565; LVGL otrzymuje je bez zmian.
    lv_color_t c;
    c.full = value;
    return c;
}
static void lvFlush(lv_disp_drv_t* disp, const lv_area_t* a, lv_color_t* colors) {
    const int16_t w = a->x2 - a->x1 + 1;
    const int16_t h = a->y2 - a->y1 + 1;
    // LVGL RGB565 bez recznej zamiany kanalow R/B.
    uint16_t* pixels = reinterpret_cast<uint16_t*>(colors);
    const uint32_t count = uint32_t(w) * uint32_t(h);
    tft.startWrite();
    tft.setAddrWindow(a->x1, a->y1, w, h);
    tft.pushColors(pixels, count, true);
    tft.endWrite();
    ++lvFlushCounter;
    lv_disp_flush_ready(disp);
}
static void lvInvalidate() { if (lvReady && lvRoot) lv_obj_invalidate(lvRoot); }

struct Paint {
    lv_draw_ctx_t* ctx;
    void rect(int x,int y,int w,int h,uint16_t col,int radius=0,bool outline=false) {
        if (w<=0||h<=0) return;
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.radius=radius;
        if (outline) { d.bg_opa=LV_OPA_TRANSP; d.border_opa=LV_OPA_COVER; d.border_color=rgb565(col); d.border_width=1; }
        else { d.bg_opa=LV_OPA_COVER; d.bg_color=rgb565(col); d.border_opa=LV_OPA_TRANSP; }
        lv_area_t a={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+w-1),lv_coord_t(y+h-1)};
        lv_draw_rect(ctx,&d,&a);
    }
    void line(int x1,int y1,int x2,int y2,uint16_t col,int width=1) {
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color=rgb565(col); d.width=width;
        lv_point_t a={lv_coord_t(x1),lv_coord_t(y1)}, b={lv_coord_t(x2),lv_coord_t(y2)};
        lv_draw_line(ctx,&d,&a,&b);
    }
    void trendArrow(int x, int y, Trend trend, int halfHeight = 5) {
        if (trend == Trend::Unknown) return;

        constexpr uint16_t color = UI_MUTED_TEXT;
        line(x, y - halfHeight, x, y + halfHeight, color);
        if (trend == Trend::Rising || trend == Trend::Stable) {
            line(x - 3, y - halfHeight + 3, x, y - halfHeight, color);
            line(x, y - halfHeight, x + 3, y - halfHeight + 3, color);
        }
        if (trend == Trend::Falling || trend == Trend::Stable) {
            line(x - 3, y + halfHeight - 3, x, y + halfHeight, color);
            line(x, y + halfHeight, x + 3, y + halfHeight - 3, color);
        }
    }

    void circle(int cx,int cy,int r,uint16_t col,bool fill) {
        rect(cx-r,cy-r,r*2+1,r*2+1,col,LV_RADIUS_CIRCLE,!fill);
    }
    const lv_font_t* font(int size) {
        if (size>=5) return &lv_font_montserrat_40;
        if (size>=4) return &lv_font_montserrat_28;
        if (size>=3) return &lv_font_montserrat_24;
        if (size==2) return &lv_font_montserrat_16;
        return &lv_font_montserrat_12;
    }
    void text(const char* s,int x,int y,int w,int h,int size,uint16_t col,bool center=false,int letterSpacing=0) {
        if (!s) return;
        lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
        d.color=rgb565(col); d.font=font(size); d.letter_space=letterSpacing;
        d.align=center?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT;
        d.opa=LV_OPA_COVER;
        lv_area_t a={lv_coord_t(x),lv_coord_t(y),lv_coord_t(x+w-1),lv_coord_t(y+h-1)};
        lv_draw_label(ctx,&d,&a,s,nullptr);
    }
    void button(int x,int y,int w,int h,uint16_t bg,const char* s,int size=1,uint16_t fc=TFT_WHITE) {
        rect(x,y,w,h,bg,5);
        int th=(size>=3?28:(size==2?20:15));
        text(s,x+2,y+(h-th)/2,w-4,th,size,fc,true);
    }
    void bars(int x,int y,int rssi) {
        // Zwarta ikona Wi-Fi: 5 slupkow o stalej podstawie i rownych odstepach.
        // Nieaktywne slupki sa przygaszone, aktywne zielone.
        const int barsCount = (rssi > -50 ? 5 : rssi > -60 ? 4 :
                               rssi > -70 ? 3 : rssi > -80 ? 2 : 1);
        const int heights[5] = {3, 5, 8, 11, 14};
        const uint16_t inactive = UI_EDGE;
        for (int i = 0; i < 5; ++i) {
            rect(x + i * 4, y - heights[i], 3, heights[i],
                 i < barsCount ? TFT_GREEN : inactive, 1);
        }
    }
    void gear(int x,int y,uint16_t col) {
        circle(x,y,7,col,false);circle(x,y,2,col,true);
        for(int i=0;i<8;i++){
            const float a=i*3.14159265f/4;
            line(x+cosf(a)*5,y+sinf(a)*5,x+cosf(a)*11,y+sinf(a)*11,col);
        }
    }
    void flame(int x,int y,uint16_t col) {
        // Gotowa sylwetka ikony plomienia (19x21), zapisana w pamieci Flash.
        // Bez recznego skladania ikony z linii; barwa zalezy od stanu TCP.
        static const uint32_t sprite[21] = {
            0x00000,
            0x00000,
            0x00000,
            0x00000,
            0x00080,
            0x00180,
            0x031C0,
            0x033C0,
            0x07FE0,
            0x07FF0,
            0x0FFF0,
            0x0FBF8,
            0x0F278,
            0x0F078,
            0x07070,
            0x07070,
            0x03CE0,
            0x01FC0,
            0x00200,
            0x00000,
            0x00000
        };
        for (int row = 0; row < 21; ++row) {
            const uint32_t bits = sprite[row];
            int start = -1;
            for (int cx = 0; cx <= 19; ++cx) {
                const bool on = (cx < 19) && ((bits >> cx) & 1U);
                if (on && start < 0) start = cx;
                if (!on && start >= 0) {
                    rect(x - 9 + start, y - 10 + row, cx - start, 1, col);
                    start = -1;
                }
            }
        }
    }
    void thermometer(int x,int y,int state) {
        // Ikona kaloryfera 22x29 w dotychczasowym miejscu termometru.
        // Grzanie ON = czerwony, OFF = niebieski.
        const uint16_t c = (state == 1) ? TFT_RED : TFT_BLUE;
        const int left = x - 4;
        const int top = y;
        // Cztery zaokraglone czlony grzejnika.
        for (int i = 0; i < 4; ++i) {
            rect(left + i * 5, top + 3, 4, 23, c, 2);
        }
        // Kolektory gorny i dolny oraz krociec przyłączeniowy.
        rect(left - 1, top + 6, 22, 3, c, 1);
        rect(left - 1, top + 20, 22, 3, c, 1);
        rect(left + 18, top + 24, 3, 4, c, 1);
    }
};

static void renderFullScreen(Paint& p) {
    p.rect(0,0,320,240,UI_BG);
    if (isProvisionMenu) {
        p.text(ScreenText::TXT_KONFIGURACJA_SLAVE,10,8,300,25,2,TFT_WHITE);
        p.text(provisionMessage,10,60,300,20,1,provStage==PROV_ERROR?TFT_RED:TFT_YELLOW);
        const char* msg=provStage==PROV_SEARCH?ScreenText::TXT_WYMAGANA_OBECNOSC_SLAVE_BLE:provStage==PROV_ERROR?ScreenText::TXT_SPRAWDZ_SLAVE_I_PONOW:provStage==PROV_SENDING?ScreenText::TXT_TRWA_LACZENIE_SLAVE_Z_WIFI:"";
        p.text(msg,10,95,300,20,1,TFT_WHITE);
        p.button(10,190,145,40,UI_ORANGE,ScreenText::TXT_ANULUJ,2);
        p.button(165,190,145,40,TFT_BLUE,ScreenText::TXT_PONOW,2);
        return;
    }
    if (isFailureMenu) {
        // Osobny ekran dostosowany do 320x240, bez dodatkowych fontow LVGL.
        p.text(ScreenText::TXT_AWARIA_PRZEKAZNIK,10,5,300,24,2,TFT_WHITE);
        p.line(10,29,310,29,UI_EDGE);
        p.text(ScreenText::TXT_AKTYWACJA_PRZEKAZNIKA_IN,10,39,215,16,1,UI_MUTED_TEXT);
        p.button(174,58,64,32,editRelayLow?TFT_DARKGREEN:UI_EDGE,ScreenText::TXT_LOW,2);
        p.button(244,58,66,32,!editRelayLow?TFT_DARKGREEN:UI_EDGE,ScreenText::TXT_HIGH,2);
        p.text(ScreenText::TXT_TRYB_PO_UTRACIE_TCP,10,98,270,17,1,UI_MUTED_TEXT);
        p.button(10,119,72,32,editFailureMode==1?TFT_DARKGREEN:UI_EDGE,ScreenText::TXT_ON,2);
        p.button(90,119,72,32,editFailureMode==0?TFT_DARKGREEN:UI_EDGE,ScreenText::TXT_OFF,2);
        p.button(170,119,140,32,editFailureMode==2?TFT_BLUE:UI_EDGE,ScreenText::TXT_OSTATNI_STAN,1);
        p.text(ScreenText::TXT_PO_ILU_MINUTACH,10,163,142,19,1,UI_MUTED_TEXT);
        p.button(165,157,40,32,UI_EDGE,ScreenText::TXT_MINUS,2);
        char t[24];snprintf(t,sizeof(t),ScreenText::TXT_CZAS_MINUTY,unsigned(editFailureTimeoutMin));
        p.text(t,209,163,57,20,1,TFT_YELLOW,true);
        p.button(270,157,40,32,UI_EDGE,ScreenText::TXT_PLUS,2);
        // Nie wyswietlaj sukcesu od samego startu / automatycznego ACK.
        const uint32_t ackAt = configAckAt;
        if (userConfigPending) {
            p.text(tcpConnected ? ScreenText::TXT_OCZEKIWANIE_NA_POTWIERDZENIE_SLAVE : ScreenText::TXT_OCZEKIWANIE_NA_POLACZENIE_TCP,10,193,300,13,1,TFT_YELLOW,true);
        } else if (ackAt != 0 && millis() - ackAt < 3000UL) {
            p.text(ScreenText::TXT_ZAPISANO_W_SLAVE,10,193,300,13,1,TFT_GREEN,true);
        }
        p.button(10,209,145,27,UI_ORANGE,ScreenText::TXT_ANULUJ);
        p.button(165,209,145,27,TFT_BLUE,ScreenText::TXT_ZAPISZ);
        return;
    }
    if (isSettingsMenu) {
        p.text(ScreenText::TXT_USTAWIENIA_SIECI,10,8,300,25,2,TFT_WHITE);
        char b[100];snprintf(b,sizeof(b),ScreenText::TXT_SSID_FORMAT,savedSSID[0]?savedSSID:ScreenText::TXT_BRAK_DANYCH);p.text(b,10,42,300,20,1,TFT_WHITE);
        snprintf(b,sizeof(b),ScreenText::TXT_IP_SLAVE_FORMAT,slaveIPText);p.text(b,10,60,300,20,1,TFT_WHITE);
        snprintf(b,sizeof(b),ScreenText::TXT_IP_MASTER_FORMAT,WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : ScreenText::TXT_BRAK_DANYCH);p.text(b,10,76,300,20,1,TFT_WHITE);
        p.button(10,96,300,35,TFT_BLUE,ScreenText::TXT_ZMIEN_SIEC,2);
        p.button(10,140,145,35,UI_EDGE,ScreenText::TXT_KOD_HOMEKIT);
        p.button(165,140,145,35,TFT_RED,ScreenText::TXT_FABRYCZNY_2X);
        p.button(10,180,300,22,TFT_BLUE,ScreenText::TXT_AWARIA_PRZEKAZNIK);
        p.button(10,207,300,29,UI_ORANGE,ScreenText::TXT_POWROT,2);
        return;
    }
    if (isMacMenu) {
        p.text(ScreenText::TXT_DIAGNOSTYKA_TCP,10,10,300,27,2,TFT_WHITE);
        char b[100];snprintf(b,sizeof(b),ScreenText::TXT_IP_SLAVE_FORMAT,slaveIPText);p.text(b,10,55,300,20,1,TFT_WHITE);
        snprintf(b,sizeof(b),ScreenText::TXT_TCP_STAN_FORMAT,tcpConnected?ScreenText::TXT_POLACZONO:ScreenText::TXT_ROZLACZONO);p.text(b,10,78,300,20,1,TFT_WHITE);
        snprintf(b,sizeof(b),ScreenText::TXT_IP_MASTER_FORMAT,WiFi.localIP().toString().c_str());p.text(b,10,100,300,20,1,TFT_WHITE);
        p.text(ScreenText::TXT_BLE_WYLACZONE_W_TRYBIE_PRACY,10,120,300,20,1,TFT_WHITE);
        p.button(10,200,300,35,UI_ORANGE,ScreenText::TXT_POWROT,2);
        return;
    }
    if (isHkCodeMenu) {
        p.text(ScreenText::TXT_KOD_PAROWANIA_HK,10,10,300,27,2,TFT_WHITE);
        if(hkCodeJustGenerated) {
            p.text(ScreenText::TXT_NOWY_KOD,20,80,260,22,2,TFT_YELLOW);
            p.text(hkCode,20,120,280,38,3,TFT_GREEN);
            p.text(ScreenText::TXT_RESTART_ZA_3_SEKUNDY,20,170,290,18,1,TFT_WHITE);
        } else {
            p.text(ScreenText::TXT_AKTUALNY_KOD,10,50,300,20,1,UI_MUTED_TEXT);
            p.text(hkCode,10,80,290,35,3,TFT_GREEN);
            p.text(ScreenText::TXT_NOWY_KOD_ZOSTANIE_WYGENEROWANY,10,130,310,18,1,UI_MUTED_TEXT);
            p.text(ScreenText::TXT_I_URZADZENIE_SIE_ZRESTARTUJE,10,145,310,18,1,UI_MUTED_TEXT);
            p.button(10,200,140,35,UI_ORANGE,ScreenText::TXT_ANULUJ,2);
            p.button(160,200,150,35,TFT_RED,ScreenText::TXT_GEN_NOWY_KOD);
        }
        return;
    }
    if (isKeyboardMode) {
        p.rect(10,8,260,30,UI_CARD,4);
        p.rect(275,8,35,30,showPassword?TFT_GREEN:UI_EDGE,4);
        char pass[PASS_MAX_LEN];
        if(showPassword)strlcpy(pass,inputPassword,sizeof(pass));
        else {size_t n=strlen(inputPassword);memset(pass,'*',n);pass[n]=0;}
        p.text(pass,18,13,245,24,2,TFT_WHITE);
        p.text(ScreenText::TXT_OKO,278,16,32,20,1,TFT_WHITE);
        static const char lower[4][11]={"1234567890","qwertyuiop","asdfghjkl;","zxcvbnm.,_"};
        static const char upper[4][11]={"!@#$%^&*()","QWERTYUIOP","ASDFGHJKL:","ZXCVBNM?-+"};
        static const char symbols[4][11]={"[]{}<>/\\|~","`'\"=+-_*&^","%$#@!?.,:;","()[]<>/\\|~"};
        for(int row=0;row<4;row++)for(int col=0;col<10;col++){
            const int x=10+col*31,y=45+row*28;
            p.rect(x,y,28,25,UI_EDGE,3);
            const char c=keyboardMode==0?lower[row][col]:keyboardMode==1?upper[row][col]:symbols[row][col];
            char key[2]={c,0};p.text(key,x,y+4,28,16,1,TFT_WHITE,true);
        }
        p.button(10,165,300,25,UI_EDGE,ScreenText::TXT_SPACJA);
        p.button(10,195,55,35,keyboardMode==1?TFT_BLUE:UI_EDGE,ScreenText::TXT_SHIFT);
        p.button(70,195,45,35,keyboardMode==2?TFT_BLUE:UI_EDGE,ScreenText::TXT_123);
        p.button(120,195,50,35,TFT_RED,ScreenText::TXT_USUN);
        p.button(175,195,65,35,UI_ORANGE,ScreenText::TXT_ANULUJ);
        p.button(245,195,65,35,TFT_GREEN,ScreenText::TXT_OK,2);
        return;
    }
    if (isWifiMenu) {
        p.text(ScreenText::TXT_WYBIERZ_SIEC_WIFI,10,10,280,30,2,TFT_WHITE);
        char b[40];snprintf(b,sizeof(b),ScreenText::TXT_KOD_HK_FORMAT,hkCode);p.text(b,10,45,210,16,1,TFT_GREEN);
        if(isScanningWifi){char dots[32];strlcpy(dots,ScreenText::TXT_SKANOWANIE,sizeof(dots));for(unsigned i=0;i<wifiScanAnimDots;i++)strcat(dots,".");p.text(dots,225,10,95,18,1,TFT_YELLOW);}
        if (xSemaphoreTake(wifiMux,pdMS_TO_TICKS(15))==pdTRUE) {
            for(int i=0;i<networkCount && i<5;i++){
                int y=WIFI_LIST_TOP+i*WIFI_ROW_STEP;
                p.rect(10,y,300,WIFI_ROW_HEIGHT,UI_CARD,4);
                p.text(networkSSIDs[i],20,y+5,250,19,1,TFT_WHITE);
                p.bars(280,y+18,networkRSSIs[i]);
            }
            xSemaphoreGive(wifiMux);
        }
        p.button(5,200,100,35,TFT_RED,ScreenText::TXT_ANULUJ);
        p.button(115,200,100,35,TFT_GREEN,ScreenText::TXT_KOD_HK);
        return;
    }
    // EKRAN GŁÓWNY. Rozmiary kart / suwaki / obszary dotyku identyczne.
    p.flame(15,14,tcpConnected?TFT_GREEN:TFT_RED);
    char b[64];
    if(tcpConnected && slaveRSSI!=-127)snprintf(b,sizeof(b),ScreenText::TXT_TCP_SYGNAL_FORMAT,(int)slaveRSSI);
    else strlcpy(b,ScreenText::TXT_TCP_BRAK_POLACZENIA,sizeof(b));
    uint16_t sigColor=(slaveRSSI>=-67)?TFT_GREEN:(slaveRSSI>=-75)?TFT_YELLOW:TFT_RED;
    p.text(b,29,8,120,18,1,tcpConnected?sigColor:TFT_RED);
    if (WiFi.status()==WL_CONNECTED) {
        const int wifiRssi = WiFi.RSSI();
        const uint16_t wifiColor = wifiRssi >= -67 ? TFT_GREEN :
                                   wifiRssi >= -75 ? TFT_YELLOW : TFT_RED;
        snprintf(b, sizeof(b), ScreenText::TXT_WIFI_SYGNAL_FORMAT, wifiRssi);
        p.text(b, WIFI_STATUS_X, 8, 265 - WIFI_STATUS_X, 18, 1, wifiColor);
        // Gotowy symbol Wi-Fi z LVGL (Font Awesome).
        p.text(LV_SYMBOL_WIFI, 265, 2, 24, 24, 2, wifiColor, true);
    } else {
        p.text(ScreenText::TXT_WIFI_BRAK_POLACZENIA, WIFI_STATUS_X, 8, 265 - WIFI_STATUS_X, 18, 1, TFT_RED);
    }
    p.text(LV_SYMBOL_SETTINGS, 289, 2, 24, 24, 2,
           WiFi.status()==WL_CONNECTED?TFT_GREEN:TFT_RED, true);
    // Wysokie panele temperatur; napisy w 2 wierszach, bez nachodzenia.
    p.rect(10,30,145,105,UI_CARD,6);p.rect(10,30,145,105,UI_EDGE,6,true);
    p.text(ScreenText::TXT_AKTUALNA,19,35,128,15,1,UI_MUTED_TEXT);
    p.text(ScreenText::TXT_TEMPERATURA,19,49,128,15,1,UI_MUTED_TEXT);
    p.rect(165,30,145,105,UI_CARD,6);p.rect(165,30,145,105,UI_EDGE,6,true);
    p.text(ScreenText::TXT_ZADANA,174,35,128,15,1,UI_MUTED_TEXT);
    p.text(ScreenText::TXT_TEMPERATURA,174,49,128,15,1,UI_MUTED_TEXT);
    // Dolne panele nizsze o 17 px, ich strefy dotyku pozostaja oryginalne.
    p.rect(10,140,94,53,UI_CARD,6);p.rect(10,140,94,53,UI_EDGE,6,true);
    p.text(ScreenText::TXT_HISTEREZA,15,144,87,15,1,UI_MUTED_TEXT);
    p.rect(113,140,94,53,UI_CARD,6);p.rect(113,140,94,53,UI_EDGE,6,true);
    p.text(ScreenText::TXT_WILGOTNOSC,118,144,87,15,1,UI_MUTED_TEXT);
    p.rect(216,140,94,53,UI_CARD,6);p.rect(216,140,94,53,UI_EDGE,6,true);
    p.text(ScreenText::TXT_CISNIENIE,221,144,87,15,1,UI_MUTED_TEXT);
    float temp,hum,press,target,hyst;int sent;
    Trend shownTempTrend = Trend::Unknown;
    Trend shownHumidityTrend = Trend::Unknown;
    uint16_t shownHumidityColor = TFT_DARKGREY;
    Trend shownPressureTrend = Trend::Unknown;
    if(xSemaphoreTake(dataMux,pdMS_TO_TICKS(15))==pdTRUE) {
        temp=lastTemp;hum=lastHum;press=lastPress;target=targetTemp;hyst=hysteresis;sent=lastSentState;
        shownTempTrend = tempTrend;
        shownHumidityTrend = humidityTrend;
        shownHumidityColor = humidityColor;
        shownPressureTrend = pressureTrend;
        xSemaphoreGive(dataMux);
    }else {temp=NAN;hum=NAN;press=NAN;target=targetTemp;hyst=hysteresis;sent=lastSentState;}
    // Glowne liczby: 40 px. Jednostki stopni: 16 px, bez zmian.
    if(isnan(temp))strlcpy(b,ScreenText::TXT_BRAK_TEMPERATURY,sizeof(b));else snprintf(b,sizeof(b),ScreenText::TXT_TEMPERATURA_FORMAT,temp);
    const bool compactTemp = strlen(b) >= 5;
    // Wysokosc strzalki zblizona do wysokosci cyfr Montserrat 28/40.
    p.trendArrow(18, 103, shownTempTrend, compactTemp ? 10 : 14);
    const int tempFont = compactTemp ? 4 : 5;
    const int tempY = compactTemp ? 88 : 80;
    p.text(b,26,tempY,94,47,tempFont,TFT_RED,true);
    p.text(b,27,tempY,94,47,tempFont,TFT_RED,true);  // pogrubienie cyfr +1 px
    p.text(ScreenText::TXT_STOPNIE_C,121,94,30,24,2,TFT_RED);
    snprintf(b,sizeof(b),ScreenText::TXT_TEMPERATURA_FORMAT,target);
    p.text(b,168,80,105,47,5,TFT_BLUE,true);
    p.text(b,169,80,105,47,5,TFT_BLUE,true);  // pogrubienie cyfr +1 px
    p.text(ScreenText::TXT_STOPNIE_C,272,94,30,24,2,TFT_BLUE);
    p.thermometer(285,35,sent);
    // Wartosci dolnych paneli: Montserrat 16 px zamiast 12 px.
    snprintf(b,sizeof(b),ScreenText::TXT_HISTEREZA_FORMAT,hyst);p.text(b,10,164,94,24,2,TFT_DARKGREEN,true);
    if(isnan(hum))strlcpy(b,ScreenText::TXT_BRAK_WILGOTNOSCI,sizeof(b));else snprintf(b,sizeof(b),ScreenText::TXT_WILGOTNOSC_FORMAT,hum);
    p.trendArrow(122, 173, shownHumidityTrend, 6);
    p.text(b,130,164,77,24,2,shownHumidityColor,true);
    if(isnan(press))strlcpy(b,ScreenText::TXT_BRAK_CISNIENIA,sizeof(b));else snprintf(b,sizeof(b),ScreenText::TXT_CISNIENIE_FORMAT,press);
    p.trendArrow(225, 173, shownPressureTrend, 6);
    p.text(b,233,164,77,24,2,TFT_DARKGREY,true);
    if(editHysteresis) {
        p.rect(8,138,98,57,TFT_MAROON,8,true);
        p.rect(9,139,96,55,TFT_MAROON,7,true);
    } else {
        p.rect(163,28,149,109,TFT_MAROON,8,true);
        p.rect(164,29,147,107,TFT_MAROON,7,true);
    }
    p.button(10,205,45,23,UI_EDGE,ScreenText::TXT_MINUS,2);
    p.button(265,205,45,23,UI_EDGE,ScreenText::TXT_PLUS,2);
    p.line(65,207,255,207,TFT_DARKGREY);
    p.line(65,204,65,211,TFT_DARKGREY);
    p.line(255,204,255,211,TFT_DARKGREY);
    int knob=editHysteresis?65+int((hyst-0.1f)/4.9f*190):65+int((target-15.f)/20.f*190);
    if (knob < 65) knob = 65;
    if (knob > 255) knob = 255;
    p.rect(knob-6,201,12,13,TFT_YELLOW,3);
    p.rect(knob-6,201,12,13,TFT_WHITE,3,true);
    p.text(editHysteresis?ScreenText::TXT_HISTEREZA:ScreenText::TXT_ZADANA_TEMPERATURA,60,225,200,15,1,UI_MUTED_TEXT,true);
}

static void lvDrawScreen(lv_event_t* e) {
    lv_draw_ctx_t* ctx=lv_event_get_draw_ctx(e);
    if(!ctx)return;
    Paint p{ctx};renderFullScreen(p);
}

// Zgodność dotychczasowych wywołań aplikacji: teraz tylko unieważniają LVGL.
void drawUI() {
    isWifiMenu=false;isKeyboardMode=false;isMacMenu=false;
    isHkCodeMenu=false;isSettingsMenu=false;isProvisionMenu=false;isFailureMenu=false;
    hkCodeJustGenerated=false;lvInvalidate();
}
void updateData(){lvInvalidate();}
void _updateDataUnlocked(){lvInvalidate();}
void drawTopBar(){lvInvalidate();}
void _drawTopBarUnlocked(){lvInvalidate();}
void drawSliderSection(){lvInvalidate();}
void _drawSliderSectionUnlocked(){lvInvalidate();}
void drawMacMenu(){isMacMenu=true;isSettingsMenu=false;lvInvalidate();}
void drawSettingsMenu(){isSettingsMenu=true;isFailureMenu=false;isWifiMenu=false;isHkCodeMenu=false;isMacMenu=false;lvInvalidate();}
void drawFailureMenu(){
    editFailureMode=failureMode;
    editFailureTimeoutMin=failureTimeoutMin;
    editRelayLow=relayLow;
    isSettingsMenu=false;isFailureMenu=true;lvInvalidate();
}
void drawProvisionScreen(){isProvisionMenu=true;lvInvalidate();}
void drawHkCodeMenu(){isHkCodeMenu=true;hkCodeJustGenerated=false;lvInvalidate();}
void drawWifiMenu(bool startScan){isWifiMenu=true;lvInvalidate();if(startScan)requestScanWifi=true;}
void drawWifiScanningStatus(){lvInvalidate();}
void drawKeyboard(){isKeyboardMode=true;lvInvalidate();}
int getSignalBars(int rssi){return rssi>-50?5:rssi>-60?4:rssi>-70?3:rssi>-80?2:1;}
void redrawCurrentScreen(){lvInvalidate();}
// Deklaracje pierwotnych funkcji pomocniczych zachowane jako puste adaptery.
void drawCenteredText(const char*,int,int,int,int,uint8_t,uint16_t,uint16_t){}
void drawGearIcon(int,int,int,uint16_t){}
void drawBluetoothIcon(int,int,int,uint16_t){}
void drawFlameIcon(int,int,int,uint16_t){}
void drawThermometerIcon(int,int,int,uint16_t){}

// ============================================================
// PRZEKAZANIE SSID/HASLA DO ZADANIA SIECIOWEGO
// ============================================================
void connectToWiFi(
    const char* ssid,
    const char* pass
) {
    if (provStage != PROV_SELECT ||
        !bleClient ||
        !bleClient->isConnected()) {
        provisionFail(ScreenText::TXT_UTRACONO_POLACZENIE_BLE);
        return;
    }

    strlcpy(pendingSSID, ssid, sizeof(pendingSSID));
    strlcpy(pendingPass, pass, sizeof(pendingPass));

    isWifiMenu = false;
    isKeyboardMode = false;
    isProvisionMenu = true;

    provStage = PROV_SENDING;

    strlcpy(provisionMessage,
            ScreenText::TXT_KONFIGURUJE_SLAVE_WIFI,
            sizeof(provisionMessage));

    drawProvisionScreen();
    provisionSendRequested = true;
}

// ============================================================
// DOTYK - ORYGINALNE WSPOLRZEDNE I SUWAK
// ============================================================
static bool touchInRect(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width &&
           y >= top && y < top + height;
}

void handleTouch(int x, int y) {
    if (isFailureMenu) {
        if (touchInRect(x, y, 0, 209, 320, 31)) {
            if (x >= 160) {
                saveFailureSettings();
                // Pozostajemy na ekranie, by pokazac odpowiedz Slave.
                lvInvalidate();
            } else {
                isFailureMenu=false;
                drawSettingsMenu();
            }
            return;
        }
        if (y >= 55 && y < 94) {
            if (x >= 171 && x < 241) editRelayLow=true;
            else if (x >= 241 && x < 320) editRelayLow=false;
        } else if (y >= 116 && y < 154) {
            if (x >= 0 && x < 86) editFailureMode=1;
            else if (x >= 86 && x < 166) editFailureMode=0;
            else if (x >= 166 && x < 320) editFailureMode=2;
        } else if (y >= 155 && y < 193) {
            if (x >= 160 && x < 208 && editFailureTimeoutMin > 1) --editFailureTimeoutMin;
            else if (x >= 268 && x < 320 && editFailureTimeoutMin < 30) ++editFailureTimeoutMin;
        }
        lvInvalidate();
        return;
    }
    if (isProvisionMenu) {
        // Margines do dolnej krawedzi ekranu; bez zmiany wygladu przycisku.
        if (touchInRect(x, y, 0, 187, 160, 53) &&
            provStage != PROV_SENDING) {
            requestResetWiFi = false;
            provisionRequested = false;
            cancelProvisionRequested = true;
            provStage = PROV_OFF;
            isProvisionMenu = false;
            drawSettingsMenu();
        } else if (touchInRect(x, y, 160, 187, 160, 53) &&
                   provStage == PROV_ERROR) {
            provisionRequested = true;
            provStage = PROV_SEARCH;
            strlcpy(provisionMessage,
                    ScreenText::TXT_PONOWNE_SZUKANIE_SLAVE,
                    sizeof(provisionMessage));
            drawProvisionScreen();
        }
        return;
    }

    if (isSettingsMenu) {
        if (touchInRect(x, y, 0, 179, 320, 26)) {
            drawFailureMenu();
            return;
        }
        if (touchInRect(x, y, 0, 205, 320, 35)) {
            isSettingsMenu = false;
            drawUI();
            updateData();
            return;
        }

        if (touchInRect(x, y, 0, 93, 320, 42)) {
            isSettingsMenu = false;
            isProvisionMenu = true;
            provStage = PROV_SEARCH;

            strlcpy(provisionMessage,
                    ScreenText::TXT_URUCHAMIANIE_BLE_SLAVE,
                    sizeof(provisionMessage));

            drawProvisionScreen();

            if (tcpSession && tcpConnected) {
                requestResetWiFi = true;
                resetRequestAt = millis();
            } else {
                provisionRequested = true;
            }
            return;
        }

        if (touchInRect(x, y, 0, 137, 160, 41)) {
            isSettingsMenu = false;
            drawHkCodeMenu();
            return;
        }

        if (touchInRect(x, y, 160, 137, 160, 41)) {
            static uint32_t resetTap = 0;

            if (millis() - resetTap < 2500) {
                factoryReset();
            }

            resetTap = millis();
            return;
        }
        return;
    }

    if (isMacMenu) {
        if (touchInRect(x, y, 0, 197, 320, 43)) {
            isMacMenu = false;
            drawUI();
            updateData();
        }
        return;
    }

    if (isHkCodeMenu) {
        if (touchInRect(x, y, 0, 197, 155, 43)) {
            isHkCodeMenu = false;

            if (provStage == PROV_SELECT) {
                drawWifiMenu(false);
            } else {
                drawSettingsMenu();
            }
            return;
        }

        if (touchInRect(x, y, 155, 197, 165, 43)) {
            generateAndSaveHkCode();

            hkCodeJustGenerated=true;
            lvInvalidate();
            lv_timer_handler();

            delay(3000);
            ESP.restart();
            return;
        }
        return;
    }

    if (isKeyboardMode) {
        if (touchInRect(x, y, 272, 5, 48, 36)) {
            showPassword = !showPassword;
            drawKeyboard();
            return;
        }

        if (y >= 194 && y < 240) {
            if (x >= 0 && x < 68) {
                keyboardMode =
                    keyboardMode == 1 ? 0 : 1;
                drawKeyboard();
            } else if (x >= 68 && x < 118) {
                keyboardMode =
                    keyboardMode == 2 ? 0 : 2;
                drawKeyboard();
            } else if (x >= 118 && x < 173) {
                int len = strlen(inputPassword);
                if (len > 0)
                    inputPassword[len - 1] = '\0';
                drawKeyboard();
            } else if (x >= 173 && x < 243) {
                isKeyboardMode = false;
                drawWifiMenu(false);
            } else if (x >= 243 && x < 320) {
                if (selectedNetworkIndex < 0 ||
                    selectedNetworkIndex >= networkCount) {
                    drawWifiMenu(false);
                    return;
                }

                char localSSID[SSID_MAX_LEN];

                if (xSemaphoreTake(
                        wifiMux, portMAX_DELAY)) {
                    strncpy(localSSID,
                            networkSSIDs[selectedNetworkIndex],
                            SSID_MAX_LEN);
                    xSemaphoreGive(wifiMux);
                } else {
                    return;
                }

                isKeyboardMode = false;
                connectToWiFi(localSSID, inputPassword);
            }
            return;
        }

        if (touchInRect(x, y, 0, 163, 320, 30)) {
            int len = strlen(inputPassword);

            if (len < PASS_MAX_LEN - 1) {
                inputPassword[len] = ' ';
                inputPassword[len + 1] = '\0';
            }
            drawKeyboard();
            return;
        }

        const char keysLower[4][10] = {
            {'1','2','3','4','5','6','7','8','9','0'},
            {'q','w','e','r','t','y','u','i','o','p'},
            {'a','s','d','f','g','h','j','k','l',';'},
            {'z','x','c','v','b','n','m','.',',','_'}
        };
        const char keysUpper[4][10] = {
            {'!','@','#','$','%','^','&','*','(',')'},
            {'Q','W','E','R','T','Y','U','I','O','P'},
            {'A','S','D','F','G','H','J','K','L',':'},
            {'Z','X','C','V','B','N','M','?','-','+'}
        };
        const char keysSymbols[4][10] = {
            {'[',']','{','}','<','>','/','\\','|','~'},
            {'`','\'','"','=','+','-','_','*','&','^'},
            {'%','$','#','@','!','?','.',',',':',';'},
            {'(',')','[',']','<','>','/','\\','|','~'}
        };

        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 10; ++col) {
                const int left = col == 0 ? 0 : 8 + col * 31;
                const int right = col == 9 ? 320 : 8 + (col + 1) * 31;
                const int top = 43 + row * 28;

                if (touchInRect(x, y, left, top, right - left, 28)) {

                    char c = keyboardMode == 0
                        ? keysLower[row][col]
                        : keyboardMode == 1
                            ? keysUpper[row][col]
                            : keysSymbols[row][col];

                    int len = strlen(inputPassword);

                    if (len < PASS_MAX_LEN - 1) {
                        inputPassword[len] = c;
                        inputPassword[len + 1] = '\0';
                    }

                    drawKeyboard();
                    return;
                }
            }
        }
        return;
    }

    if (isWifiMenu) {
        // Lewa/dolna krawedz nalezy do ANULUJ; KOD HK zaczyna sie od x=115.
        if (touchInRect(x, y, 0, 200, 110, 40)) {
            isWifiMenu = false;
            requestScanWifi = false;
            requestResetWiFi = false;
            provisionSendRequested = false;
            provisionRequested = false;
            cancelProvisionRequested = true;
            provStage = PROV_OFF;

            drawUI();
            updateData();
            return;
        }

        if (touchInRect(x, y, 110, 200, 110, 40)) {
            isWifiMenu = false;
            drawHkCodeMenu();
            return;
        }

        for (int i = 0; i < min(networkCount, 5); ++i) {
            const int yPos = WIFI_LIST_TOP + i * WIFI_ROW_STEP;

            if (touchInRect(x, y, 0, yPos, 320, WIFI_ROW_STEP)) {
                selectedNetworkIndex = i;
                inputPassword[0] = '\0';
                keyboardMode = 0;
                showPassword = false;
                drawKeyboard();
                return;
            }
        }
        return;
    }

    // GLOWNY EKRAN - ORYGINALNE POLA DOTYKU
    if (x >= 0 && x <= 30 &&
        y >= 0 && y <= 28) {
        drawMacMenu();
        return;
    }

    if (x >= 285 && x <= 319 &&
        y >= 0 && y <= 28) {
        drawSettingsMenu();
        return;
    }

    if (touchInRect(x, y, 160, 30, 160, 106)) {
        editHysteresis = false;
        drawSliderSection();
        updateData();
        return;
    }

    if (touchInRect(x, y, 0, 137, 109, 58)) {
        editHysteresis = true;
        drawSliderSection();
        updateData();
        return;
    }

    if (y >= 195 && y <= 239) {
        if (x >= 0 && x < 60) {
            if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
                if (editHysteresis) {
                    hysteresis -= 0.1;
                    if (hysteresis < 0.1)
                        hysteresis = 0.1;
                } else {
                    targetTemp -= 0.1;
                    if (targetTemp < 15.0)
                        targetTemp = 15.0;
                }

                pendingSave = true;
                lastChangeTime = millis();
                xSemaphoreGive(dataMux);
            }

            lastThermoState = -2;
            drawSliderSection();
            updateData();
            return;
        }

        if (x >= 260 && x < 320) {
            if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
                if (editHysteresis) {
                    hysteresis += 0.1;
                    if (hysteresis > 5.0)
                        hysteresis = 5.0;
                } else {
                    targetTemp += 0.1;
                    if (targetTemp > 35.0)
                        targetTemp = 35.0;
                }

                pendingSave = true;
                lastChangeTime = millis();
                xSemaphoreGive(dataMux);
            }

            lastThermoState = -2;
            drawSliderSection();
            updateData();
            return;
        }
    }

    if (touchInRect(x, y, 60, 195, 200, 45)) {
        if (xSemaphoreTake(dataMux, portMAX_DELAY)) {
            if (editHysteresis) {
                hysteresis =
                    0.1 + float(x - 65) / 190.0 * 4.9;
                if (hysteresis < 0.1)
                    hysteresis = 0.1;
                if (hysteresis > 5.0)
                    hysteresis = 5.0;
            } else {
                targetTemp =
                    15.0 + float(x - 65) / 190.0 * 20.0;
                if (targetTemp < 15.0)
                    targetTemp = 15.0;
                if (targetTemp > 35.0)
                    targetTemp = 35.0;
            }

            pendingSave = true;
            lastChangeTime = millis();
            xSemaphoreGive(dataMux);
        }

        lastThermoState = -2;
        drawSliderSection();
        updateData();
        return;
    }
}
