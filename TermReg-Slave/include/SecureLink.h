
#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <esp_system.h>
#include <mbedtls/gcm.h>
#include <cstring>
#include <algorithm>

namespace SecureLink {

static constexpr uint16_t PORT = 42317;
static constexpr size_t MAX_PLAIN = 160;
static constexpr size_t MAX_PACKET = MAX_PLAIN + 28;
static constexpr size_t MAX_FRAME = MAX_PACKET + 2;

static const char* SERVICE_UUID =
    "1ee71a50-8dda-451b-9476-f46398734710";

static const char* CHALLENGE_UUID =
    "1ee71a51-8dda-451b-9476-f46398734710";

static const char* CREDENTIALS_UUID =
    "1ee71a52-8dda-451b-9476-f46398734710";

static const char* STATUS_UUID =
    "1ee71a53-8dda-451b-9476-f46398734710";

// Identyczny klucz na Masterze i Slave.
// Zachowany z poprzedniej wersji.
static const uint8_t KEY[32] = {
// tu wprowadz swoj klucz
};

inline void randomBytes(
    uint8_t* dst,
    size_t len
) {
    esp_fill_random(dst, len);
}

inline void putU32(
    uint8_t* p,
    uint32_t value
) {
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
}

inline uint32_t getU32(const uint8_t* p) {
    return
        (uint32_t(p[0]) << 24) |
        (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) |
        uint32_t(p[3]);
}

// ============================================================
// AES-256-GCM - SZYFROWANIE
// Format: nonce[12] + ciphertext + tag[16]
// ============================================================
inline bool seal(
    uint8_t context,
    const uint8_t* plain,
    size_t len,
    uint8_t* packet,
    size_t& outLen
) {
    outLen = 0;

    if (!plain || !packet ||
        len == 0 || len > MAX_PLAIN) {
        return false;
    }

    randomBytes(packet, 12);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int rc = mbedtls_gcm_setkey(
        &gcm,
        MBEDTLS_CIPHER_ID_AES,
        KEY,
        256
    );

    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(
            &gcm,
            MBEDTLS_GCM_ENCRYPT,
            len,
            packet,
            12,
            &context,
            1,
            plain,
            packet + 12,
            16,
            packet + 12 + len
        );
    }

    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        return false;
    }

    outLen = len + 28;
    return true;
}

// ============================================================
// AES-256-GCM - ODSZYFROWANIE I AUTORYZACJA
// ============================================================
inline bool open(
    uint8_t context,
    const uint8_t* packet,
    size_t len,
    uint8_t* plain,
    size_t& outLen
) {
    outLen = 0;

    if (!packet || !plain ||
        len < 29 || len > MAX_PACKET) {
        return false;
    }

    size_t plainLen = len - 28;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int rc = mbedtls_gcm_setkey(
        &gcm,
        MBEDTLS_CIPHER_ID_AES,
        KEY,
        256
    );

    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &gcm,
            plainLen,
            packet,
            12,
            &context,
            1,
            packet + 12 + plainLen,
            16,
            packet + 12,
            plain
        );
    }

    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        return false;
    }

    outLen = plainLen;
    return true;
}

// ============================================================
// TCP - ODCZYT DOKLADNIE WYMAGANEJ LICZBY BAJTOW
// ============================================================
inline bool readExact(
    WiFiClient& client,
    uint8_t* dst,
    size_t len,
    uint32_t timeoutMs = 1200
) {
    if (!dst || len == 0) {
        return false;
    }

    uint32_t started = millis();
    size_t received = 0;

    while (received < len) {
        int available = client.available();

        if (available > 0) {
            size_t count = std::min(
                size_t(available),
                len - received
            );

            int result = client.read(
                dst + received,
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

        if (millis() - started >= timeoutMs) {
            return false;
        }

        delay(1);
    }

    return true;
}

// ============================================================
// TCP - WYSYLANIE CALEJ RAMKI JEDNYM ZAPISEM
//
// Poprzednio:
//   client.write(header, 2);
//   client.write(packet, packetLen);
//
// Teraz:
//   client.write(frame, frameLen);
//
// Format danych pozostaje identyczny.
// ============================================================
inline bool sendFrame(
    WiFiClient& client,
    uint8_t context,
    const uint8_t* plain,
    size_t len
) {
    if (!client.connected()) {
        return false;
    }

    uint8_t frame[MAX_FRAME];

    size_t packetLen = 0;

    if (!seal(
        context,
        plain,
        len,
        frame + 2,
        packetLen
    )) {
        return false;
    }

    if (packetLen > MAX_PACKET) {
        return false;
    }

    // Naglowek: dlugosc zaszyfrowanego pakietu
    frame[0] = uint8_t(packetLen >> 8);
    frame[1] = uint8_t(packetLen);

    size_t frameLen = packetLen + 2;

    // Jeden zapis do TCP.
    size_t written = client.write(
        frame,
        frameLen
    );

    return written == frameLen;
}

// ============================================================
// TCP - ODBIOR RAMKI
// ============================================================
inline bool recvFrame(
    WiFiClient& client,
    uint8_t context,
    uint8_t* plain,
    size_t& plainLen,
    uint32_t timeoutMs = 1200
) {
    plainLen = 0;

    uint8_t header[2];

    if (!readExact(
        client,
        header,
        sizeof(header),
        timeoutMs
    )) {
        return false;
    }

    size_t packetLen =
        (size_t(header[0]) << 8) |
        size_t(header[1]);

    if (packetLen < 29 ||
        packetLen > MAX_PACKET) {
        return false;
    }

    uint8_t packet[MAX_PACKET];

    if (!readExact(
        client,
        packet,
        packetLen,
        timeoutMs
    )) {
        return false;
    }

    return open(
        context,
        packet,
        packetLen,
        plain,
        plainLen
    );
}

} // namespace SecureLink
