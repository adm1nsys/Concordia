/*
 * Unpacking a ZIP onto the SD card.
 *
 * The deflate decoder is not ours and costs no flash: tinfl lives in the
 * ESP32-C6's ROM, mapped by the linker script, so calling it is free. All that
 * is written here is the ZIP container around it.
 *
 * Both directions stream. The archive is read from the card a few kilobytes at
 * a time and written back the same way, so the only real memory cost is the
 * 32 KB LZ window deflate requires, taken from the heap for the duration and
 * given straight back. That means the size of the archive does not matter.
 *
 * Entries are read through the central directory at the end of the file rather
 * than by walking local headers from the front. Both are legal; only the former
 * is trustworthy, because archives written by a streaming zipper leave the sizes
 * in the local headers set to zero and put the real ones after the data.
 */
#pragma once

#include <Arduino.h>

namespace Zip {

struct Result {
  bool ok = false;
  uint16_t files = 0;
  uint32_t bytes = 0;
  String message;
};

/* Extracts every file in `archivePath` into `intoDir`, creating the
 * subdirectories the archive names. Nothing is written outside `intoDir`. */
Result extract(const String &archivePath, const String &intoDir);

} // namespace Zip
