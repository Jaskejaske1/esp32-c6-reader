#pragma once

#include <Arduino.h>

// Hardware
constexpr uint8_t I2C_SDA = 8;
constexpr uint8_t I2C_SCL = 10;
constexpr uint8_t BTN_YELLOW_PRIMARY = 4;
constexpr uint8_t BTN_GREEN_UP_ACCEPT = 5;
constexpr uint8_t BTN_RED_DOWN_REJECT = 6;
constexpr uint8_t OLED_ADDRESS = 0x3C;

// Storage and reader limits
constexpr char BOOK_PATH[] = "/book.rsvp";
constexpr char UPLOAD_PATH[] = "/book.upload";
constexpr char BACKUP_PATH[] = "/book.prev";
constexpr uint16_t BOOK_VERSION = 1;
constexpr uint16_t MIN_WPM = 50;
constexpr uint16_t MAX_WPM = 800;
constexpr uint16_t WPM_STEP = 25;
constexpr uint8_t WORD_BUFFER_SIZE = 24;
constexpr uint16_t CHECKPOINT_INTERVAL = 40;
constexpr uint32_t MAX_UPLOAD_BYTES = 6500000;
constexpr uint32_t UPLOAD_SPACE_MARGIN_BYTES = 4096;

// UI timing
constexpr uint16_t USB_READY_HOLD_MS = 3000;
constexpr uint32_t USB_READY_RESULT_MS = 1800;
