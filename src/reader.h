#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "reader_state.h"

bool readerLoadCurrentWord(ReaderState &reader);
void readerBookmark(ReaderState &reader, Preferences &preferences);
void readerResetToFirstWord(ReaderState &reader);
bool readerSeekToWord(ReaderState &reader, Preferences &preferences, uint32_t index);
bool readerOpenBook(ReaderState &reader, Preferences &preferences);
void readerClearBook(ReaderState &reader);
uint32_t readerDelayFor(const ReaderState &reader, const char *word);
void readerSetReading(ReaderState &reader, Preferences &preferences, bool reading);
bool readerTick(ReaderState &reader, Preferences &preferences, uint32_t now);
