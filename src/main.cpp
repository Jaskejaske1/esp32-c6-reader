#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "ble_transport.h"
#include "book_format.h"
#include "book_store.h"
#include "buttons.h"
#include "config.h"
#include "control.h"
#include "display_ui.h"
#include "reader.h"
#include "serial_transport.h"
#include "upload_session.h"

U8G2_SSD1309_128X64_NONAME0_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE);

Preferences preferences;

Button yellowButton{BTN_YELLOW_PRIMARY};
Button greenButton{BTN_GREEN_UP_ACCEPT};
Button redButton{BTN_RED_DOWN_REJECT};

ReaderState reader;
UploadSession uploadSession;
SerialTransport serialTransport;
BleTransport bleTransport;

UiMode uiMode = UiMode::Reader;
uint32_t yellowPressedAt = 0;
uint32_t usbReadyUntil = 0;
bool yellowHoldHandled = false;
char uploadStatus[80] = "idle";

void render();
void publishReaderState();
void setUploadStatus(const char *status);
const char *currentUploadStatus();
bool activateUploadedBook();
void stopReadingForUpload();
bool handleSerialControlCommand(const char *command);
void handleReaderButtons(uint32_t now);

bool activateUploadedBook()
{
  BookHeader uploadedHeader{};

  if (reader.bookFile)
  {
    reader.bookFile.close();
  }

  if (!installUploadedBook(uploadedHeader))
  {
    Serial.println("Upload activation failed; active book preserved");
    return false;
  }

  preferences.putUInt("book", uploadedHeader.bookId);
  preferences.putUInt("index", 0);
  preferences.putUInt("offset", sizeof(BookHeader));
  preferences.putUShort("wpm", uploadedHeader.defaultWpm);

  readerClearBook(reader);
  reader.hasBook = readerOpenBook(reader, preferences);
  render();

  Serial.printf(
      "Upload activated: %s (%lu words)\n",
      uploadedHeader.title,
      static_cast<unsigned long>(uploadedHeader.wordCount));

  return reader.hasBook;
}

void render()
{
  renderDisplay(display, uiMode, reader, preferences);
}

void publishReaderState()
{
  // Serial is request/response in v1. STATUS returns the latest state on demand.
}

void setUploadStatus(const char *status)
{
  snprintf(uploadStatus, sizeof(uploadStatus), "%s", status);
}

const char *currentUploadStatus()
{
  return uploadStatus;
}

void stopReadingForUpload()
{
  readerSetReading(reader, preferences, false);
  render();
}

bool handleSerialControlCommand(const char *command)
{
  if (!applyControlCommand(reader, preferences, command))
  {
    return false;
  }

  render();
  return true;
}

void handleReaderButtons(uint32_t now)
{
  if (yellowButton.pressed())
  {
    yellowPressedAt = now;
    yellowHoldHandled = false;
  }

  if (yellowButton.isDown() &&
      !yellowHoldHandled &&
      !reader.isReading &&
      now - yellowPressedAt >= USB_READY_HOLD_MS)
  {
    yellowHoldHandled = true;
    uiMode = UiMode::UsbReady;
    usbReadyUntil = now + USB_READY_RESULT_MS;
    readerSetReading(reader, preferences, false);
    render();
    return;
  }

  if (yellowButton.released() && !yellowHoldHandled)
  {
    if (uiMode == UiMode::UsbReady)
    {
      uiMode = UiMode::Reader;
      render();
      return;
    }

    if (toggleReader(reader, preferences))
    {
      render();
      publishReaderState();
    }
  }

  if (greenButton.pressed())
  {
    if (increaseReaderWpm(reader, preferences))
    {
      render();
      publishReaderState();
    }
  }

  if (redButton.pressed())
  {
    if (decreaseReaderWpm(reader, preferences))
    {
      render();
      publishReaderState();
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=== RSVP reader boot ===");

  Serial.println("1: Wire");
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("2: OLED begin");
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  Serial.println("3: OLED power");
  display.setPowerSave(0);

  Serial.println("4: buttons");
  pinMode(BTN_YELLOW_PRIMARY, INPUT_PULLUP);
  pinMode(BTN_GREEN_UP_ACCEPT, INPUT_PULLUP);
  pinMode(BTN_RED_DOWN_REJECT, INPUT_PULLUP);

  Serial.println("5: preferences");
  preferences.begin("rsvp", false);

  Serial.println("6: filesystem");
  if (!LittleFS.begin(false, "/littlefs", 10, "spiffs"))
  {
    Serial.println("ERROR: LittleFS mount failed");
  }
  else
  {
    Serial.printf(
        "LittleFS mounted: %u / %u bytes used\n",
        LittleFS.usedBytes(),
        LittleFS.totalBytes());

    reader.hasBook = readerOpenBook(reader, preferences);
  }

  Serial.println("7: USB serial & BLE");
  const SerialTransportCallbacks serialCallbacks = {
      &reader,
      &preferences,
      &uploadSession,
      stopReadingForUpload,
      activateUploadedBook,
      handleSerialControlCommand,
      render,
      currentUploadStatus,
      setUploadStatus};
  serialTransport.begin(Serial, serialCallbacks);
  bleTransport.begin(serialCallbacks);

  render();

  if (reader.hasBook)
  {
    Serial.printf(
        "Book: %s (%lu words), resuming %lu\n",
        reader.book.title,
        static_cast<unsigned long>(reader.book.wordCount),
        static_cast<unsigned long>(reader.currentIndex + 1));
  }
  else
  {
    Serial.println("ERROR: /book.rsvp is absent or invalid");
  }
}

void loop()
{
  yellowButton.update();
  greenButton.update();
  redButton.update();

  const uint32_t now = millis();

  serialTransport.tick(now);
  bleTransport.tick(now);
  handleReaderButtons(now);

  if (uiMode == UiMode::UsbReady && now > usbReadyUntil)
  {
    uiMode = UiMode::Reader;
    render();
  }

  if (readerTick(reader, preferences, now))
  {
    render();
    publishReaderState();
  }
}
