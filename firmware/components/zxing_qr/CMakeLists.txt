set(ZXING_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/zxing-cpp/core/src")

set(ZXING_QR_SRCS
    "zxing_qr_decoder.cpp"
    "${ZXING_SRC_DIR}/Barcode.cpp"
    "${ZXING_SRC_DIR}/BarcodeFormat.cpp"
    "${ZXING_SRC_DIR}/BinaryBitmap.cpp"
    "${ZXING_SRC_DIR}/BitArray.cpp"
    "${ZXING_SRC_DIR}/BitMatrix.cpp"
    "${ZXING_SRC_DIR}/BitMatrixIO.cpp"
    "${ZXING_SRC_DIR}/BitSource.cpp"
    "${ZXING_SRC_DIR}/CharacterSet.cpp"
    "${ZXING_SRC_DIR}/ConcentricFinder.cpp"
    "${ZXING_SRC_DIR}/Content.cpp"
    "${ZXING_SRC_DIR}/ECI.cpp"
    "${ZXING_SRC_DIR}/Error.cpp"
    "${ZXING_SRC_DIR}/GlobalHistogramBinarizer.cpp"
    "${ZXING_SRC_DIR}/GridSampler.cpp"
    "${ZXING_SRC_DIR}/HRI.cpp"
    "${ZXING_SRC_DIR}/HybridBinarizer.cpp"
    "${ZXING_SRC_DIR}/JSON.cpp"
    "${ZXING_SRC_DIR}/LocalGrid.cpp"
    "${ZXING_SRC_DIR}/MultiFormatReader.cpp"
    "${ZXING_SRC_DIR}/PerspectiveTransform.cpp"
    "${ZXING_SRC_DIR}/ReadBarcode.cpp"
    "${ZXING_SRC_DIR}/ReedSolomon.cpp"
    "${ZXING_SRC_DIR}/ResultPoint.cpp"
    "${ZXING_SRC_DIR}/TextDecoder.cpp"
    "${ZXING_SRC_DIR}/Utf.cpp"
    "${ZXING_SRC_DIR}/WhiteRectDetector.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRBitMatrixParser.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRCodecMode.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRDataBlock.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRDecoder.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRDetector.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRErrorCorrectionLevel.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRFormatInformation.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRReader.cpp"
    "${ZXING_SRC_DIR}/qrcode/QRVersion.cpp"
    "${ZXING_SRC_DIR}/libzueci/zueci.c"
)

idf_component_register(
    SRCS ${ZXING_QR_SRCS}
    INCLUDE_DIRS
        "include"
        "${ZXING_SRC_DIR}"
    PRIV_REQUIRES esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ZXING_INTERNAL=1
    LIBRSCPP_SAVE_MEMORY=1
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-missing-field-initializers
    -Wno-type-limits
    -Wno-unused-parameter
    -Wno-unused-function
)
