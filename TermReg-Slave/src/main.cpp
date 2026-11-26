#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "SecureLink.h"

// ============================================================
// DIAGNOSTYKA
// W platformio.ini: -D DIAG=1
// ============================================================
#ifndef DIAG
#define DIAG 0
#endif

#if DIAG
#define DLOG(...) Serial.printf(__VA_ARGS__)
#define DPRINTLN(x) Serial.println(x)
#else
#define DLOG(...) do {} while (0)
#define DPRINTLN(x) do {} while (0)
#endif

// ============================================================
// ORYGINALNE PINY I POLARYZACJA
// ============================================================
#define RELAY_PIN 4
#define LED_PIN 8
#define RESET_BTN 9

// Domyslnie zachowujemy zgodnosc ze starym Slave: HIGH=ON.
// Polaryzacja przychodzi z Mastera i jest zapisywana w NVS.
bool relayActiveLow = false;
uint8_t failureMode = 2;        // 0=OFF, 1=ON, 2=OSTATNI STAN
uint8_t failureTimeoutMin = 10; // 1..30 minut

inline uint8_t relayLevel(bool heating) {
    return heating ? (relayActiveLow ? LOW : HIGH)
                   : (relayActiveLow ? HIGH : LOW);
}

// ============================================================
// PARAMETRY CZASOWE
// ============================================================
const uint32_t TCP_IDLE_MS = 5000;
// Timeout awaryjny konfigurowalny (patrz failureTimeoutMin).
const uint32_t WIFI_RETRY_MS = 10000;
// Po minucie bez zapisanego Wi-Fi uruchom BLE bez resetu NVS.
const uint32_t AUTO_BLE_AFTER_MS = 60000;
// W trybie BLE co minute sprobuj odzyskac poprzednie Wi-Fi.
// Bez skanowania Mesh, aby nie blokowac reklam BLE.
const uint32_t AUTO_BLE_WIFI_RETRY_MS = 60000;
const uint32_t BOOT_HOLD_MS = 5000;
const uint32_t BLE_SUCCESS_GRACE_MS = 18000;
// Powrot po anulowaniu przed polaczeniem BLE; dotyczy tylko zapisanej sieci.
const uint32_t BLE_RECONFIG_IDLE_MS = 30000;

#if DIAG
const uint32_t DIAG_INTERVAL_MS = 10000;
#endif

// ============================================================
// OBIEKTY
// ============================================================
Preferences prefs;

WiFiServer server(SecureLink::PORT);
WiFiClient peer;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleChallengeChr = nullptr;
NimBLECharacteristic* bleAckChr = nullptr;

uint8_t bleChallenge[16] = {};

volatile bool bleEnabled = false;
volatile bool blePeerConnected = false;
volatile bool blePeerDisconnected = false;

uint16_t bleConnHandle = 0;

bool provisionMode = false;
bool provisionSuccess = false;
uint32_t provisionSuccessAt = 0;
uint32_t bleReconfigureIdleSince = 0;

struct ProvisionCreds {
    char ssid[33];
    char pass[65];
};

QueueHandle_t credentialsQueue = nullptr;

String wifiSSID;
String wifiPASS;

// ============================================================
// TCP
// ============================================================
uint32_t lastWiFiTry = 0;
uint32_t wifiLostSince = 0;
bool autoBLEFallback = false;
uint32_t lastTCP = 0;
uint32_t lastAccepted = 0;
uint32_t lastSequence = 0;

uint8_t tcpChallenge[16] = {};

bool sessionReady = false;
bool serverStarted = false;

// ============================================================
// PRZEKAZNIK
// ============================================================
int relayState = 0;

bool validCommandReceived = false;
bool failsafeTriggered = false;
uint32_t bootSafetyStart = 0;

// ============================================================
// LED
// ============================================================
uint32_t lastLedToggle = 0;
bool blink = false;
uint32_t nextLedPhase = 0;
uint8_t ledPhase = 0;

// ============================================================
// PRZYCISK BOOT
// ============================================================
uint32_t bootPressedAt = 0;
bool bootHandled = false;

// ============================================================
// LICZNIKI DIAGNOSTYCZNE
// ============================================================
#if DIAG

uint32_t diagLastReport = 0;

uint32_t diagTcpSessions = 0;
uint32_t diagTcpOk = 0;
uint32_t diagTcpInvalid = 0;
uint32_t diagTcpTimeout = 0;
uint32_t diagTcpDisconnect = 0;
uint32_t diagTcpWriteError = 0;

uint32_t diagFrameHeaderTimeout = 0;
uint32_t diagFrameBodyTimeout = 0;
uint32_t diagFrameInvalidLength = 0;
uint32_t diagFrameAuthFailed = 0;
uint32_t diagFrameInvalidCommand = 0;
uint32_t diagFrameInvalidSequence = 0;
uint32_t diagFrameChallengeError = 0;

uint32_t diagWifiReconnects = 0;
uint32_t diagWifiDisconnects = 0;

uint32_t diagRelayOnCount = 0;
uint32_t diagRelayOffCount = 0;
uint32_t diagFailsafeCount = 0;

uint32_t diagLastValidCommand = 0;

size_t diagInitialHeap = 0;
size_t diagInitialLargest = 0;

uint32_t diagPreviousOk = 0;
uint32_t diagPreviousInvalid = 0;
uint32_t diagPreviousSessions = 0;

// ============================================================
// ROZPOZNAWANIE BLEDU RAMKI
// ============================================================
enum class FrameError : uint8_t {
    NONE,
    HEADER_TIMEOUT,
    BODY_TIMEOUT,
    INVALID_LENGTH,
    AES_AUTH_FAILED,
    CONNECTION_CLOSED
};

const char* frameErrorName(FrameError error) {
    switch (error) {
        case FrameError::NONE:
            return "NONE";

        case FrameError::HEADER_TIMEOUT:
            return "HEADER_TIMEOUT";

        case FrameError::BODY_TIMEOUT:
            return "BODY_TIMEOUT";

        case FrameError::INVALID_LENGTH:
            return "INVALID_LENGTH";

        case FrameError::AES_AUTH_FAILED:
            return "AES_AUTH_FAILED";

        case FrameError::CONNECTION_CLOSED:
            return "CONNECTION_CLOSED";

        default:
            return "UNKNOWN";
    }
}

// ============================================================
// ODCZYT DOKLADNEJ LICZBY BAJTOW
// ============================================================
bool diagReadExact(
    WiFiClient& client,
    uint8_t* destination,
    size_t expected,
    uint32_t timeoutMs,
    size_t& received
) {
    received = 0;
    uint32_t start = millis();

    while (received < expected) {
        int available = client.available();

        if (available > 0) {
            size_t count = min(
                size_t(available),
                expected - received
            );

            int result = client.read(
                destination + received,
                count
            );

            if (result > 0) {
                received += size_t(result);
                continue;
            }
        }

        if (!client.connected() &&
            client.available() <= 0) {
            return false;
        }

        if (millis() - start >= timeoutMs) {
            return false;
        }

        delay(1);
    }

    return true;
}

// ============================================================
// ODBIOR ZASZYFROWANEJ RAMKI Z DIAGNOSTYKA
// ============================================================
bool diagRecvFrame(
    WiFiClient& client,
    uint8_t context,
    uint8_t* plain,
    size_t& plainLen,
    uint32_t timeoutMs,
    FrameError& error
) {
    error = FrameError::NONE;
    plainLen = 0;

    uint8_t header[2] = {};
    size_t received = 0;

    if (!diagReadExact(
            client,
            header,
            sizeof(header),
            timeoutMs,
            received
        )) {

        error = client.connected()
            ? FrameError::HEADER_TIMEOUT
            : FrameError::CONNECTION_CLOSED;

        DLOG(
            "[FRAME] Header received=%u/2\n",
            unsigned(received)
        );

        return false;
    }

    size_t packetLen =
        (size_t(header[0]) << 8) |
        size_t(header[1]);

    if (packetLen < 29 ||
        packetLen > SecureLink::MAX_PACKET) {

        error = FrameError::INVALID_LENGTH;

        DLOG(
            "[FRAME] Invalid packet length=%u\n",
            unsigned(packetLen)
        );

        return false;
    }

    uint8_t packet[SecureLink::MAX_PACKET];

    if (!diagReadExact(
            client,
            packet,
            packetLen,
            timeoutMs,
            received
        )) {

        error = client.connected()
            ? FrameError::BODY_TIMEOUT
            : FrameError::CONNECTION_CLOSED;

        DLOG(
            "[FRAME] Body received=%u/%u\n",
            unsigned(received),
            unsigned(packetLen)
        );

        return false;
    }

    if (!SecureLink::open(
            context,
            packet,
            packetLen,
            plain,
            plainLen
        )) {
        error = FrameError::AES_AUTH_FAILED;
        return false;
    }

    return true;
}

// ============================================================
// ZAPIS PRZYCZYNY BLEDU
// ============================================================
void diagRecordFrameError(FrameError error) {
    ++diagTcpInvalid;

    switch (error) {
        case FrameError::HEADER_TIMEOUT:
            ++diagFrameHeaderTimeout;
            break;

        case FrameError::BODY_TIMEOUT:
            ++diagFrameBodyTimeout;
            break;

        case FrameError::INVALID_LENGTH:
            ++diagFrameInvalidLength;
            break;

        case FrameError::AES_AUTH_FAILED:
            ++diagFrameAuthFailed;
            break;

        case FrameError::CONNECTION_CLOSED:
            ++diagTcpDisconnect;
            break;

        default:
            break;
    }

    DLOG(
        "[TCP-ERROR] reason=%s uptime=%lu "
        "seq=%lu RSSI=%ld heap=%u\n",
        frameErrorName(error),
        (unsigned long)millis(),
        (unsigned long)lastSequence,
        (long)WiFi.RSSI(),
        unsigned(ESP.getFreeHeap())
    );
}

// ============================================================
// RAPORT DIAGNOSTYCZNY
// ============================================================
void diagnosticReport() {
    uint32_t now = millis();

    if (now - diagLastReport < DIAG_INTERVAL_MS) {
        return;
    }

    diagLastReport = now;

    size_t freeHeap =
        heap_caps_get_free_size(MALLOC_CAP_8BIT);

    size_t minHeap =
        heap_caps_get_minimum_free_size(
            MALLOC_CAP_8BIT
        );

    size_t largestBlock =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_8BIT
        );

    size_t totalHeap =
        heap_caps_get_total_size(MALLOC_CAP_8BIT);

    int32_t heapChange =
        int32_t(freeHeap) -
        int32_t(diagInitialHeap);

    int32_t largestChange =
        int32_t(largestBlock) -
        int32_t(diagInitialLargest);

    bool wifiConnected =
        WiFi.status() == WL_CONNECTED;

    bool tcpActive =
        bool(peer) && peer.connected();

    uint32_t sinceValid =
        validCommandReceived
            ? now - lastAccepted
            : 0;

    uint32_t okDelta =
        diagTcpOk - diagPreviousOk;

    uint32_t invalidDelta =
        diagTcpInvalid - diagPreviousInvalid;

    uint32_t sessionsDelta =
        diagTcpSessions - diagPreviousSessions;

    diagPreviousOk = diagTcpOk;
    diagPreviousInvalid = diagTcpInvalid;
    diagPreviousSessions = diagTcpSessions;

    Serial.println();
    Serial.println(
        "================ DIAG SLAVE ================"
    );

    Serial.printf(
        "[SYS] Uptime=%lu s ResetReason=%d\n",
        (unsigned long)(now / 1000),
        int(esp_reset_reason())
    );

    Serial.printf(
        "[RAM] Free=%u Min=%u Largest=%u "
        "Delta=%ld LargestDelta=%ld\n",
        unsigned(freeHeap),
        unsigned(minHeap),
        unsigned(largestBlock),
        (long)heapChange,
        (long)largestChange
    );

    Serial.printf(
        "[RAM] Allocated=%u Total=%u "
        "FragmentationApprox=%u%%\n",
        unsigned(totalHeap - freeHeap),
        unsigned(totalHeap),
        freeHeap > 0
            ? unsigned(
                100 -
                largestBlock * 100 / freeHeap
            )
            : 0
    );

    Serial.printf(
        "[WIFI] Connected=%d RSSI=%ld dBm "
        "Channel=%d Status=%d\n",
        wifiConnected ? 1 : 0,
        wifiConnected ? (long)WiFi.RSSI() : 0L,
        int(wifiConnected ? WiFi.channel() : -1),
        int(WiFi.status())
    );

    Serial.printf(
        "[WIFI] IP=%s Reconnects=%lu "
        "Disconnects=%lu\n",
        WiFi.localIP().toString().c_str(),
        (unsigned long)diagWifiReconnects,
        (unsigned long)diagWifiDisconnects
    );

    Serial.printf(
        "[TCP] Connected=%d SessionReady=%d "
        "Sessions=%lu Disconnects=%lu\n",
        tcpActive ? 1 : 0,
        sessionReady ? 1 : 0,
        (unsigned long)diagTcpSessions,
        (unsigned long)diagTcpDisconnect
    );

    Serial.printf(
        "[TCP] OK=%lu Invalid=%lu "
        "Timeout=%lu WriteError=%lu\n",
        (unsigned long)diagTcpOk,
        (unsigned long)diagTcpInvalid,
        (unsigned long)diagTcpTimeout,
        (unsigned long)diagTcpWriteError
    );

    Serial.printf(
        "[TCP-10s] OK=%lu Invalid=%lu "
        "NewSessions=%lu\n",
        (unsigned long)okDelta,
        (unsigned long)invalidDelta,
        (unsigned long)sessionsDelta
    );

    Serial.printf(
        "[FRAME] HeaderTimeout=%lu "
        "BodyTimeout=%lu InvalidLength=%lu\n",
        (unsigned long)diagFrameHeaderTimeout,
        (unsigned long)diagFrameBodyTimeout,
        (unsigned long)diagFrameInvalidLength
    );

    Serial.printf(
        "[FRAME] AESAuthFailed=%lu "
        "BadChallenge=%lu BadSequence=%lu "
        "BadCommand=%lu\n",
        (unsigned long)diagFrameAuthFailed,
        (unsigned long)diagFrameChallengeError,
        (unsigned long)diagFrameInvalidSequence,
        (unsigned long)diagFrameInvalidCommand
    );

    Serial.printf(
        "[RELAY] State=%s OnCount=%lu "
        "OffCount=%lu FailsafeCount=%lu\n",
        relayState ? "ON" : "OFF",
        (unsigned long)diagRelayOnCount,
        (unsigned long)diagRelayOffCount,
        (unsigned long)diagFailsafeCount
    );

    if (validCommandReceived) {
        Serial.printf(
            "[SAFETY] LastValid=%lu ms "
            "Timeout=%lu ms Remaining=%lu ms "
            "Triggered=%d\n",
            (unsigned long)sinceValid,
            (unsigned long)(uint32_t(failureTimeoutMin) * 60000UL),
            (unsigned long)(
                sinceValid < (uint32_t(failureTimeoutMin) * 60000UL)
                    ? (uint32_t(failureTimeoutMin) * 60000UL) - sinceValid
                    : 0
            ),
            failsafeTriggered ? 1 : 0
        );
    } else {
        Serial.println(
            "[SAFETY] No valid command since boot"
        );
    }

    Serial.printf(
        "[BLE] Enabled=%d ProvisionMode=%d "
        "ProvisionSuccess=%d\n",
        bleEnabled ? 1 : 0,
        provisionMode ? 1 : 0,
        provisionSuccess ? 1 : 0
    );

    const bool advertisingActive = bleEnabled &&
        NimBLEDevice::getAdvertising()->isAdvertising();
    Serial.printf(
        "[BLE] Advertising=%d PeerConnected=%d PeerDisconnected=%d Handle=%u\n",
        advertisingActive ? 1 : 0, blePeerConnected ? 1 : 0,
        blePeerDisconnected ? 1 : 0, unsigned(bleConnHandle)
    );

    Serial.println(
        "============================================"
    );
    Serial.println();
}

#else

inline void diagnosticReport() {}

#endif

// ============================================================
// STEROWANIE PRZEKAZNIKIEM
// ============================================================
void relayOff() {
#if DIAG
    if (relayState != 0) {
        ++diagRelayOffCount;

        DLOG(
            "[RELAY] ON -> OFF uptime=%lu ms\n",
            (unsigned long)millis()
        );
    }
#endif

    digitalWrite(RELAY_PIN, relayLevel(false));
    relayState = 0;
}

void setRelay(int state) {
    state = state ? 1 : 0;

    if (relayState == state) {
        return;
    }

#if DIAG
    if (state) {
        ++diagRelayOnCount;
    } else {
        ++diagRelayOffCount;
    }

    DLOG(
        "[RELAY] %s -> %s uptime=%lu ms\n",
        relayState ? "ON" : "OFF",
        state ? "ON" : "OFF",
        (unsigned long)millis()
    );
#endif

    ledPhase = 1;
    nextLedPhase = millis() + 130;

    digitalWrite(
        RELAY_PIN,
        relayLevel(state != 0)
    );

    relayState = state;
}

// ============================================================
// KONFIGUROWALNY FAILSAFE, BEZ PRZELACZANIA PODCZAS KRÓTKICH ZERWAN
// Po restarcie bez odebranej komendy: ON/OFF po timeout; ostatni stan=OFF.
// ============================================================
void relaySafetyStep() {
    // Tylko fabryczny tryb parowania nie wymusza grzania.
    // Automatyczny fallback BLE po utracie Wi-Fi NIE blokuje failsafe:
    // przekaznik nadal reaguje po skonfigurowanym czasie.
    if (provisionMode && !autoBLEFallback) {
        bootSafetyStart = millis();
        return;
    }
    const uint32_t elapsed = millis() -
        (validCommandReceived ? lastAccepted : bootSafetyStart);
    if (elapsed < uint32_t(failureTimeoutMin) * 60000UL || failsafeTriggered)
        return;
    failsafeTriggered = true;
#if DIAG
    ++diagFailsafeCount;
    DLOG("[FAILSAFE] elapsed=%lu mode=%u relayBefore=%d\n",
         (unsigned long)elapsed, unsigned(failureMode), relayState);
#endif
    if (failureMode == 1) setRelay(1);
    else if (failureMode == 0) setRelay(0);
    // OSTATNI STAN: bez zmian, po restarcie startujemy z OFF.
}

// Master wysyla konfiguracje w osobnej komendzie F (24 bajty), ACK jak
// dotychczas 26 bajtow. Nie zmieniamy formatu standardowych komend 0/1.
// Zapis NVS dopiero po poprawnej autoryzacji ramki TCP.
bool applyRemoteConfig(uint8_t mode, uint8_t minutes, uint8_t activeLow) {
    if (mode > 2 || minutes < 1 || minutes > 30 || activeLow > 1) return false;
    const bool newLow = activeLow != 0;
    if (mode == failureMode && minutes == failureTimeoutMin && newLow == relayActiveLow)
        return true;
    Preferences configPrefs;
    if (!configPrefs.begin("settings", false)) return false;
    bool ok = configPrefs.putUChar("failMode", mode) == 1;
    ok = (configPrefs.putUChar("failTime", minutes) == 1) && ok;
    ok = (configPrefs.putBool("relayLow", newLow) == 1) && ok;
    configPrefs.end();
    if (!ok) return false;
    failureMode = mode;
    failureTimeoutMin = minutes;
    relayActiveLow = newLow;
    // Utrzymaj LOGICZNY stan zapotrzebowania; dostosuj GPIO do polaryzacji.
    digitalWrite(RELAY_PIN, relayLevel(relayState != 0));
    DLOG("[CONFIG] mode=%u timeout=%u min activeLow=%u\n",
         unsigned(mode), unsigned(minutes), unsigned(activeLow));
    return true;
}

// ============================================================
// RESET FABRYCZNY
// ============================================================
void factoryErase() {
    relayOff();
    peer.stop();

    DPRINTLN("[RESET] Factory reset requested");

    nvs_flash_erase();

    while (digitalRead(RESET_BTN) == LOW) {
        delay(20);
    }

    delay(150);
    ESP.restart();
}

// ============================================================
// BLE - CALLBACK POLACZENIA
// ============================================================
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(
        NimBLEServer* server,
        NimBLEConnInfo& info
    ) override {
        bleConnHandle = info.getConnHandle();
        blePeerConnected = true;
        blePeerDisconnected = false;

        DLOG("[BLE] Master connected uptime=%lu ms handle=%u\n",
             (unsigned long)millis(), unsigned(info.getConnHandle()));
    }

    void onDisconnect(
        NimBLEServer* server,
        NimBLEConnInfo& info,
        int reason
    ) override {
        blePeerConnected = false;
        blePeerDisconnected = true;

        DLOG(
            "[BLE] Master disconnected reason=%d (0x%X) uptime=%lu ms handle=%u\n",
            reason, static_cast<unsigned int>(reason),
            (unsigned long)millis(), unsigned(info.getConnHandle())
        );
    }
};

// ============================================================
// BLE - ODBIOR DANYCH
// ============================================================
class CredentialsCallbacks :
    public NimBLECharacteristicCallbacks {

    void onWrite(
        NimBLECharacteristic* chr,
        NimBLEConnInfo& info
    ) override {
        auto value = chr->getValue();
        DLOG("[BLE] Write received bytes=%u handle=%u uptime=%lu ms\n",
             unsigned(value.size()), unsigned(info.getConnHandle()),
             (unsigned long)millis());

        if (value.size() < 29) {
            DPRINTLN("[BLE] Packet too short");
            return;
        }

        const uint8_t* packet =
            reinterpret_cast<const uint8_t*>(
                value.data()
            );

        // Uwierzytelnienie Mastera.
        {
            uint8_t plain[SecureLink::MAX_PLAIN];
            size_t len = 0;

            if (SecureLink::open(
                    'H',
                    packet,
                    value.size(),
                    plain,
                    len
                ) &&
                len == 17 &&
                plain[16] == 0xA5 &&
                memcmp(
                    plain,
                    bleChallenge,
                    16
                ) == 0) {

                uint8_t answer[17];

                memcpy(
                    answer,
                    bleChallenge,
                    16
                );

                answer[16] = 0x5A;

                uint8_t encrypted[
                    SecureLink::MAX_PACKET
                ];

                size_t encryptedLen = 0;

                DPRINTLN("[BLE] H verified: length, marker and challenge OK");
                DPRINTLN("[BLE] J encryption attempt");
                if (SecureLink::seal(
                        'J',
                        answer,
                        sizeof(answer),
                        encrypted,
                        encryptedLen
                    )) {
                    bleAckChr->setValue(
                        encrypted,
                        encryptedLen
                    );
                    DLOG("[BLE] J encrypted and published bytes=%u\n",
                         unsigned(encryptedLen));
                    DPRINTLN("[BLE] Authentication OK");
                } else {
                    DPRINTLN("[BLE] ERROR: J encryption failed; response not published");
                }

                return;
            }
        }

        if (provisionSuccess) {
            return;
        }

        uint8_t plain[SecureLink::MAX_PLAIN];
        size_t len = 0;

        if (!SecureLink::open(
                'P',
                packet,
                value.size(),
                plain,
                len
            )) {
            DPRINTLN(
                "[BLE] AES authentication failed"
            );
            return;
        }

        if (len < 18 ||
            memcmp(
                plain,
                bleChallenge,
                16
            ) != 0) {
            DPRINTLN(
                "[BLE] Invalid challenge"
            );
            return;
        }

        uint8_t ssidLen = plain[16];

        if (ssidLen < 1 || ssidLen > 32) {
            DPRINTLN(
                "[BLE] Invalid SSID length"
            );
            return;
        }

        if (size_t(18 + ssidLen) > len) {
            DPRINTLN(
                "[BLE] Invalid payload length"
            );
            return;
        }

        uint8_t passLen =
            plain[17 + ssidLen];

        if (passLen > 64 ||
            size_t(18 + ssidLen + passLen) != len) {
            DPRINTLN(
                "[BLE] Invalid password length"
            );
            return;
        }

        ProvisionCreds credentials = {};

        memcpy(
            credentials.ssid,
            plain + 17,
            ssidLen
        );

        memcpy(
            credentials.pass,
            plain + 18 + ssidLen,
            passLen
        );

        if (credentialsQueue) {
            xQueueSend(
                credentialsQueue,
                &credentials,
                0
            );
        }

        DPRINTLN(
            "[BLE] WiFi credentials received"
        );
    }
};

static ServerCallbacks serverCallbacks;
static CredentialsCallbacks credentialsCallbacks;

// ============================================================
// BLE - START
// ============================================================
void startBLE() {
    if (bleEnabled) {
        return;
    }

    SecureLink::randomBytes(
        bleChallenge,
        sizeof(bleChallenge)
    );

    NimBLEDevice::init(
        "Termostat-Slave-Setup"
    );

    NimBLEDevice::setMTU(247);

    bleServer =
        NimBLEDevice::createServer();

    bleServer->setCallbacks(
        &serverCallbacks
    );

    NimBLEService* service =
        bleServer->createService(
            SecureLink::SERVICE_UUID
        );

    bleChallengeChr =
        service->createCharacteristic(
            SecureLink::CHALLENGE_UUID,
            NIMBLE_PROPERTY::READ
        );

    bleChallengeChr->setValue(
        bleChallenge,
        sizeof(bleChallenge)
    );

    NimBLECharacteristic* credentialsChr =
        service->createCharacteristic(
            SecureLink::CREDENTIALS_UUID,
            NIMBLE_PROPERTY::WRITE
        );

    credentialsChr->setCallbacks(
        &credentialsCallbacks
    );

    bleAckChr =
        service->createCharacteristic(
            SecureLink::STATUS_UUID,
            NIMBLE_PROPERTY::READ
        );

    bleAckChr->setValue("WAIT");


    NimBLEAdvertising* advertising =
        NimBLEDevice::getAdvertising();

    advertising->addServiceUUID(
        SecureLink::SERVICE_UUID
    );

    advertising->setName(
        "Termostat-Slave-Setup"
    );

    advertising->enableScanResponse(true);
#if DIAG
    DLOG("[BLE] Advertising start attempt uptime=%lu ms\n",
         (unsigned long)millis());
    const bool advertisingStarted = advertising->start();
    DLOG("[BLE] Advertising start result=%d active=%d\n",
         advertisingStarted ? 1 : 0,
         advertising->isAdvertising() ? 1 : 0);
#else
    advertising->start();
#endif

    blePeerConnected = false;
    blePeerDisconnected = false;
    bleEnabled = true;
    bleReconfigureIdleSince = millis();

    DPRINTLN("[BLE] Provisioning initialized; see advertising start result");
}

// ============================================================
// BLE - STOP
// ============================================================
void stopBLE() {
    if (!bleEnabled || blePeerConnected) {
        return;
    }

    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(true);

    bleServer = nullptr;
    bleChallengeChr = nullptr;
    bleAckChr = nullptr;

    bleEnabled = false;
    autoBLEFallback = false;

    DPRINTLN("[BLE] Deinitialized");
}

// ============================================================
// BLE - ACK PROVISIONING
// ============================================================
void publishProvisionAck(bool success) {
    if (!bleEnabled || !bleAckChr) {
        return;
    }

    uint8_t plain[21] = {};

    memcpy(
        plain,
        bleChallenge,
        16
    );

    plain[16] = success ? 1 : 0;

    IPAddress ip = WiFi.localIP();

    for (int i = 0; i < 4; ++i) {
        plain[17 + i] = success ? ip[i] : 0;
    }

    uint8_t packet[SecureLink::MAX_PACKET];
    size_t packetLen = 0;

    if (SecureLink::seal(
            'K',
            plain,
            sizeof(plain),
            packet,
            packetLen
        )) {
        bleAckChr->setValue(
            packet,
            packetLen
        );
    }

    DLOG(
        "[BLE] Provision ACK=%d\n",
        success ? 1 : 0
    );
}

// ============================================================
// WIFI
// ============================================================
void startWiFi(
    const String& ssid,
    const String& password
) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("Piec-Termostat-Slave");
    WiFi.setAutoReconnect(true);

    // Wybierz najsilniejszy punkt Mesh o zapisanym SSID.
    int count = WiFi.scanNetworks(false, false);
    bool found = false;
    int bestRSSI = -127;
    int bestChannel = 0;
    uint8_t bestBSSID[6] = {};

    for (int i = 0; i < count; ++i) {
        if (WiFi.SSID(i) != ssid) continue;

        int rssi = WiFi.RSSI(i);
        const uint8_t* bssid = WiFi.BSSID(i);
        if (bssid && (!found || rssi > bestRSSI)) {
            found = true;
            bestRSSI = rssi;
            bestChannel = WiFi.channel(i);
            memcpy(bestBSSID, bssid, sizeof(bestBSSID));
        }
    }

#if DIAG
    if (found) {
        DLOG(
            "[MESH] Selected BSSID=%02X:%02X:%02X:%02X:%02X:%02X "
            "Channel=%d RSSI=%d dBm\n",
            bestBSSID[0], bestBSSID[1], bestBSSID[2],
            bestBSSID[3], bestBSSID[4], bestBSSID[5],
            bestChannel, bestRSSI
        );
    } else {
        DLOG("[MESH] SSID '%s' not found; automatic connection\n", ssid.c_str());
    }
#endif

    WiFi.scanDelete();

    if (found) {
        WiFi.begin(ssid.c_str(), password.c_str(), bestChannel, bestBSSID);
    } else {
        WiFi.begin(ssid.c_str(), password.c_str());
    }

    lastWiFiTry = millis();

#if DIAG
    ++diagWifiReconnects;
    DLOG(
        "[WIFI] Connection attempt=%lu\n",
        (unsigned long)diagWifiReconnects
    );
#endif
}

// ============================================================
// ZAMKNIECIE TCP
// NIE ZMIENIA STANU PRZEKAZNIKA
// ============================================================
void stopTCP() {
    peer.stop();

    sessionReady = false;
    lastSequence = 0;

#if DIAG
    DLOG(
        "[TCP] Session closed. "
        "Relay=%s LastValidAge=%lu ms\n",
        relayState ? "ON" : "OFF",
        validCommandReceived
            ? (unsigned long)(
                millis() - lastAccepted
            )
            : 0UL
    );
#endif
}

// ============================================================
// TCP SERVER
// ============================================================
void setupTCP() {
    server.begin();
    server.setNoDelay(true);

    if (MDNS.begin("termostat-slave")) {
        MDNS.addService(
            "termostat-co",
            "tcp",
            SecureLink::PORT
        );
    }

    DLOG(
        "[TCP] Server started IP=%s Port=%u\n",
        WiFi.localIP().toString().c_str(),
        SecureLink::PORT
    );
}

// ============================================================
// BLE PROVISIONING
// ============================================================
// Wywolywane z loop(), nigdy z callbacku stosu BLE.
// Zapisane SSID/haslo pozostaja bez zmian; usuwamy tylko tryb konfiguracji.
bool resumeSavedWiFiAfterCancel(const char* reason) {
    if (!bleEnabled || blePeerConnected || provisionSuccess ||
        !provisionMode || autoBLEFallback || wifiSSID.isEmpty() ||
        (credentialsQueue && uxQueueMessagesWaiting(credentialsQueue) != 0)) {
        return false;
    }

    if (!prefs.begin("settings", false)) {
        DPRINTLN("[BLE] Cannot clear provisioning flag: NVS open failed");
        return false;
    }
    const bool saved = prefs.putBool("prov", false) == 1;
    prefs.end();
    if (!saved) {
        DPRINTLN("[BLE] Cannot clear provisioning flag: NVS write failed");
        return false;
    }

    DLOG("[BLE] Reconfiguration ended: %s; restoring saved WiFi/TCP\n", reason);
    blePeerDisconnected = false;
    stopBLE();
    provisionMode = false;
    wifiLostSince = millis();
    startWiFi(wifiSSID, wifiPASS);
    return true;
}

void handleProvisioning() {
    ProvisionCreds credentials = {};

    if (credentialsQueue &&
        xQueueReceive(
            credentialsQueue,
            &credentials,
            0
        ) == pdTRUE) {

        provisionSuccess = false;

        relayOff();

        validCommandReceived = false;
        failsafeTriggered = false;
        lastAccepted = 0;
        bootSafetyStart = millis();

        WiFi.disconnect(false, false);

        startWiFi(
            credentials.ssid,
            credentials.pass
        );

        uint32_t start = millis();

        while (
            WiFi.status() != WL_CONNECTED &&
            millis() - start < 20000
        ) {
            delay(100);
        }

        bool success =
            WiFi.status() == WL_CONNECTED;

        if (success) {
            prefs.begin("settings", false);

            prefs.putString(
                "ssid",
                credentials.ssid
            );

            prefs.putString(
                "pass",
                credentials.pass
            );

            prefs.putBool("prov", false);

            prefs.end();

            wifiSSID = credentials.ssid;
            wifiPASS = credentials.pass;

            provisionSuccess = true;
            provisionSuccessAt = millis();
            provisionMode = false;

            DPRINTLN(
                "[WIFI] Provision successful"
            );

        } else {
            DPRINTLN("[WIFI] Provision failed");

            if (!wifiSSID.isEmpty()) {
                startWiFi(wifiSSID, wifiPASS);
            }
        }

        publishProvisionAck(success);
    }

    // Kontakt BLE przerwany przed udana zmiana sieci: wroc do starej.
    // Fabryczny provisioning i automatyczny fallback zachowuja stare dzialanie.
    if (bleEnabled && !provisionSuccess && provisionMode &&
        !autoBLEFallback && !wifiSSID.isEmpty()) {
        if (blePeerConnected) {
            bleReconfigureIdleSince = millis();
        } else if (blePeerDisconnected) {
            if (resumeSavedWiFiAfterCancel("BLE disconnected before success")) return;
        } else if (millis() - bleReconfigureIdleSince >= BLE_RECONFIG_IDLE_MS) {
            if (resumeSavedWiFiAfterCancel("no BLE peer for 30 seconds")) return;
        }
    }

    if (bleEnabled &&
        blePeerDisconnected &&
        !provisionSuccess) {

        blePeerDisconnected = false;

#if DIAG
        NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
        DLOG("[BLE] Advertising restart after disconnect uptime=%lu ms\n",
             (unsigned long)millis());
        const bool advertisingStarted = advertising->start();
        DLOG("[BLE] Advertising restart result=%d active=%d\n",
             advertisingStarted ? 1 : 0,
             advertising->isAdvertising() ? 1 : 0);
#else
        NimBLEDevice::getAdvertising()->start();
#endif
    }

    if (bleEnabled &&
        provisionSuccess &&
        (!blePeerConnected ||
         millis() - provisionSuccessAt >=
             BLE_SUCCESS_GRACE_MS)) {

        if (blePeerConnected) {
            bleServer->disconnect(bleConnHandle);
            return;
        }

        stopBLE();
    }
}

// ============================================================
// TCP - ODBIOR KOMEND
// ============================================================
void tcpStep() {
    if (
        WiFi.status() != WL_CONNECTED ||
        provisionMode ||
        bleEnabled
    ) {
        if (peer) {
            stopTCP();
        }
        return;
    }

    // --------------------------------------------------------
    // NOWA SESJA - BEZ PRZELACZANIA PRZEKAZNIKA
    // --------------------------------------------------------
    if (!peer || !peer.connected()) {
        if (peer) {
#if DIAG
            ++diagTcpDisconnect;
            DPRINTLN(
                "[TCP] Previous client disconnected"
            );
#endif
            stopTCP();
        }

        WiFiClient incoming = server.accept();

        if (incoming) {
            peer = incoming;
            peer.setNoDelay(true);

#if DIAG
            ++diagTcpSessions;

            DLOG(
                "[TCP] New session=%lu "
                "remote=%s:%u heap=%u RSSI=%ld "
                "relay=%s\n",
                (unsigned long)diagTcpSessions,
                peer.remoteIP().toString().c_str(),
                unsigned(peer.remotePort()),
                unsigned(ESP.getFreeHeap()),
                (long)WiFi.RSSI(),
                relayState ? "ON" : "OFF"
            );
#endif

            SecureLink::randomBytes(
                tcpChallenge,
                sizeof(tcpChallenge)
            );

            uint8_t greeting[20] = {
                'E', 'T', '2', '!'
            };

            memcpy(
                greeting + 4,
                tcpChallenge,
                16
            );

            if (peer.write(
                    greeting,
                    sizeof(greeting)
                ) != sizeof(greeting)) {

#if DIAG
                ++diagTcpWriteError;
                DPRINTLN(
                    "[TCP-ERROR] Greeting write failed"
                );
#endif
                stopTCP();
                return;
            }

            sessionReady = true;
            lastTCP = millis();
            lastSequence = 0;

            DPRINTLN(
                "[TCP] Client connected - relay unchanged"
            );
        }

        return;
    }

    // --------------------------------------------------------
    // TIMEOUT SESJI TCP - PRZEKAZNIK BEZ ZMIAN
    // --------------------------------------------------------
    if (millis() - lastTCP > TCP_IDLE_MS) {

#if DIAG
        ++diagTcpTimeout;

        DLOG(
            "[TCP-ERROR] IDLE_TIMEOUT "
            "idle=%lu ms lastSeq=%lu "
            "RSSI=%ld heap=%u relay=%s\n",
            (unsigned long)(millis() - lastTCP),
            (unsigned long)lastSequence,
            (long)WiFi.RSSI(),
            unsigned(ESP.getFreeHeap()),
            relayState ? "ON" : "OFF"
        );
#endif

        stopTCP();
        return;
    }

    if (peer.available() < 2) {
        return;
    }

    uint8_t plain[SecureLink::MAX_PLAIN];
    size_t len = 0;

#if DIAG
    FrameError frameError = FrameError::NONE;

    bool validFrame = diagRecvFrame(
        peer,
        'C',
        plain,
        len,
        1200,
        frameError
    );

    if (!validFrame) {
        diagRecordFrameError(frameError);
        stopTCP();
        return;
    }
#else
    bool validFrame = SecureLink::recvFrame(
        peer,
        'C',
        plain,
        len,
        1200
    );

    if (!validFrame) {
        stopTCP();
        return;
    }
#endif

    if (len != 21 && len != 24) {

#if DIAG
        ++diagTcpInvalid;
        ++diagFrameInvalidLength;

        DLOG(
            "[TCP-ERROR] INVALID_PLAIN_LENGTH=%u\n",
            unsigned(len)
        );
#endif

        stopTCP();
        return;
    }

    if (memcmp(
            plain,
            tcpChallenge,
            16
        ) != 0) {

#if DIAG
        ++diagTcpInvalid;
        ++diagFrameChallengeError;

        DPRINTLN(
            "[TCP-ERROR] BAD_CHALLENGE"
        );
#endif

        stopTCP();
        return;
    }

    uint32_t sequence =
        SecureLink::getU32(plain + 16);

    uint8_t command = plain[20];

    if (sequence <= lastSequence) {

#if DIAG
        ++diagTcpInvalid;
        ++diagFrameInvalidSequence;

        DLOG(
            "[TCP-ERROR] BAD_SEQUENCE "
            "received=%lu previous=%lu\n",
            (unsigned long)sequence,
            (unsigned long)lastSequence
        );
#endif

        stopTCP();
        return;
    }

    if (
        command != '0' &&
        command != '1' &&
        command != 'R' &&
        command != 'F'
    ) {

#if DIAG
        ++diagTcpInvalid;
        ++diagFrameInvalidCommand;

        DLOG(
            "[TCP-ERROR] BAD_COMMAND=0x%02X\n",
            unsigned(command)
        );
#endif

        stopTCP();
        return;
    }

    // Walidacja struktury komendy przed jej zaakceptowaniem.
    if ((command == 'F' && len != 24) || (command != 'F' && len != 21) ||
        (command == 'F' && (plain[21] > 2 || plain[22] < 1 ||
                            plain[22] > 30 || plain[23] > 1))) {
        stopTCP();
        return;
    }
    if (command == 'F' && !applyRemoteConfig(plain[21], plain[22], plain[23])) {
        stopTCP();
        return;
    }
    lastSequence = sequence;
    lastTCP = millis();

#if DIAG
    ++diagTcpOk;
    diagLastValidCommand = millis();
#endif

    // --------------------------------------------------------
    // WYKONANIE POPRAWNEJ KOMENDY
    // --------------------------------------------------------
    if (command == 'R') {
        relayOff();

        validCommandReceived = false;
        lastAccepted = 0;
        failsafeTriggered = false;

    } else if (command == 'F') {
        // Konfiguracja nie jest heartbeat grzania.
        // Wymagane jest osobne polecenie 0/1.
    } else {
        lastAccepted = millis();

        validCommandReceived = true;
        failsafeTriggered = false;

        setRelay(command == '1');
    }

    // --------------------------------------------------------
    // ACK: 26 BAJTOW
    // BAJT 25 = RSSI SLAVE W dBm
    // --------------------------------------------------------
    uint8_t ack[26] = {};

    memcpy(
        ack,
        tcpChallenge,
        16
    );

    SecureLink::putU32(
        ack + 16,
        sequence
    );

    ack[20] = command;

    IPAddress ip = WiFi.localIP();

    for (int i = 0; i < 4; ++i) {
        ack[21 + i] = ip[i];
    }

    // JEDYNA ZMIANA W PROTOKOLE:
    // Przeslanie RSSI zmierzonego przez Slave.
    // int8_t zachowuje wartosci ujemne.
    ack[25] = static_cast<uint8_t>(
        static_cast<int8_t>(WiFi.RSSI())
    );

    if (!SecureLink::sendFrame(
            peer,
            'A',
            ack,
            sizeof(ack)
        )) {

#if DIAG
        ++diagTcpWriteError;

        DLOG(
            "[TCP-ERROR] ACK_WRITE_FAILED "
            "seq=%lu heap=%u\n",
            (unsigned long)sequence,
            unsigned(ESP.getFreeHeap())
        );
#endif

        stopTCP();
        return;
    }

    // --------------------------------------------------------
    // PONOWNA KONFIGURACJA WIFI
    // --------------------------------------------------------
    if (command == 'R') {
        DPRINTLN(
            "[TCP] Reprovisioning requested"
        );

        prefs.begin("settings", false);
        prefs.putBool("prov", true);
        prefs.end();

        relayOff();

        delay(100);
        ESP.restart();
    }
}

// ============================================================
// LED
// ============================================================
void updateLED() {
    uint32_t now = millis();

    bool normalConnected =
        WiFi.status() == WL_CONNECTED &&
        !provisionMode &&
        !bleEnabled &&
        sessionReady &&
        peer.connected();

    if (normalConnected) {
        if (ledPhase > 0) {
            if (ledPhase == 1 ||
                ledPhase == 3) {
                digitalWrite(LED_PIN, HIGH);
            } else {
                digitalWrite(LED_PIN, LOW);
            }

            if ((int32_t)(
                    now - nextLedPhase
                ) >= 0) {

                ++ledPhase;
                nextLedPhase = now + 130;

                if (ledPhase > 4) {
                    ledPhase = 0;
                }
            }

            return;
        }

        digitalWrite(LED_PIN, LOW);
        return;
    }

    ledPhase = 0;

    if (now - lastLedToggle >= 350) {
        lastLedToggle = now;
        blink = !blink;

        digitalWrite(
            LED_PIN,
            blink ? LOW : HIGH
        );
    }
}

// ============================================================
// BOOT GPIO9
// ============================================================
void buttonStep() {
    bool pressed =
        digitalRead(RESET_BTN) == LOW;

    if (!pressed) {
        bootPressedAt = 0;
        bootHandled = false;
        return;
    }

    if (!bootPressedAt) {
        bootPressedAt = millis();
    }

    if (!bootHandled &&
        millis() - bootPressedAt >
            BOOT_HOLD_MS) {

        bootHandled = true;
        factoryErase();
    }
}

// ============================================================
// WIFI EVENTS - DIAGNOSTYKA
// ============================================================
#if DIAG
void diagWiFiEvent(
    WiFiEvent_t event,
    WiFiEventInfo_t info
) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        ++diagWifiDisconnects;

        Serial.printf(
            "[WIFI-EVENT] DISCONNECTED reason=%u "
            "total=%lu uptime=%lu ms\n",
            unsigned(
                info.wifi_sta_disconnected.reason
            ),
            (unsigned long)diagWifiDisconnects,
            (unsigned long)millis()
        );
    }

    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.printf(
            "[WIFI-EVENT] GOT_IP=%s RSSI=%ld\n",
            WiFi.localIP().toString().c_str(),
            (long)WiFi.RSSI()
        );
    }
}
#endif

// ============================================================
// SETUP
// ============================================================
void setup() {
    // Najpierw odczyt konfiguracji, potem inicjalizacja GPIO.
    // Nie rekonstruujemy z NVS ostatniego stanu grzania.
    Preferences relayPrefs;
    if (relayPrefs.begin("settings", true)) {
        failureMode = relayPrefs.getUChar("failMode", 2);
        failureTimeoutMin = relayPrefs.getUChar("failTime", 10);
        relayActiveLow = relayPrefs.getBool("relayLow", false);
        relayPrefs.end();
    }
    if (failureMode > 2) failureMode = 2;
    if (failureTimeoutMin < 1 || failureTimeoutMin > 30) failureTimeoutMin = 10;
    bootSafetyStart = millis();
    digitalWrite(RELAY_PIN, relayLevel(false));
    pinMode(RELAY_PIN, OUTPUT);
    relayOff();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    pinMode(RESET_BTN, INPUT_PULLUP);

    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);

    WiFi.persistent(false);

    validCommandReceived = false;
    failsafeTriggered = false;
    lastAccepted = 0;

#if DIAG
    Serial.println();

    Serial.println(
        "========== SLAVE DIAG ENABLED =========="
    );

    Serial.printf(
        "[BOOT] ESP32-C3 ResetReason=%d "
        "CPU=%u MHz Flash=%u\n",
        int(esp_reset_reason()),
        unsigned(ESP.getCpuFreqMHz()),
        unsigned(ESP.getFlashChipSize())
    );

    diagInitialHeap =
        heap_caps_get_free_size(
            MALLOC_CAP_8BIT
        );

    diagInitialLargest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_8BIT
        );

    WiFi.onEvent(diagWiFiEvent);
#endif

    credentialsQueue = xQueueCreate(
        1,
        sizeof(ProvisionCreds)
    );

    if (!credentialsQueue) {
        while (true) {
            relayOff();
            delay(1000);
        }
    }

    prefs.begin("settings", true);

    wifiSSID =
        prefs.getString("ssid", "");

    wifiPASS =
        prefs.getString("pass", "");

    provisionMode =
        prefs.getBool("prov", false) ||
        wifiSSID.isEmpty();

    prefs.end();

    if (provisionMode) {
        relayOff();
        startBLE();
    } else {
        wifiLostSince = millis();
        startWiFi(wifiSSID, wifiPASS);
    }

#if DIAG
    DLOG(
        "[BOOT] ProvisionMode=%d "
        "Heap=%u Largest=%u "
        "Failsafe=%lu ms mode=%u activeLow=%d\n",
        provisionMode ? 1 : 0,
        unsigned(ESP.getFreeHeap()),
        unsigned(
            heap_caps_get_largest_free_block(
                MALLOC_CAP_8BIT
            )
        ),
        (unsigned long)(uint32_t(failureTimeoutMin) * 60000UL),
        unsigned(failureMode), int(relayActiveLow)
    );
#endif
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    buttonStep();

    // Zabezpieczenie niezalezne od sesji TCP.
    relaySafetyStep();

    handleProvisioning();

    // Wi-Fi / BLE fallback dla zmiany lokalizacji.
    // Nigdy nie kasuj hasla ani ustawien przekaźnika w NVS.
    if (!provisionMode && !provisionSuccess) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiLostSince = 0;
            // Stara siec wrocila. Jesli nikt nie konfiguruje przez
            // BLE, zwolnij RAM/radio i wznow normalne TCP.
            if (autoBLEFallback && bleEnabled && !blePeerConnected) {
                DPRINTLN("[BLE] Old WiFi recovered - stop fallback BLE");
                stopBLE();
            }
        } else {
            if (wifiLostSince == 0) wifiLostSince = millis();
            if (peer) stopTCP();

            if (!bleEnabled &&
                millis() - lastWiFiTry >= WIFI_RETRY_MS) {
                // Zwykly reconnect Mesh, dopoki BLE nie dziala.
                startWiFi(wifiSSID, wifiPASS);
            }

            if (!bleEnabled &&
                millis() - wifiLostSince >= AUTO_BLE_AFTER_MS) {
                // Nowa lokalizacja: mozna przejsc do konfiguracji
                // z Mastera bez przycisku BOOT i bez factory reset.
                autoBLEFallback = true;
                startBLE();
                DPRINTLN("[BLE] AUTO FALLBACK after WiFi loss");
            }

            if (autoBLEFallback && bleEnabled &&
                !blePeerConnected &&
                millis() - lastWiFiTry >= AUTO_BLE_WIFI_RETRY_MS) {
                // Powrot sieci po chwilowej awarii: bez kosztownego
                // skanowania WiFi, ktore utrudnia discovery BLE.
                WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
                lastWiFiTry = millis();
                DPRINTLN("[WIFI] Fallback reconnect attempt");
            }
        }
    }

    // Stop TCP po utracie WiFi
    if (
        WiFi.status() != WL_CONNECTED &&
        serverStarted
    ) {
        server.stop();
        MDNS.end();

        serverStarted = false;
        stopTCP();
    }

    // Start serwera TCP
    if (
        !provisionMode &&
        !bleEnabled &&
        WiFi.status() == WL_CONNECTED &&
        !serverStarted
    ) {
        setupTCP();
        serverStarted = true;
    }

    // Obsluga TCP
    if (
        !provisionMode &&
        !bleEnabled &&
        WiFi.status() == WL_CONNECTED &&
        serverStarted
    ) {
        tcpStep();
    }

    // Powtorna kontrola po odbiorze TCP.
    relaySafetyStep();

    updateLED();

    diagnosticReport();

    delay(10);
}
