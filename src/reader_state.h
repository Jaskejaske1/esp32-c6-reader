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

  uint16_t chapterCount = 1;
  uint16_t currentChapter = 0;
  uint32_t chapterStartWord = 0;
  uint32_t chapterEndWord = 0;
  static constexpr uint16_t MAX_CHAPTERS = 256;
  uint32_t chapterStarts[MAX_CHAPTERS] = {};

  char currentWord[WORD_BUFFER_SIZE] = {};
};
