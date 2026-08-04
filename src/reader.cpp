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

void readerResetToFirstWord(ReaderState &reader)
{
  reader.currentIndex = 0;
  reader.currentOffset = sizeof(BookHeader);
  reader.reachedEnd = false;
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

  uint32_t offset = sizeof(BookHeader);
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

  reader.wpm = preferences.getUShort("wpm", reader.book.defaultWpm);

  if (reader.wpm < MIN_WPM || reader.wpm > MAX_WPM)
  {
    reader.wpm = reader.book.defaultWpm;
  }

  if (preferences.getUInt("book", 0) == reader.book.bookId)
  {
    reader.currentIndex = preferences.getUInt("index", 0);
    reader.currentOffset = preferences.getUInt("offset", sizeof(BookHeader));

    if (reader.currentIndex >= reader.book.wordCount ||
        !readerLoadCurrentWord(reader))
    {
      readerResetToFirstWord(reader);
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
  memset(&reader.book, 0, sizeof(reader.book));
  memset(reader.currentWord, 0, sizeof(reader.currentWord));
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
    multiplier += 0.20f;
  }
  else if (length >= 7)
  {
    multiplier += 0.10f;
  }

  const size_t byteLength = strlen(word);
  const char last = byteLength ? word[byteLength - 1] : '\0';

  if (last == ',' || last == ';' || last == ':')
  {
    multiplier += 0.35f;
  }

  if (last == '.' || last == '!' || last == '?')
  {
    multiplier += 0.85f;
  }

  return static_cast<uint32_t>(60000.0f / reader.wpm * multiplier);
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
