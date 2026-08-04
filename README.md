# ESP32-C6 RSVP reader — storage milestone

This firmware reads a streaming `book.rsvp` file from LittleFS. It never loads
the whole book into RAM. A bookmark is saved in NVS while paused and every 40
words while reading.

## Build a test book and flash it

Run from the PlatformIO project root in PowerShell:

```powershell
py tools/make_book.py examples/demo.txt data/book.rsvp --title "RSVP Demo" --author "Jasper"
pio run --target uploadfs
pio run --target upload
pio device monitor
```

`uploadfs` overwrites the LittleFS partition. That is only the development
provisioning route; BLE will upload to a temporary file, validate it, then
atomically activate it.
