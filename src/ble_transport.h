#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "serial_transport.h"

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class BleTransport : public BLEServerCallbacks, public BLECharacteristicCallbacks
{
public:
  void begin(const SerialTransportCallbacks &callbacks);
  void tick(uint32_t now);
  void sendStatusFrame();
  void sendErrorFrame(const char *reason);

  void onConnect(BLEServer *server) override;
  void onDisconnect(BLEServer *server) override;
  void onWrite(BLECharacteristic *characteristic) override;

private:
  SerialTransportCallbacks callbacks_{};
  BLEServer *pServer_ = nullptr;
  BLECharacteristic *pTxCharacteristic_ = nullptr;
  BLECharacteristic *pRxCharacteristic_ = nullptr;
  bool deviceConnected_ = false;
  bool oldDeviceConnected_ = false;

  char line_[80] = {};
  uint16_t lineLength_ = 0;

  void handleLine(char *line);
  void notifyString(const char *str);
};
