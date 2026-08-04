#include "buttons.h"

Button::Button(uint8_t pin) : pin_(pin)
{
}

void Button::update()
{
  fell_ = false;
  rose_ = false;

  const bool raw = digitalRead(pin_);
  const uint32_t now = millis();

  if (raw != rawPrevious_)
  {
    rawPrevious_ = raw;
    changedAt_ = now;
  }

  if (now - changedAt_ >= 30 && raw != stable_)
  {
    stable_ = raw;
    fell_ = stable_ == LOW;
    rose_ = stable_ == HIGH;
  }
}

bool Button::pressed() const
{
  return fell_;
}

bool Button::released() const
{
  return rose_;
}

bool Button::isDown() const
{
  return stable_ == LOW;
}
