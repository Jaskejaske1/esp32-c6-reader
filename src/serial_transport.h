#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "reader_state.h"
#include "upload_session.h"

struct SerialTransportCallbacks
{
  ReaderState *reader;
  Preferences *preferences;
  UploadSession *uploadSession;
  void (*stopReading)();
  bool (*activateBook)();
  bool (*handleControlCommand)(const char *command);
  void (*render)();
  const char *(*uploadStatus)();
  void (*setUploadStatus)(const char *status);
};

class SerialTransport
{
public:
  void begin(Stream &stream, const SerialTransportCallbacks &callbacks);
  void tick(uint32_t now);
  void publishState();
  void sendStatusFrame();
  void sendErrorFrame(const char *reason);

private:
  static constexpr uint16_t LINE_BUFFER_SIZE = 80;
  static constexpr uint16_t MAX_UPLOAD_CHUNK_BYTES = 240;
  static constexpr uint32_t UPLOAD_TIMEOUT_MS = 120000;
  static constexpr uint32_t ABORT_DRAIN_MS = 3000;

  Stream *stream_ = nullptr;
  SerialTransportCallbacks callbacks_{};
  char line_[LINE_BUFFER_SIZE] = {};
  uint16_t lineLength_ = 0;
  bool uploadActive_ = false;
  bool drainAfterAbort_ = false;
  bool uploadAckMode_ = false;
  uint16_t uploadChunkSize_ = 0;
  uint32_t uploadRemaining_ = 0;
  uint32_t uploadExpectedBytes_ = 0;
  uint32_t lastUploadByteAt_ = 0;
  uint32_t drainUntil_ = 0;

  bool ready() const;
  void handleByte(uint8_t value, uint32_t now);
  void handleLine(char *line);
  void beginUpload(uint32_t bytes, uint16_t chunkSize);
  void handleUploadBytes(uint32_t now);
  void finishUpload();
  void abortUpload(const char *reason);
  void drainInput();
  void sendUploadContinueFrame();
  void sendFrameLine(const char *line);
};
