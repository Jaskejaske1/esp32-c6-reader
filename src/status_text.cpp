#include "status_text.h"

#include <Arduino.h>

void composeMetadata(char *metadata, size_t size, const ReaderState &reader)
{
  if (!reader.hasBook)
  {
    snprintf(metadata, size, "META v1|NO_BOOK");
    return;
  }

  snprintf(
      metadata,
      size,
      "META v1|%s|%s|%lu",
      reader.book.title,
      reader.book.author,
      static_cast<unsigned long>(reader.book.wordCount));
}

void composeState(char *state, size_t size, const ReaderState &reader)
{
  snprintf(
      state,
      size,
      "STATE playing=%u;wpm=%u;word=%lu;total=%lu",
      reader.isReading ? 1 : 0,
      reader.wpm,
      static_cast<unsigned long>(reader.hasBook ? reader.currentIndex + 1 : 0),
      static_cast<unsigned long>(reader.hasBook ? reader.book.wordCount : 0));
}

void composeStatus(
    char *status,
    size_t size,
    const ReaderState &reader,
    const char *uploadStatus)
{
  char metadata[128];
  char state[80];
  composeMetadata(metadata, sizeof(metadata), reader);
  composeState(state, sizeof(state), reader);

  snprintf(
      status,
      size,
      "%s\n%s\nUPLOAD %s",
      metadata,
      state,
      uploadStatus);
}
