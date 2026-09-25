# Build notes - real problems and how they were fixed

## Firmware (ESP32-S3-BOX-3, ESP-IDF + LVGL)

**Icons that drew nothing.** A 40x40 weather icon rendered as zero pixels
while every 24x24 icon worked. `lv_snapshot_take()` - an independent
re-render through LVGL's real draw pipeline - proved the draw produced no
pixels (not a layout or visibility issue), and the file on flash was
byte-identical to the source. Shrinking the native asset to 24 px fixed it;
larger icons are now 24 px assets scaled up.

**Logos that disappeared at random.** PNGs decoded at runtime from flash
need a large contiguous heap allocation, and after hours of uptime the heap
was too fragmented. All logos and icons are now converted at build time into
compiled-in C image descriptors (`tools/convert_*.py`), so nothing is decoded
at runtime.

**"Error create mqtt task".** The default MQTT task stack was almost the
entire free internal RAM in one contiguous request. Stack sized down to what
the small JSON payloads need; LVGL's heap moved to PSRAM.

**Messages silently dropped.** A 1,219-byte aircraft payload exceeded the
MQTT client's default 1 KB receive buffer and arrived in fragments that
failed to parse. Buffer raised to 2 KB; fragmented messages are now logged
instead of dropped silently.

**Chime but no picture.** The special-aircraft chime and the screen update
are two paths off the same MQTT message, and the screen update could lose a
race for the display lock and be skipped. Fixed so rendering can't be
dropped.

**The serial monitor that reset the board.** The BOX-3 uses the ESP32-S3's
native USB-Serial/JTAG, which watches the DTR/RTS lines like auto-reset
boards do - just opening the port could reset it, and once froze the live
firmware. Verification moved to the MQTT broker's log instead of serial.

**Crowded screen.** Adding a 3-day forecast to the home screen made it
cramped (and it had to leave room for an all-day calendar entry on the
bottom bar). Options were rendered as to-scale mockups; the owner chose a
dedicated Weather page, and the home screen got a single line that only
appears when weather is imminent.

## Backend (n8n on a Raspberry Pi)

**An apostrophe took down the aircraft pipeline.** An aircraft owner named
"L'EAGLE AIR" broke SQL that pasted values into the query text. Every cache
write moved to Postgres query parameters, verified by round-tripping a
string full of quotes, `$` signs, backslashes, emoji and an injection attempt
byte-for-byte.

**Weather that disagreed with the window.** A gridded model can't place
Miami's small pop-up showers; the nearest airport station is miles away; a
radar-image service was 10-15 minutes behind. The fix was to fuse sources by
what each is good at: NOAA MRMS radar at the house's 1 km cell for rain
(decoded from GRIB2 every 20 s, pushed on change), the airport METAR for
cloud cover, the model for temperature and forecast. Every decision is
logged with its inputs so real rainy days can be audited.

**Alert storms.** One broker outage produced 93 Telegram messages in
minutes. The shared error handler now sends one message per incident per
workflow (30 quiet minutes before the next), and ignores a single
"task request timed out" - which is what an n8n restart does to an in-flight
step - unless it repeats.

**Edits that didn't take effect.** On this instance, changing an active
workflow through the API didn't reload it until the container restarted.
Every change is scripted, followed by a restart and a live execution check.

**One location, everywhere.** A coordinate approximation about 0.9 mi off
swung computed aircraft bearings by 27+ degrees. The calibrated location is
now shared by the receiver, the aircraft pipeline, weather and radar.
