#pragma once

#include <stddef.h>

#include "reader_state.h"

void composeMetadata(char *metadata, size_t size, const ReaderState &reader);
void composeState(char *state, size_t size, const ReaderState &reader);
void composeStatus(
    char *status,
    size_t size,
    const ReaderState &reader,
    const char *uploadStatus);
