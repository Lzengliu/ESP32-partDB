#include "zxing_qr_decoder.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>

#include "Barcode.h"
#include "BarcodeFormat.h"
#include "ImageView.h"
#include "ReadBarcode.h"
#include "ReaderOptions.h"
#include "esp_timer.h"

namespace {

void copy_cstr(char *dst, size_t dst_len, const std::string &src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    size_t n = std::min(dst_len - 1, src.size());
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = std::min(dst_len - 1, std::strlen(src));
    if (n > 0) {
        std::memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void clear_result(zxing_qr_result_t *result, int width, int height)
{
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->width = width;
    result->height = height;
}

} // namespace

extern "C" esp_err_t zxing_qr_decode_luma_ex(const uint8_t *luma,
                                              int width,
                                              int height,
                                              int row_stride,
                                              char *text,
                                              size_t text_len,
                                              zxing_qr_result_t *result,
                                              bool robust_fallback)
{
    clear_result(result, width, height);
    if (text && text_len > 0) {
        text[0] = '\0';
    }

    if (!luma || width <= 0 || height <= 0 || row_stride < width || !text || text_len == 0) {
        copy_cstr(result ? result->error : nullptr, result ? sizeof(result->error) : 0, "invalid_image");
        return ESP_ERR_INVALID_ARG;
    }

    int64_t start_us = esp_timer_get_time();
    try {
        ZXing::ImageView image(luma, width, height, ZXing::ImageFormat::Lum, row_stride, 1);
        auto read_qr = [&](bool robust) {
            ZXing::ReaderOptions opts;
            opts.formats(ZXing::BarcodeFormat::QRCode)
                .tryHarder(robust)
                .tryRotate(true)
                .tryInvert(robust)
                .tryDownscale(robust)
                .binarizer(robust ? ZXing::Binarizer::LocalAverage :
                                     ZXing::Binarizer::GlobalHistogram)
                .textMode(ZXing::TextMode::Plain)
                .maxNumberOfSymbols(1);
            return ZXing::ReadBarcode(image, opts);
        };

        ZXing::Barcode barcode = read_qr(false);
        if (!barcode.isValid() && robust_fallback) {
            barcode = read_qr(true);
        }
        if (result) {
            result->elapsed_ms = (int)((esp_timer_get_time() - start_us) / 1000);
        }

        if (!barcode.isValid()) {
            if (result) {
                result->found = false;
                copy_cstr(result->format, sizeof(result->format), ZXing::ToString(barcode.format()));
                copy_cstr(result->error, sizeof(result->error), ZXing::ToString(barcode.error()));
            }
            return ESP_ERR_NOT_FOUND;
        }

        std::string payload = barcode.text(ZXing::TextMode::Plain);
        if (payload.empty()) {
            if (result) {
                copy_cstr(result->error, sizeof(result->error), "empty_payload");
            }
            return ESP_ERR_INVALID_RESPONSE;
        }

        copy_cstr(text, text_len, payload);
        if (result) {
            result->found = true;
            result->orientation = barcode.orientation();
            result->inverted = barcode.isInverted();
            result->mirrored = barcode.isMirrored();
            copy_cstr(result->format, sizeof(result->format), ZXing::ToString(barcode.format()));
            result->error[0] = '\0';
        }
        return ESP_OK;
    } catch (const std::exception &e) {
        if (result) {
            result->elapsed_ms = (int)((esp_timer_get_time() - start_us) / 1000);
            copy_cstr(result->error, sizeof(result->error), e.what());
        }
        return ESP_FAIL;
    } catch (...) {
        if (result) {
            result->elapsed_ms = (int)((esp_timer_get_time() - start_us) / 1000);
            copy_cstr(result->error, sizeof(result->error), "zxing_exception");
        }
        return ESP_FAIL;
    }
}

extern "C" esp_err_t zxing_qr_decode_luma(const uint8_t *luma,
                                           int width,
                                           int height,
                                           int row_stride,
                                           char *text,
                                           size_t text_len,
                                           zxing_qr_result_t *result)
{
    return zxing_qr_decode_luma_ex(luma, width, height, row_stride,
                                   text, text_len, result, true);
}
