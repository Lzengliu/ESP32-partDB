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

typedef struct {
    esp_err_t read0_err;
    esp_err_t read4_err;
    bool type2_ndef;
    size_t capacity;
    uint8_t page0[16];
    uint8_t page4[16];
    uint8_t cc[4];
} nfc_type2_info_t;

esp_err_t nfc_pn532_init(void);
esp_err_t nfc_pn532_read_passive(nfc_tag_t *tag, unsigned timeout_ms);
esp_err_t nfc_pn532_start_passive_scan(void);
bool nfc_pn532_passive_scan_pending(void);
bool nfc_pn532_passive_scan_ready(void);
esp_err_t nfc_pn532_read_passive_scan_result(nfc_tag_t *tag, unsigned timeout_ms);
esp_err_t nfc_pn532_read_ndef_text(char *out, size_t out_len, unsigned timeout_ms);
esp_err_t nfc_pn532_read_ndef_text_from_tag(const nfc_tag_t *tag, char *out,
                                            size_t out_len, unsigned timeout_ms);
esp_err_t nfc_pn532_write_ndef_text(const char *text, unsigned timeout_ms);
esp_err_t nfc_pn532_write_ndef_text_to_tag(const nfc_tag_t *tag, const char *text,
                                           unsigned timeout_ms);
esp_err_t nfc_pn532_erase_ndef(unsigned timeout_ms);
esp_err_t nfc_pn532_erase_ndef_from_tag(const nfc_tag_t *tag, unsigned timeout_ms);
esp_err_t nfc_pn532_probe_type2_from_tag(const nfc_tag_t *tag, nfc_type2_info_t *info,
                                         unsigned timeout_ms);
esp_err_t nfc_pn532_ping(unsigned timeout_ms);
void nfc_pn532_mark_not_ready(void);
void nfc_pn532_suspend_for_camera(void);
bool nfc_pn532_is_ready(void);
