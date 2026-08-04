#include "ble_transport.h"
#include "status_text.h"

void BleTransport::begin(const SerialTransportCallbacks &callbacks)
{
  callbacks_ = callbacks;

  BLEDevice::init("RSVP-Reader");
  pServer_ = BLEDevice::createServer();
  pServer_->setCallbacks(this);

  BLEService *pService = pServer_->createService(SERVICE_UUID);

  pTxCharacteristic_ = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);

  pRxCharacteristic_ = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxCharacteristic_->setCallbacks(this);

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE transport active (RSVP-Reader)");
}

void BleTransport::onConnect(BLEServer *)
{
  deviceConnected_ = true;
}

void BleTransport::onDisconnect(BLEServer *)
{
  deviceConnected_ = false;
}

void BleTransport::onWrite(BLECharacteristic *characteristic)
{
  String rxValue = characteristic->getValue();
  if (rxValue.length() == 0)
  {
    return;
  }

  for (size_t i = 0; i < rxValue.length(); ++i)
  {
    char c = rxValue[i];
    if (c == '\r') continue;
    if (c == '\n')
    {
      line_[lineLength_] = '\0';
      if (lineLength_ > 0)
      {
        handleLine(line_);
      }
      lineLength_ = 0;
    }
    else if (lineLength_ + 1 < sizeof(line_))
    {
      line_[lineLength_++] = c;
    }
    else
    {
      lineLength_ = 0;
    }
  }
}

void BleTransport::handleLine(char *line)
{
  if (strcmp(line, "STATUS") == 0)
  {
    sendStatusFrame();
    return;
  }

  if (callbacks_.handleControlCommand && callbacks_.handleControlCommand(line))
  {
    sendStatusFrame();
    return;
  }

  sendErrorFrame("unknown command");
}

void BleTransport::notifyString(const char *str)
{
  if (deviceConnected_ && pTxCharacteristic_)
  {
    pTxCharacteristic_->setValue(const_cast<char *>(str));
    pTxCharacteristic_->notify();
    delay(5);
  }
}

void BleTransport::sendStatusFrame()
{
  if (!deviceConnected_ || !callbacks_.reader)
  {
    return;
  }

  char metadata[128];
  char state[80];
  char upload[96];
  composeMetadata(metadata, sizeof(metadata), *callbacks_.reader);
  composeState(state, sizeof(state), *callbacks_.reader);
  snprintf(upload, sizeof(upload), "UPLOAD %s", callbacks_.uploadStatus ? callbacks_.uploadStatus() : "idle");

  notifyString("RSVP/1\n");
  notifyString(metadata);
  notifyString("\n");
  notifyString(state);
  notifyString("\n");
  notifyString(upload);
  notifyString("\n.\n");
}

void BleTransport::sendErrorFrame(const char *reason)
{
  if (!deviceConnected_)
  {
    return;
  }

  char frame[64];
  snprintf(frame, sizeof(frame), "RSVP/1\nERR %s\n.\n", reason);
  notifyString(frame);
}

void BleTransport::tick(uint32_t)
{
  if (!deviceConnected_ && oldDeviceConnected_)
  {
    delay(500);
    pServer_->startAdvertising();
    Serial.println("BLE re-advertising...");
    oldDeviceConnected_ = deviceConnected_;
  }
  if (deviceConnected_ && !oldDeviceConnected_)
  {
    oldDeviceConnected_ = deviceConnected_;
    Serial.println("BLE client connected");
  }
}
