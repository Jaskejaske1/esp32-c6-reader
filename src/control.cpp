#include "control.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "reader.h"

bool toggleReader(ReaderState &reader, Preferences &preferences)
{
  if (!reader.hasBook)
  {
    return false;
  }

  if (!reader.isReading && reader.reachedEnd)
  {
    readerResetToFirstWord(reader);
  }

  readerSetReading(reader, preferences, !reader.isReading);
  return true;
}

bool increaseReaderWpm(ReaderState &reader, Preferences &preferences)
{
  if (reader.wpm >= MAX_WPM)
  {
    return false;
  }

  reader.wpm = min<uint16_t>(MAX_WPM, reader.wpm + WPM_STEP);
  readerBookmark(reader, preferences);
  return true;
}

bool decreaseReaderWpm(ReaderState &reader, Preferences &preferences)
{
  if (reader.wpm <= MIN_WPM)
  {
    return false;
  }

  reader.wpm = max<uint16_t>(MIN_WPM, reader.wpm - WPM_STEP);
  readerBookmark(reader, preferences);
  return true;
}

bool applyControlCommand(
    ReaderState &reader,
    Preferences &preferences,
    const char *command)
{
  if (strcmp(command, "PLAY") == 0 && reader.hasBook)
  {
    if (reader.reachedEnd)
    {
      readerResetToFirstWord(reader);
    }
    readerSetReading(reader, preferences, true);
    return true;
  }

  if (strcmp(command, "PAUSE") == 0)
  {
    readerSetReading(reader, preferences, false);
    return true;
  }

  if (strcmp(command, "WPM:+25") == 0)
  {
    return increaseReaderWpm(reader, preferences);
  }

  if (strcmp(command, "WPM:-25") == 0)
  {
    return decreaseReaderWpm(reader, preferences);
  }

  if (strncmp(command, "WPM:", 4) == 0)
  {
    const long requested = strtol(command + 4, nullptr, 10);

    if (requested >= MIN_WPM && requested <= MAX_WPM)
    {
      reader.wpm = static_cast<uint16_t>(requested);
      readerBookmark(reader, preferences);
      return true;
    }
    return false;
  }

  return false;
}
