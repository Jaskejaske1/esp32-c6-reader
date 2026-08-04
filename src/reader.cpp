#include "reader.h"

#include <LittleFS.h>

#include "book_store.h"
#include "config.h"

bool readerLoadCurrentWord(ReaderState &reader)
{
  return readWordAt(
      reader.bookFile,
      reader.currentOffset,
      reader.currentWord,
      reader.nextOffset);
}

void readerBookmark(ReaderState &reader, Preferences &preferences)
{
  if (!reader.hasBook)
  {
    return;
  }

  preferences.putUInt("book", reader.book.bookId);
  preferences.putUInt("index", reader.currentIndex);
  preferences.putUInt("offset", reader.currentOffset);
  preferences.putUShort("wpm", reader.wpm);
}

void updateCurrentChapter(ReaderState &reader)
{
  if (!reader.hasBook || reader.chapterCount == 0)
  {
    reader.currentChapter = 0;
    reader.chapterStartWord = 0;
    reader.chapterEndWord = reader.hasBook ? reader.book.wordCount : 0;
    return;
  }

  uint16_t chap = 0;
  while (chap + 1 < reader.chapterCount && reader.chapterStarts[chap + 1] <= reader.currentIndex)
  {
    ++chap;
  }

  reader.currentChapter = chap;
  reader.chapterStartWord = reader.chapterStarts[chap];
  reader.chapterEndWord = (chap + 1 < reader.chapterCount)
                              ? reader.chapterStarts[chap + 1]
                              : reader.book.wordCount;
}

void readerResetToFirstWord(ReaderState &reader)
{
  reader.currentIndex = 0;
  reader.currentOffset = getPayloadOffset(reader.book);
  reader.reachedEnd = false;
  updateCurrentChapter(reader);
  readerLoadCurrentWord(reader);
}

bool readerSeekToWord(
    ReaderState &reader,
    Preferences &preferences,
    uint32_t index)
{
  if (!reader.hasBook || index >= reader.book.wordCount)
  {
    return false;
  }

  uint32_t offset = getPayloadOffset(reader.book);
  uint32_t after = offset;
  char buffer[WORD_BUFFER_SIZE] = {};

  for (uint32_t i = 0; i <= index; ++i)
  {
    if (!readWordAt(reader.bookFile, offset, buffer, after))
    {
      return false;
    }

    if (i == index)
    {
      reader.currentIndex = index;
      reader.currentOffset = offset;
      reader.nextOffset = after;
      strncpy(reader.currentWord, buffer, sizeof(reader.currentWord) - 1);
      reader.currentWord[sizeof(reader.currentWord) - 1] = '\0';
      reader.reachedEnd = false;
      updateCurrentChapter(reader);
      readerBookmark(reader, preferences);
      return true;
    }

    offset = after;
  }

  return false;
}

bool readerOpenBook(ReaderState &reader, Preferences &preferences)
{
  reader.bookFile = LittleFS.open(BOOK_PATH, "r");

  if (!reader.bookFile ||
      reader.bookFile.size() < sizeof(BookHeader) ||
      reader.bookFile.readBytes(
          reinterpret_cast<char *>(&reader.book),
          sizeof(reader.book)) != sizeof(reader.book) ||
      !validHeader(reader.book, reader.bookFile.size()))
  {
    if (reader.bookFile)
    {
      reader.bookFile.close();
    }

    return false;
  }

  reader.hasBook = true;

  loadChapterTable(
      reader.bookFile,
      reader.book,
      reader.chapterStarts,
      ReaderState::MAX_CHAPTERS,
      reader.chapterCount);

  reader.wpm = preferences.getUShort("wpm", reader.book.defaultWpm);

  if (reader.wpm < MIN_WPM || reader.wpm > MAX_WPM)
  {
    reader.wpm = reader.book.defaultWpm;
  }

  const uint32_t payloadStart = getPayloadOffset(reader.book);

  if (preferences.getUInt("book", 0) == reader.book.bookId)
  {
    reader.currentIndex = preferences.getUInt("index", 0);
    reader.currentOffset = preferences.getUInt("offset", payloadStart);

    if (reader.currentIndex >= reader.book.wordCount ||
        reader.currentOffset < payloadStart ||
        !readerLoadCurrentWord(reader))
    {
      readerResetToFirstWord(reader);
    }
    else
    {
      updateCurrentChapter(reader);
    }
  }
  else
  {
    readerResetToFirstWord(reader);
  }

  return true;
}

void readerClearBook(ReaderState &reader)
{
  reader.hasBook = false;
  reader.isReading = false;
  reader.reachedEnd = false;
  reader.chapterCount = 1;
  reader.currentChapter = 0;
  reader.chapterStartWord = 0;
  reader.chapterEndWord = 0;
  memset(&reader.book, 0, sizeof(reader.book));
  memset(reader.currentWord, 0, sizeof(reader.currentWord));
  memset(reader.chapterStarts, 0, sizeof(reader.chapterStarts));
}

uint32_t readerDelayFor(const ReaderState &reader, const char *word)
{
  float multiplier = 1.0f;
  size_t length = 0;

  for (size_t offset = 0; word[offset] != '\0';)
  {
    const uint8_t byte = static_cast<uint8_t>(word[offset]);

    if ((byte & 0x80) == 0)
    {
      offset += 1;
    }
    else if ((byte & 0xE0) == 0xC0)
    {
      offset += 2;
    }
    else if ((byte & 0xF0) == 0xE0)
    {
      offset += 3;
    }
    else if ((byte & 0xF8) == 0xF0)
    {
      offset += 4;
    }
    else
    {
      offset += 1;
    }

    ++length;
  }

  if (length >= 9)
  {
    multiplier += 0.10f;
  }
  else if (length >= 7)
  {
    multiplier += 0.05f;
  }

  const size_t byteLength = strlen(word);
  const char last = byteLength ? word[byteLength - 1] : '\0';

  if (last == ',' || last == ';' || last == ':')
  {
    multiplier += 0.15f;
  }
  else if (last == '.' || last == '!' || last == '?')
  {
    multiplier += 0.35f;
  }

  const float targetMs = (60000.0f / reader.wpm) * multiplier;
  const int32_t netMs = static_cast<int32_t>(targetMs) - 15;
  return static_cast<uint32_t>(netMs > 15 ? netMs : 15);
}

void readerSetReading(
    ReaderState &reader,
    Preferences &preferences,
    bool reading)
{
  reader.isReading = reading;

  if (reader.isReading)
  {
    reader.nextWordAt = millis() + readerDelayFor(reader, reader.currentWord);
  }
  else
  {
    readerBookmark(reader, preferences);
  }
}

bool readerTick(ReaderState &reader, Preferences &preferences, uint32_t now)
{
  if (!reader.isReading ||
      static_cast<int32_t>(now - reader.nextWordAt) < 0)
  {
    return false;
  }

  if (reader.currentIndex + 1 >= reader.book.wordCount)
  {
    reader.reachedEnd = true;
    readerSetReading(reader, preferences, false);
    return true;
  }

  ++reader.currentIndex;
  updateCurrentChapter(reader);
  reader.currentOffset = reader.nextOffset;

  if (!readerLoadCurrentWord(reader))
  {
    reader.reachedEnd = true;
    readerSetReading(reader, preferences, false);
    return true;
  }

  reader.nextWordAt = now + readerDelayFor(reader, reader.currentWord);

  if (reader.currentIndex % CHECKPOINT_INTERVAL == 0)
  {
    readerBookmark(reader, preferences);
  }

  return true;
}
