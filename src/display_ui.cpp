#include "display_ui.h"

#include "config.h"

namespace
{
uint8_t utf8CharLength(char lead)
{
  const uint8_t byte = static_cast<uint8_t>(lead);

  if ((byte & 0x80) == 0)
  {
    return 1;
  }
  if ((byte & 0xE0) == 0xC0)
  {
    return 2;
  }
  if ((byte & 0xF0) == 0xE0)
  {
    return 3;
  }
  if ((byte & 0xF8) == 0xF0)
  {
    return 4;
  }

  return 1;
}

uint8_t utf8CodePointCount(const char *word)
{
  uint8_t count = 0;

  for (uint8_t offset = 0; word[offset] != '\0';)
  {
    offset += utf8CharLength(word[offset]);
    ++count;
  }

  return count;
}

uint8_t byteIndexForCodePoint(const char *word, uint8_t codePointIndex)
{
  uint8_t offset = 0;

  for (uint8_t index = 0; word[offset] != '\0' && index < codePointIndex; ++index)
  {
    offset += utf8CharLength(word[offset]);
  }

  return offset;
}

uint8_t orpCodePointIndex(const char *word)
{
  const uint8_t length = utf8CodePointCount(word);

  if (length <= 1)
  {
    return 0;
  }
  if (length <= 5)
  {
    return 1;
  }
  if (length <= 9)
  {
    return 2;
  }

  return 3;
}

void drawPlayIcon(U8G2 &display, int x, int y)
{
  display.drawTriangle(x, y, x, y + 8, x + 7, y + 4);
}

void drawPauseIcon(U8G2 &display, int x, int y)
{
  display.drawBox(x, y, 2, 8);
  display.drawBox(x + 5, y, 2, 8);
}

void drawWord(U8G2 &display, const char *word)
{
  const uint8_t focusCodePoint = orpCodePointIndex(word);
  const uint8_t focusByte = byteIndexForCodePoint(word, focusCodePoint);
  const uint8_t focusByteEnd = focusByte + utf8CharLength(word[focusByte]);

  char before[WORD_BUFFER_SIZE] = {};
  strncpy(before, word, focusByte);

  char focusChar[5] = {};
  strncpy(focusChar, word + focusByte, focusByteEnd - focusByte);

  const uint8_t *const fonts[] = {
      u8g2_font_helvB14_tf,
      u8g2_font_helvB12_tf,
      u8g2_font_helvB10_tf,
      u8g2_font_6x10_tf};

  for (const uint8_t *font : fonts)
  {
    display.setFont(font);

    const int beforeWidth = display.getUTF8Width(before);
    const int focusWidth = display.getUTF8Width(focusChar);
    const int x = 64 - beforeWidth - focusWidth / 2;

    if (x >= 2 && x + display.getUTF8Width(word) <= 126)
    {
      break;
    }
  }

  const int beforeWidth = display.getUTF8Width(before);
  const int focusWidth = display.getUTF8Width(focusChar);
  const int width = display.getUTF8Width(word);

  const int desiredX = 64 - beforeWidth - focusWidth / 2;
  const int x = constrain(desiredX, 2, 126 - width);
  const int y = 34 + (display.getAscent() - display.getDescent()) / 2;

  display.drawUTF8(x, y, word);

  const int focusX = x + beforeWidth;
  const int focusY = y - display.getAscent() - 1;
  const int focusHeight = display.getAscent() - display.getDescent() + 2;

  display.drawBox(focusX, focusY, focusWidth, focusHeight);

  display.setDrawColor(0);
  display.drawUTF8(focusX, y, focusChar);
  display.setDrawColor(1);
}

void renderEmpty(U8G2 &display)
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

void renderUsbReady(U8G2 &display)
{
  display.clearBuffer();

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(35, 10, "USB READY");
  display.drawHLine(8, 18, 112);

  display.drawStr(14, 36, "Use rsvp --port");

  display.setFont(u8g2_font_5x7_tf);
  display.drawStr(0, 63, "Y returns");

  display.sendBuffer();
}

void renderReader(U8G2 &display, const ReaderState &reader)
{
  display.clearBuffer();
  display.setFontMode(1);

  display.setFont(u8g2_font_6x10_tf);
  display.setCursor(11, 9);
  display.print(reader.isReading ? "READING" : "PAUSED");

  if (reader.isReading)
  {
    drawPlayIcon(display, 1, 1);
  }
  else
  {
    drawPauseIcon(display, 1, 1);
  }

  char speed[12];
  snprintf(speed, sizeof(speed), "%u WPM", reader.wpm);

  display.setCursor(128 - display.getStrWidth(speed), 9);
  display.print(speed);
  display.drawHLine(0, 12, 128);

  drawWord(display, reader.currentWord);

  display.setFont(u8g2_font_5x7_tf);

  char chapStr[16];
  snprintf(
      chapStr,
      sizeof(chapStr),
      "Ch %u/%u",
      reader.currentChapter + 1,
      reader.chapterCount);

  display.setCursor(0, 62);
  display.print(chapStr);

  const int textWidth = display.getStrWidth(chapStr);
  const int barX = textWidth + 4;
  const int barWidth = 128 - barX;

  display.drawFrame(barX, 55, barWidth, 8);

  const uint32_t chapWords = reader.chapterEndWord > reader.chapterStartWord
                                 ? (reader.chapterEndWord - reader.chapterStartWord)
                                 : 1;
  const uint32_t wordInChap = reader.currentIndex >= reader.chapterStartWord
                                  ? min(reader.currentIndex - reader.chapterStartWord + 1, chapWords)
                                  : 1;

  const int maxFill = barWidth - 2;
  const uint8_t fill = static_cast<uint8_t>(
      (static_cast<uint64_t>(wordInChap) * maxFill) / chapWords);

  if (fill > 0 && maxFill > 0)
  {
    display.drawBox(barX + 1, 56, min<uint8_t>(fill, maxFill), 6);
  }

  display.sendBuffer();
}
}

void renderDisplay(
    U8G2 &display,
    UiMode mode,
    const ReaderState &reader,
    Preferences &)
{
  if (mode == UiMode::UsbReady)
  {
    renderUsbReady(display);
    return;
  }

  if (!reader.hasBook)
  {
    renderEmpty(display);
    return;
  }

  renderReader(display, reader);
}
