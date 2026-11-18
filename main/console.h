// Serial command console over USB serial JTAG.
// A connected host sets WiFi credentials, edits the URL page list, triggers fetches and page
// changes, and reads device status, so a badge can be configured and inspected over serial.
// console_start installs the USB serial JTAG driver and starts a background task that owns the
// console.
// Call it once.
#pragma once

void console_start();
