#include "serial_transport.h"

#include <stdlib.h>
#include <string.h>

#include "status_text.h"

bool SerialTransport::ready() const
{
  return stream_ &&
         callbacks_.reader &&
         callbacks_.preferences &&
         callbacks_.uploadSession &&
         callbacks_.stopReading &&
         callbacks_.activateBook &&
         callbacks_.handleControlCommand &&
         callbacks_.render &&
         callbacks_.uploadStatus &&
         callbacks_.setUploadStatus;
}

void SerialTransport::begin(Stream &stream, const SerialTransportCallbacks &callbacks)
{
  stream_ = &stream;
  callbacks_ = callbacks;
}

void SerialTransport::tick(uint32_t now)
{
  if (!ready())
  {
    return;
  }

  if (uploadActive_)
  {
    handleUploadBytes(now);

    if (uploadActive_ && now - lastUploadByteAt_ > UPLOAD_TIMEOUT_MS)
    {
      abortUpload("upload timeout");
    }

    return;
  }

  if (drainAfterAbort_)
  {
    drainInput();

    if (now < drainUntil_)
    {
      return;
    }

    drainAfterAbort_ = false;
    lineLength_ = 0;
  }

  while (stream_->available() > 0 && !uploadActive_)
  {
    handleByte(static_cast<uint8_t>(stream_->read()), now);
  }
}

void SerialTransport::handleByte(uint8_t value, uint32_t)
{
  if (value == '\r')
  {
    return;
  }

  if (value == '\n')
  {
    line_[lineLength_] = '\0';

    if (lineLength_ > 0)
    {
      handleLine(line_);
    }

    lineLength_ = 0;
    return;
  }

  if (lineLength_ + 1 >= LINE_BUFFER_SIZE)
  {
    lineLength_ = 0;
    sendErrorFrame("line too long");
    return;
  }

  line_[lineLength_++] = static_cast<char>(value);
}

void SerialTransport::handleLine(char *line)
{
  if (strcmp(line, "STATUS") == 0)
  {
    sendStatusFrame();
    return;
  }

  if (strncmp(line, "UPLOAD ", 7) == 0)
  {
    const uint32_t bytes = strtoul(line + 7, nullptr, 10);
    beginUpload(bytes);
    return;
  }

  if (callbacks_.handleControlCommand(line))
  {
    sendStatusFrame();
    return;
  }

  sendErrorFrame("unknown command");
}

void SerialTransport::beginUpload(uint32_t bytes)
{
  if (!callbacks_.uploadSession->begin(bytes))
  {
    callbacks_.setUploadStatus(callbacks_.uploadSession->status());
    sendErrorFrame(callbacks_.uploadSession->status());
    return;
  }

  callbacks_.stopReading();
  callbacks_.setUploadStatus(callbacks_.uploadSession->status());
  uploadActive_ = true;
  uploadRemaining_ = bytes;
  lastUploadByteAt_ = millis();

  stream_->println("RSVP/1");
  stream_->println("UPLOAD READY");
  stream_->println(".");
}

void SerialTransport::handleUploadBytes(uint32_t now)
{
  uint8_t buffer[128];

  while (uploadActive_ && uploadRemaining_ > 0 && stream_->available() > 0)
  {
    const uint16_t wanted = min<uint32_t>(
        sizeof(buffer),
        min<uint32_t>(uploadRemaining_, stream_->available()));
    const size_t read = stream_->readBytes(buffer, wanted);

    if (read == 0)
    {
      return;
    }

    lastUploadByteAt_ = millis();

    if (!callbacks_.uploadSession->append(buffer, static_cast<uint16_t>(read)))
    {
      uploadActive_ = false;
      uploadRemaining_ = 0;
      callbacks_.setUploadStatus(callbacks_.uploadSession->status());
      sendErrorFrame(callbacks_.uploadSession->status());
      return;
    }

    uploadRemaining_ -= static_cast<uint32_t>(read);
    callbacks_.setUploadStatus(callbacks_.uploadSession->status());
  }

  if (uploadActive_ && uploadRemaining_ == 0)
  {
    finishUpload();
  }
}

void SerialTransport::finishUpload()
{
  uploadActive_ = false;

  if (!callbacks_.uploadSession->finishWrites())
  {
    callbacks_.setUploadStatus(callbacks_.uploadSession->status());
    sendErrorFrame(callbacks_.uploadSession->status());
    return;
  }

  if (!callbacks_.activateBook())
  {
    callbacks_.setUploadStatus("ERR:validation_or_activation_failed");
    sendErrorFrame("validation_or_activation_failed");
    return;
  }

  callbacks_.setUploadStatus("OK:activated");
  sendStatusFrame();
  callbacks_.setUploadStatus("idle");
}

void SerialTransport::abortUpload(const char *reason)
{
  uploadActive_ = false;
  uploadRemaining_ = 0;
  drainAfterAbort_ = true;
  drainUntil_ = millis() + ABORT_DRAIN_MS;
  lineLength_ = 0;
  callbacks_.uploadSession->abort(reason);
  callbacks_.setUploadStatus(callbacks_.uploadSession->status());
  sendErrorFrame(reason);
}

void SerialTransport::drainInput()
{
  while (stream_->available() > 0)
  {
    stream_->read();
  }
}

void SerialTransport::publishState()
{
  sendStatusFrame();
}

void SerialTransport::sendStatusFrame()
{
  if (!ready())
  {
    return;
  }

  char metadata[128];
  char state[80];
  char upload[96];
  composeMetadata(metadata, sizeof(metadata), *callbacks_.reader);
  composeState(state, sizeof(state), *callbacks_.reader);
  snprintf(upload, sizeof(upload), "UPLOAD %s", callbacks_.uploadStatus());

  stream_->println("RSVP/1");
  sendFrameLine(metadata);
  sendFrameLine(state);
  sendFrameLine(upload);
  stream_->println(".");
}

void SerialTransport::sendErrorFrame(const char *reason)
{
  if (!stream_)
  {
    return;
  }

  stream_->println("RSVP/1");
  stream_->print("ERR ");
  stream_->println(reason);
  stream_->println(".");
}

void SerialTransport::sendFrameLine(const char *line)
{
  stream_->println(line);
}
