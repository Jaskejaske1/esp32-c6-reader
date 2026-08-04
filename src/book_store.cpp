#include "book_store.h"

#include <LittleFS.h>

#include "config.h"

uint32_t getPayloadOffset(const BookHeader &header)
{
  const uint32_t chapterTableBytes = (header.version == 2 && header.chapterCount > 0)
                                          ? (static_cast<uint32_t>(header.chapterCount) * sizeof(uint32_t))
                                          : 0;
  return sizeof(BookHeader) + chapterTableBytes;
}

bool validHeader(const BookHeader &header, size_t fileSize)
{
  return memcmp(header.magic, "RSVP", 4) == 0 &&
         (header.version == 1 || header.version == 2) &&
         header.headerSize == sizeof(BookHeader) &&
         header.wordCount > 0 &&
         header.defaultWpm >= MIN_WPM &&
         header.defaultWpm <= MAX_WPM &&
         fileSize >= getPayloadOffset(header) + header.payloadBytes;
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t length)
{
  crc = ~crc;

  for (size_t i = 0; i < length; ++i)
  {
    crc ^= data[i];

    for (uint8_t bit = 0; bit < 8; ++bit)
    {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1U)));
    }
  }

  return ~crc;
}

bool validateBookFile(const char *path, BookHeader &validatedHeader)
{
  File file = LittleFS.open(path, "r");

  if (!file)
  {
    return false;
  }

  if (file.size() < sizeof(BookHeader) ||
      file.readBytes(
          reinterpret_cast<char *>(&validatedHeader),
          sizeof(validatedHeader)) != sizeof(validatedHeader) ||
      !validHeader(validatedHeader, file.size()))
  {
    file.close();
    return false;
  }

  const uint32_t payloadStart = getPayloadOffset(validatedHeader);
  if (!file.seek(payloadStart, SeekSet))
  {
    file.close();
    return false;
  }

  uint32_t crc = 0;
  uint32_t words = 0;
  uint32_t payloadRead = 0;

  while (payloadRead < validatedHeader.payloadBytes)
  {
    const int length = file.read();

    if (length <= 0 ||
        length >= WORD_BUFFER_SIZE ||
        payloadRead + 1U + static_cast<uint32_t>(length) >
            validatedHeader.payloadBytes)
    {
      file.close();
      return false;
    }

    uint8_t buffer[WORD_BUFFER_SIZE] = {};

    if (file.read(buffer, length) != length)
    {
      file.close();
      return false;
    }

    const uint8_t lengthByte = static_cast<uint8_t>(length);
    crc = crc32Update(crc, &lengthByte, 1);
    crc = crc32Update(crc, buffer, length);
    payloadRead += 1U + static_cast<uint32_t>(length);
    ++words;
  }

  const bool valid = words == validatedHeader.wordCount &&
                     crc == validatedHeader.payloadCrc32 &&
                     file.position() == file.size();

  file.close();
  return valid;
}

bool loadChapterTable(
    File &file,
    const BookHeader &header,
    uint32_t *tableOut,
    uint16_t maxChapters,
    uint16_t &loadedCount)
{
  if (header.version == 2 && header.chapterCount > 0)
  {
    const uint16_t count = min(header.chapterCount, maxChapters);
    if (!file.seek(sizeof(BookHeader), SeekSet))
    {
      loadedCount = 1;
      tableOut[0] = 0;
      return false;
    }

    const size_t bytesToRead = count * sizeof(uint32_t);
    if (file.readBytes(reinterpret_cast<char *>(tableOut), bytesToRead) == bytesToRead)
    {
      loadedCount = count;
      return true;
    }
  }

  loadedCount = 1;
  tableOut[0] = 0;
  return true;
}

bool installUploadedBook(BookHeader &installedHeader)
{
  if (!validateBookFile(UPLOAD_PATH, installedHeader))
  {
    LittleFS.remove(UPLOAD_PATH);
    return false;
  }

  LittleFS.remove(BACKUP_PATH);

  if (LittleFS.exists(BOOK_PATH) && !LittleFS.rename(BOOK_PATH, BACKUP_PATH))
  {
    LittleFS.remove(UPLOAD_PATH);
    return false;
  }

  if (!LittleFS.rename(UPLOAD_PATH, BOOK_PATH))
  {
    if (LittleFS.exists(BACKUP_PATH))
    {
      LittleFS.rename(BACKUP_PATH, BOOK_PATH);
    }

    LittleFS.remove(UPLOAD_PATH);
    return false;
  }

  LittleFS.remove(BACKUP_PATH);
  return true;
}

bool readWordAt(File &file, uint32_t offset, char *out, uint32_t &after)
{
  if (!file.seek(offset, SeekSet))
  {
    return false;
  }

  const int length = file.read();

  if (length <= 0 ||
      length >= WORD_BUFFER_SIZE ||
      offset + 1U + length > file.size())
  {
    return false;
  }

  if (file.readBytes(out, length) != static_cast<size_t>(length))
  {
    return false;
  }

  out[length] = '\0';
  after = offset + 1U + length;
  return true;
}
