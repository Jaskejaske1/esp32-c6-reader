#pragma once

#include <Arduino.h>
#include <FS.h>

class UploadSession
{
public:
  const char *status() const;
  bool begin(uint32_t expectedBytes);
  bool append(const uint8_t *data, uint16_t length);
  bool finishWrites();
  void abort(const char *reason);

private:
  File file;
  bool active = false;
  uint32_t expectedBytes = 0;
  uint32_t writtenBytes = 0;
  char statusText[80] = "idle";

  void setStatus(const char *status);
  void setBusyStatus();
};
