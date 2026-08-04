#pragma once

#include <Arduino.h>
#include <FS.h>

#include "book_format.h"

bool validHeader(const BookHeader &header, size_t fileSize);
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length);
bool validateBookFile(const char *path, BookHeader &validatedHeader);
bool installUploadedBook(BookHeader &installedHeader);
bool readWordAt(File &file, uint32_t offset, char *out, uint32_t &after);
