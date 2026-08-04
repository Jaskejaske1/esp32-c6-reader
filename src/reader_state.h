#pragma once

#include <Arduino.h>
#include <FS.h>

#include "book_format.h"
#include "config.h"

struct ReaderState
{
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
};
