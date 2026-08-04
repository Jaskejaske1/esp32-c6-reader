#pragma once

#include <Arduino.h>

class Button
{
public:
  explicit Button(uint8_t pin);

  void update();
  bool pressed() const;
  bool released() const;
  bool isDown() const;

private:
  uint8_t pin_;
  bool stable_ = HIGH;
  bool rawPrevious_ = HIGH;
  bool fell_ = false;
  bool rose_ = false;
  uint32_t changedAt_ = 0;
};
