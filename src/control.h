#pragma once

#include <Preferences.h>

#include "reader_state.h"

bool toggleReader(ReaderState &reader, Preferences &preferences);
bool increaseReaderWpm(ReaderState &reader, Preferences &preferences);
bool decreaseReaderWpm(ReaderState &reader, Preferences &preferences);
bool applyControlCommand(
    ReaderState &reader,
    Preferences &preferences,
    const char *command);
