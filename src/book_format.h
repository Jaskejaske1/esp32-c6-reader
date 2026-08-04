#pragma once

#include <Arduino.h>

struct __attribute__((packed)) BookHeader
{
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint32_t bookId;
  uint32_t wordCount;
  uint32_t payloadBytes;
  uint16_t defaultWpm;
  uint16_t chapterCount;
  char title[48];
  char author[32];
  uint32_t payloadCrc32;
};

static_assert(sizeof(BookHeader) == 108, "Book header format changed");
