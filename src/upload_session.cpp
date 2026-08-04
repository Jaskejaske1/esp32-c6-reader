#include "upload_session.h"

#include <LittleFS.h>

#include "book_format.h"
#include "config.h"

const char *UploadSession::status() const
{
  return statusText;
}

bool UploadSession::begin(uint32_t expected)
{
  if (expected < sizeof(BookHeader) || expected > MAX_UPLOAD_BYTES)
  {
    abort("invalid size");
    return false;
  }

  if (file)
  {
    file.close();
  }

  LittleFS.remove(UPLOAD_PATH);

  const size_t totalBytes = LittleFS.totalBytes();
  const size_t usedBytes = LittleFS.usedBytes();
  const size_t availableBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

  if (expected + UPLOAD_SPACE_MARGIN_BYTES > availableBytes)
  {
    abort("not enough space");
    return false;
  }

  file = LittleFS.open(UPLOAD_PATH, "w");

  if (!file)
  {
    abort("could not open upload file");
    return false;
  }

  active = true;
  expectedBytes = expected;
  writtenBytes = 0;
  setBusyStatus();
  return true;
}

bool UploadSession::append(const uint8_t *data, uint16_t length)
{
  if (!active || !file)
  {
    abort("data without begin");
    return false;
  }

  if (writtenBytes + length > expectedBytes)
  {
    abort("too many bytes");
    return false;
  }

  if (file.write(data, length) != length)
  {
    abort("write failed");
    return false;
  }

  writtenBytes += length;
  setBusyStatus();
  return true;
}

bool UploadSession::finishWrites()
{
  if (!active || !file)
  {
    abort("end without begin");
    return false;
  }

  file.close();
  active = false;

  if (writtenBytes != expectedBytes)
  {
    abort("size mismatch");
    return false;
  }

  expectedBytes = 0;
  writtenBytes = 0;
  return true;
}

void UploadSession::abort(const char *reason)
{
  if (file)
  {
    file.close();
  }

  active = false;
  expectedBytes = 0;
  writtenBytes = 0;
  LittleFS.remove(UPLOAD_PATH);

  char status[80];
  snprintf(status, sizeof(status), "ERR:%s", reason);
  setStatus(status);
}

void UploadSession::setStatus(const char *status)
{
  snprintf(statusText, sizeof(statusText), "%s", status);
}

void UploadSession::setBusyStatus()
{
  snprintf(
      statusText,
      sizeof(statusText),
      "BUSY:%lu/%lu",
      static_cast<unsigned long>(writtenBytes),
      static_cast<unsigned long>(expectedBytes));
}
