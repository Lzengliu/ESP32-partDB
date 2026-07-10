#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t target_number;
} nfc_tag_t;

esp_err_t nfc_pn532_init(void);
esp_err_t nfc_pn532_read_passive(nfc_tag_t *tag, unsigned timeout_ms);
esp_err_t nfc_pn532_read_ndef_text(char *out, size_t out_len, unsigned timeout_ms);
esp_err_t nfc_pn532_write_ndef_text(const char *text, unsigned timeout_ms);
esp_err_t nfc_pn532_ping(unsigned timeout_ms);
void nfc_pn532_mark_not_ready(void);
bool nfc_pn532_is_ready(void);
