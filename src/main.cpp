#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Wire.h>

constexpr uint8_t I2C_SDA = 8, I2C_SCL = 10;
constexpr uint8_t BTN_PLAY_PAUSE = 4, BTN_SPEED_UP = 5, BTN_SPEED_DOWN = 6;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr char BOOK_PATH[] = "/book.rsvp";
constexpr uint16_t BOOK_VERSION = 1, MIN_WPM = 50, MAX_WPM = 800, WPM_STEP = 25;
constexpr uint8_t WORD_BUFFER_SIZE = 24;
constexpr uint16_t CHECKPOINT_INTERVAL = 40;

#ifndef RSVP_USB_SERIAL_WAIT_MS
#define RSVP_USB_SERIAL_WAIT_MS 0
#endif

U8G2_SSD1309_128X64_NONAME0_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_ADDRESS);
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
  bool stable = HIGH, rawPrevious = HIGH;
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
Button playButton{BTN_PLAY_PAUSE}, upButton{BTN_SPEED_UP}, downButton{BTN_SPEED_DOWN};

File bookFile;
BookHeader book{};
bool hasBook = false, isReading = false, reachedEnd = false;
uint16_t wpm = 250;
uint32_t currentIndex = 0, currentOffset = 0, nextOffset = 0, nextWordAt = 0;
char currentWord[WORD_BUFFER_SIZE] = {};

bool validHeader(const BookHeader &header, size_t fileSize)
{
  return memcmp(header.magic, "RSVP", 4) == 0 && header.version == BOOK_VERSION &&
         header.headerSize == sizeof(BookHeader) && header.wordCount > 0 &&
         header.defaultWpm >= MIN_WPM && header.defaultWpm <= MAX_WPM &&
         fileSize >= sizeof(BookHeader) + header.payloadBytes;
}

bool readWordAt(uint32_t offset, char *out, uint32_t &after)
{
  if (!bookFile.seek(offset, SeekSet))
    return false;
  const int length = bookFile.read();
  if (length <= 0 || length >= WORD_BUFFER_SIZE || offset + 1U + length > bookFile.size())
    return false;
  if (bookFile.readBytes(out, length) != static_cast<size_t>(length))
    return false;
  out[length] = '\0';
  after = offset + 1U + length;
  return true;
}

bool loadCurrentWord() { return readWordAt(currentOffset, currentWord, nextOffset); }

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
  if (!bookFile || bookFile.size() < sizeof(BookHeader) ||
      bookFile.readBytes(reinterpret_cast<char *>(&book), sizeof(book)) != sizeof(book) ||
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
    resetToFirstWord();
  return true;
}

uint32_t delayFor(const char *word)
{
  float multiplier = 1.0f;
  const size_t length = strlen(word);
  if (length >= 9)
    multiplier += .20f;
  else if (length >= 7)
    multiplier += .10f;
  const char last = length ? word[length - 1] : '\0';
  if (last == ',' || last == ';' || last == ':')
    multiplier += .35f;
  if (last == '.' || last == '!' || last == '?')
    multiplier += .85f;
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
void drawPlayIcon(int x, int y) { display.drawTriangle(x, y, x, y + 8, x + 7, y + 4); }
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
  const uint8_t *const fonts[] = {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf,
                                  u8g2_font_helvB10_tf, u8g2_font_6x10_tf, u8g2_font_5x8_tf};
  for (const uint8_t *font : fonts)
  {
    display.setFont(font);
    const int beforeWidth = display.getStrWidth(before);
    const int focusWidth = display.getStrWidth(focusChar);
    const int x = 64 - beforeWidth - focusWidth / 2;
    if (x >= 2 && x + display.getStrWidth(word) <= 126)
      break;
  }
  const int beforeWidth = display.getStrWidth(before), focusWidth = display.getStrWidth(focusChar);
  const int width = display.getStrWidth(word), desiredX = 64 - beforeWidth - focusWidth / 2;
  const int x = constrain(desiredX, 2, 126 - width), y = 34 + (display.getAscent() - display.getDescent()) / 2;
  display.setCursor(x, y);
  display.print(word);
  const int focusX = x + beforeWidth;
  display.drawBox(focusX, y - display.getAscent() - 1, focusWidth, display.getAscent() - display.getDescent() + 2);
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
  snprintf(position, sizeof(position), "%lu/%lu", static_cast<unsigned long>(currentIndex + 1), static_cast<unsigned long>(book.wordCount));
  display.setCursor(0, 58);
  display.print(position);
  display.drawFrame(28, 55, 100, 8);
  const uint8_t fill = static_cast<uint8_t>(((currentIndex + 1ULL) * 98ULL) / book.wordCount);
  if (fill)
    display.drawBox(29, 56, fill, 6);
  display.sendBuffer();
}

void setReading(bool reading)
{
  isReading = reading;
  if (isReading)
    nextWordAt = millis() + delayFor(currentWord);
  else
    bookmark();
  render();
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
  {
    delay(10);
  }

  Serial.println("\n=== RSVP reader boot ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin();
  display.setPowerSave(0);

  pinMode(BTN_PLAY_PAUSE, INPUT_PULLUP);
  pinMode(BTN_SPEED_UP, INPUT_PULLUP);
  pinMode(BTN_SPEED_DOWN, INPUT_PULLUP);

  preferences.begin("rsvp", false);

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
  if (playButton.pressed() && hasBook)
  {
    if (!isReading && reachedEnd)
      resetToFirstWord();
    setReading(!isReading);
  }
  if (upButton.pressed() && wpm < MAX_WPM)
  {
    wpm = min<uint16_t>(MAX_WPM, wpm + WPM_STEP);
    bookmark();
    render();
  }
  if (downButton.pressed() && wpm > MIN_WPM)
  {
    wpm = max<uint16_t>(MIN_WPM, wpm - WPM_STEP);
    bookmark();
    render();
  }
  if (isReading && static_cast<int32_t>(millis() - nextWordAt) >= 0)
  {
    if (currentIndex + 1 >= book.wordCount)
    {
      reachedEnd = true;
      setReading(false);
    }
    else
    {
      ++currentIndex;
      currentOffset = nextOffset;
      if (!loadCurrentWord())
      {
        reachedEnd = true;
        setReading(false);
      }
      else
      {
        nextWordAt = millis() + delayFor(currentWord);
        if (currentIndex % CHECKPOINT_INTERVAL == 0)
          bookmark();
        render();
      }
    }
  }
}
