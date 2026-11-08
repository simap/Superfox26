// Line console on USB Serial/JTAG, for a terminal or a Web Serial page.
//
// One command per line, CR or LF terminated, echoed as typed. Every command answers with one
// or more lines that start with "OK" or one line that starts with "ERR". Log output shares
// the port.
//
//   help
//   status                  WiFi, IP, page list source, cycle state, heap
//   wifi                    the stored SSID
//   wifi SSID [PASSWORD]    store in NVS and connect. Quote an SSID or password with spaces.
//   scan                    list the 2.4 GHz networks in range, about 3 s
//   get URL                 fetch up to 64 KB and print status, size, timing, first 200 bytes
//   urls                    the list, numbered from 1: URL, scroll offset, seconds
//   url add URL [OFFSET [SECONDS]]   offset in document pixels, seconds 0 = the Kconfig default
//   url set N OFFSET [SECONDS]
//   url del N
//   url clear               remove the file, back to the built-in list
//   next                    render the next page now
#pragma once

// Installs the USB Serial/JTAG driver on stdin and stdout and starts the console task.
void console_start();
