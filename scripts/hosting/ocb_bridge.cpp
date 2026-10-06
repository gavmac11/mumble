// Test-only C ABI around the repository's unchanged Mumble OCB2 implementation.
// Use of this source code is governed by the repository's BSD-style license.
#include "crypto/CryptStateOCB2.h"
#include <memory>
#include <string>

extern "C" {
void *mumble_pilot_crypto_new(const unsigned char *key, const unsigned char *client_nonce,
                            const unsigned char *server_nonce) noexcept {
    if (!key || !client_nonce || !server_nonce) {
        return nullptr;
    }
    try {
        auto state = std::make_unique<CryptStateOCB2>();
        if (!state->setKey(std::string(reinterpret_cast<const char *>(key), 16),
                           std::string(reinterpret_cast<const char *>(client_nonce), 16),
                           std::string(reinterpret_cast<const char *>(server_nonce), 16))) {
            return nullptr;
        }
        return state.release();
    } catch (...) {
        return nullptr;
    }
}

void mumble_pilot_crypto_free(void *handle) noexcept {
    delete static_cast<CryptStateOCB2 *>(handle);
}

int mumble_pilot_encrypt(void *handle, const unsigned char *input, unsigned int length,
                        unsigned char *output, unsigned int capacity) noexcept {
    if (!handle || !input || !output || length == 0 || length > 1024 || capacity < length + 4) {
        return -1;
    }
    try {
        return static_cast<CryptStateOCB2 *>(handle)->encrypt(input, output, length)
            ? static_cast<int>(length + 4) : -1;
    } catch (...) {
        return -1;
    }
}

int mumble_pilot_decrypt(void *handle, const unsigned char *input, unsigned int length,
                        unsigned char *output, unsigned int capacity) noexcept {
    if (!handle || !input || !output || length < 5 || length > 1028 || capacity < length - 4) {
        return -1;
    }
    try {
        return static_cast<CryptStateOCB2 *>(handle)->decrypt(input, output, length)
            ? static_cast<int>(length - 4) : -1;
    } catch (...) {
        return -1;
    }
}
}
