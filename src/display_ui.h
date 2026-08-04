#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <U8g2lib.h>

#include "reader_state.h"

enum class UiMode
{
  Reader,
  UsbReady
};

void renderDisplay(
    U8G2 &display,
    UiMode mode,
    const ReaderState &reader,
    Preferences &preferences);
