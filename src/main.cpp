#include <Arduino.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>

// --- Hardware ---------------------------------------------------------------

constexpr uint8_t I2C_SDA = 8;
constexpr uint8_t I2C_SCL = 10;

constexpr uint8_t BTN_YELLOW_PRIMARY = 4;  // Play / pause / later pairing
constexpr uint8_t BTN_GREEN_UP_ACCEPT = 5; // WPM up / later confirm
constexpr uint8_t BTN_RED_DOWN_REJECT = 6; // WPM down / later back

constexpr uint8_t OLED_ADDRESS = 0x3C;

// --- Reader / storage -------------------------------------------------------

constexpr char BOOK_PATH[] = "/book.rsvp";
constexpr uint16_t BOOK_VERSION = 1;
constexpr uint16_t MIN_WPM = 50;
constexpr uint16_t MAX_WPM = 800;
constexpr uint16_t WPM_STEP = 25;
constexpr uint8_t WORD_BUFFER_SIZE = 24;
constexpr uint16_t CHECKPOINT_INTERVAL = 40;

// --- BLE v1 namespace -------------------------------------------------------
// This checkpoint only advertises, reports metadata and reports state.
// Pairing, trusted devices, remote controls and uploads come next.

constexpr char BLE_SERVICE_UUID[] = "12345678-1234-1234-1234-123456789abc";
constexpr char BLE_METADATA_UUID[] = "12345678-1234-1234-1234-123456789abe";
constexpr char BLE_STATE_UUID[] = "12345678-1234-1234-1234-123456789abf";

U8G2_SSD1309_128X64_NONAME0_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE);

Preferences preferences;

struct __attribute__((packed)) BookHeader
{
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t bookId;
  uint32_t wordCount;
  uint32_t payloadBytes;
  uint16_t defaultWpm;
  uint16_t flags;
  char title[48];
  char author[32];
  uint32_t payloadCrc32;
};

static_assert(sizeof(BookHeader) == 108, "Book header format changed");

struct Button
{
  uint8_t pin;
  bool stable = HIGH;
  bool rawPrevious = HIGH;
  uint32_t changedAt = 0;

  bool pressed()
  {
    const bool raw = digitalRead(pin);
    const uint32_t now = millis();

    if (raw != rawPrevious)
    {
      rawPrevious = raw;
      changedAt = now;
    }

    if (now - changedAt >= 30 && raw != stable)
    {
      stable = raw;
      return stable == LOW;
    }

    return false;
  }
};

Button yellowButton{BTN_YELLOW_PRIMARY};
Button greenButton{BTN_GREEN_UP_ACCEPT};
Button redButton{BTN_RED_DOWN_REJECT};

File bookFile;
BookHeader book{};

bool hasBook = false;
bool isReading = false;
bool reachedEnd = false;

uint16_t wpm = 250;
uint32_t currentIndex = 0;
uint32_t currentOffset = 0;
uint32_t nextOffset = 0;
uint32_t nextWordAt = 0;

char currentWord[WORD_BUFFER_SIZE] = {};

// --- BLE state --------------------------------------------------------------

NimBLEAdvertising *bleAdvertising = nullptr;
NimBLECharacteristic *bleMetadataCharacteristic = nullptr;
NimBLECharacteristic *bleStateCharacteristic = nullptr;

void publishBleMetadata();
void publishBleState();

// --- Book handling ----------------------------------------------------------

bool validHeader(const BookHeader &header, size_t fileSize)
{
  return memcmp(header.magic, "RSVP", 4) == 0 &&
         header.version == BOOK_VERSION &&
         header.headerSize == sizeof(BookHeader) &&
         header.wordCount > 0 &&
         header.defaultWpm >= MIN_WPM &&
         header.defaultWpm <= MAX_WPM &&
         fileSize >= sizeof(BookHeader) + header.payloadBytes;
}

bool readWordAt(uint32_t offset, char *out, uint32_t &after)
{
  if (!bookFile.seek(offset, SeekSet))
    return false;

  const int length = bookFile.read();

  if (length <= 0 ||
      length >= WORD_BUFFER_SIZE ||
      offset + 1U + length > bookFile.size())
  {
    return false;
  }

  if (bookFile.readBytes(out, length) != static_cast<size_t>(length))
    return false;

  out[length] = '\0';
  after = offset + 1U + length;
  return true;
}

bool loadCurrentWord()
{
  return readWordAt(currentOffset, currentWord, nextOffset);
}

void bookmark()
{
  if (!hasBook)
    return;

  preferences.putUInt("book", book.bookId);
  preferences.putUInt("index", currentIndex);
  preferences.putUInt("offset", currentOffset);
  preferences.putUShort("wpm", wpm);
}

void resetToFirstWord()
{
  currentIndex = 0;
  currentOffset = sizeof(BookHeader);
  reachedEnd = false;
  loadCurrentWord();
}

bool openBook()
{
  bookFile = LittleFS.open(BOOK_PATH, "r");

  if (!bookFile ||
      bookFile.size() < sizeof(BookHeader) ||
      bookFile.readBytes(
          reinterpret_cast<char *>(&book),
          sizeof(book)) != sizeof(book) ||
      !validHeader(book, bookFile.size()))
  {
    if (bookFile)
      bookFile.close();

    return false;
  }

  hasBook = true;

  wpm = preferences.getUShort("wpm", book.defaultWpm);

  if (wpm < MIN_WPM || wpm > MAX_WPM)
    wpm = book.defaultWpm;

  if (preferences.getUInt("book", 0) == book.bookId)
  {
    currentIndex = preferences.getUInt("index", 0);
    currentOffset = preferences.getUInt("offset", sizeof(BookHeader));

    if (currentIndex >= book.wordCount || !loadCurrentWord())
      resetToFirstWord();
  }
  else
  {
    resetToFirstWord();
  }

  return true;
}

// --- RSVP timing / ORP ------------------------------------------------------

uint32_t delayFor(const char *word)
{
  float multiplier = 1.0f;
  const size_t length = strlen(word);

  if (length >= 9)
    multiplier += 0.20f;
  else if (length >= 7)
    multiplier += 0.10f;

  const char last = length ? word[length - 1] : '\0';

  if (last == ',' || last == ';' || last == ':')
    multiplier += 0.35f;

  if (last == '.' || last == '!' || last == '?')
    multiplier += 0.85f;

  return static_cast<uint32_t>(60000.0f / wpm * multiplier);
}

uint8_t orpIndex(const char *word)
{
  const uint8_t length = strlen(word);

  if (length <= 1)
    return 0;
  if (length <= 5)
    return 1;
  if (length <= 9)
    return 2;

  return 3;
}

// --- Display ----------------------------------------------------------------

void drawPlayIcon(int x, int y)
{
  display.drawTriangle(x, y, x, y + 8, x + 7, y + 4);
}

void drawPauseIcon(int x, int y)
{
  display.drawBox(x, y, 2, 8);
  display.drawBox(x + 5, y, 2, 8);
}

void drawWord(const char *word)
{
  const uint8_t focus = orpIndex(word);

  char before[WORD_BUFFER_SIZE] = {};
  strncpy(before, word, focus);

  char focusChar[2] = {word[focus], '\0'};

  const uint8_t *const fonts[] = {
      u8g2_font_helvB14_tf,
      u8g2_font_helvB12_tf,
      u8g2_font_helvB10_tf,
      u8g2_font_6x10_tf,
      u8g2_font_5x8_tf};

  for (const uint8_t *font : fonts)
  {
    display.setFont(font);

    const int beforeWidth = display.getStrWidth(before);
    const int focusWidth = display.getStrWidth(focusChar);
    const int x = 64 - beforeWidth - focusWidth / 2;

    if (x >= 2 && x + display.getStrWidth(word) <= 126)
      break;
  }

  const int beforeWidth = display.getStrWidth(before);
  const int focusWidth = display.getStrWidth(focusChar);
  const int width = display.getStrWidth(word);

  const int desiredX = 64 - beforeWidth - focusWidth / 2;
  const int x = constrain(desiredX, 2, 126 - width);
  const int y = 34 + (display.getAscent() - display.getDescent()) / 2;

  display.setCursor(x, y);
  display.print(word);

  const int focusX = x + beforeWidth;
  const int focusY = y - display.getAscent() - 1;
  const int focusHeight = display.getAscent() - display.getDescent() + 2;

  display.drawBox(focusX, focusY, focusWidth, focusHeight);

  display.setDrawColor(0);
  display.setCursor(focusX, y);
  display.print(focusChar);
  display.setDrawColor(1);
}

void renderEmpty()
{
  display.clearBuffer();

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(43, 18, "NO BOOK");
  display.drawHLine(8, 23, 112);

  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(14, 40, "make: data/book.rsvp");
  display.drawStr(19, 52, "flash: pio uploadfs");

  display.sendBuffer();
}

void render()
{
  if (!hasBook)
  {
    renderEmpty();
    return;
  }

  display.clearBuffer();
  display.setFontMode(1);

  display.setFont(u8g2_font_6x10_tf);
  display.setCursor(11, 9);
  display.print(isReading ? "READING" : "PAUSED");

  // State icon: triangle means reading, bars mean paused.
  if (isReading)
    drawPlayIcon(1, 1);
  else
    drawPauseIcon(1, 1);

  char speed[12];
  snprintf(speed, sizeof(speed), "%u WPM", wpm);

  display.setCursor(128 - display.getStrWidth(speed), 9);
  display.print(speed);
  display.drawHLine(0, 12, 128);

  drawWord(currentWord);

  display.setFont(u8g2_font_5x7_tf);

  char position[24];
  snprintf(
      position,
      sizeof(position),
      "%lu/%lu",
      static_cast<unsigned long>(currentIndex + 1),
      static_cast<unsigned long>(book.wordCount));

  display.setCursor(0, 58);
  display.print(position);

  display.drawFrame(28, 55, 100, 8);

  const uint8_t fill = static_cast<uint8_t>(
      ((currentIndex + 1ULL) * 98ULL) / book.wordCount);

  if (fill)
    display.drawBox(29, 56, fill, 6);

  display.sendBuffer();
}

// --- BLE --------------------------------------------------------------------

class ReaderBleCallbacks final : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override
  {
    Serial.printf(
        "BLE connected: %s\n",
        connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(
      NimBLEServer *,
      NimBLEConnInfo &connInfo,
      int reason) override
  {
    Serial.printf(
        "BLE disconnected: %s (reason %d)\n",
        connInfo.getAddress().toString().c_str(),
        reason);

    if (bleAdvertising)
      bleAdvertising->start();
  }
};

void publishBleMetadata()
{
  if (!bleMetadataCharacteristic)
    return;

  char metadata[128];

  if (hasBook)
  {
    snprintf(
        metadata,
        sizeof(metadata),
        "v1|%s|%s|%lu",
        book.title,
        book.author,
        static_cast<unsigned long>(book.wordCount));
  }
  else
  {
    snprintf(metadata, sizeof(metadata), "v1|NO_BOOK");
  }

  bleMetadataCharacteristic->setValue(metadata);
}

void publishBleState()
{
  if (!bleStateCharacteristic)
    return;

  char state[80];

  snprintf(
      state,
      sizeof(state),
      "playing=%u;wpm=%u;word=%lu;total=%lu",
      isReading ? 1 : 0,
      wpm,
      static_cast<unsigned long>(hasBook ? currentIndex + 1 : 0),
      static_cast<unsigned long>(hasBook ? book.wordCount : 0));

  bleStateCharacteristic->setValue(state);
  bleStateCharacteristic->notify();
}

void initBle()
{
  NimBLEDevice::init("RSVP Reader");
  NimBLEDevice::setMTU(247);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ReaderBleCallbacks());

  NimBLEService *service = server->createService(BLE_SERVICE_UUID);

  bleMetadataCharacteristic = service->createCharacteristic(
      BLE_METADATA_UUID,
      NIMBLE_PROPERTY::READ);

  bleStateCharacteristic = service->createCharacteristic(
      BLE_STATE_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  publishBleMetadata();
  publishBleState();

  bleAdvertising = NimBLEDevice::getAdvertising();
  bleAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  bleAdvertising->setName("RSVP Reader");
  bleAdvertising->enableScanResponse(true);

  // 1,600 × 0.625 ms = one advertisement each second.
  bleAdvertising->setAdvertisingInterval(1600);
  bleAdvertising->start();

  Serial.println("BLE advertising as: RSVP Reader");
}

// --- Reader actions ---------------------------------------------------------

void setReading(bool reading)
{
  isReading = reading;

  if (isReading)
    nextWordAt = millis() + delayFor(currentWord);
  else
    bookmark();

  render();
  publishBleState();
}

// --- Arduino ----------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  // Lets `pio device monitor` attach after PlatformIO resets the board.
  delay(1500);

  Serial.println("\n=== RSVP reader boot ===");

  Serial.println("1: Wire");
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("2: OLED begin");
  display.setI2CAddress(OLED_ADDRESS << 1); // U8g2 uses 8-bit address: 0x78
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

    hasBook = openBook();
  }

  Serial.println("7: BLE");
  initBle();

  render();

  if (hasBook)
  {
    Serial.printf(
        "Book: %s (%lu words), resuming %lu\n",
        book.title,
        static_cast<unsigned long>(book.wordCount),
        static_cast<unsigned long>(currentIndex + 1));
  }
  else
  {
    Serial.println("ERROR: /book.rsvp is absent or invalid");
  }
}

void loop()
{
  if (yellowButton.pressed() && hasBook)
  {
    if (!isReading && reachedEnd)
      resetToFirstWord();

    setReading(!isReading);
  }

  if (greenButton.pressed() && wpm < MAX_WPM)
  {
    wpm = min<uint16_t>(MAX_WPM, wpm + WPM_STEP);
    bookmark();
    render();
    publishBleState();
  }

  if (redButton.pressed() && wpm > MIN_WPM)
  {
    wpm = max<uint16_t>(MIN_WPM, wpm - WPM_STEP);
    bookmark();
    render();
    publishBleState();
  }

  if (isReading && static_cast<int32_t>(millis() - nextWordAt) >= 0)
  {
    if (currentIndex + 1 >= book.wordCount)
    {
      reachedEnd = true;
      setReading(false);
      return;
    }

    ++currentIndex;
    currentOffset = nextOffset;

    if (!loadCurrentWord())
    {
      reachedEnd = true;
      setReading(false);
      return;
    }

    nextWordAt = millis() + delayFor(currentWord);

    if (currentIndex % CHECKPOINT_INTERVAL == 0)
      bookmark();

    render();
    publishBleState();
  }
}