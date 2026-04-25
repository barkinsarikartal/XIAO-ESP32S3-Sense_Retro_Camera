#include "shared.h"
#include <cstring>

// ================= EXIF WRITER =================
// Builds a minimal EXIF APP1 segment (TIFF IFD0 only) at boot time.
// Injected between the SOI marker and the rest of every JPEG photo.
//
// EXIF structure:
//   FFD8                     <- SOI (already in camera output)
//   FFE1 [len]               <- APP1 marker + 2-byte length
//   "Exif\0\0"               <- EXIF signature
//   TIFF header (II, 0x2A, offset to IFD0)
//   IFD0 entries             <- tag/type/count/value-or-offset
//   Next IFD pointer (0)
//   String pool              <- values longer than 4 bytes
//
// No RTC on this board — DateTimeOriginal uses compile-time __DATE__ __TIME__.

// Pre-built EXIF block cached in PSRAM. Populated once by buildExifBlock().
static uint8_t *exifBlock = nullptr;
static size_t   exifBlockLen = 0;

// ── TIFF tag IDs ──
#define TAG_MAKE              0x010F
#define TAG_MODEL             0x0110
#define TAG_SOFTWARE          0x0131
#define TAG_ARTIST            0x013B
#define TAG_EXIF_IFD_POINTER  0x8769
#define TAG_DATETIME_ORIGINAL 0x9003

// TIFF data types
#define TIFF_ASCII  2
#define TIFF_LONG   4

// Helper: write a 16-bit little-endian value into buf at offset
static inline void put16(uint8_t *buf, int off, uint16_t v) {
  buf[off]     = v & 0xFF;
  buf[off + 1] = (v >> 8) & 0xFF;
}

// Helper: write a 32-bit little-endian value into buf at offset
static inline void put32(uint8_t *buf, int off, uint32_t v) {
  buf[off]     = v & 0xFF;
  buf[off + 1] = (v >> 8) & 0xFF;
  buf[off + 2] = (v >> 16) & 0xFF;
  buf[off + 3] = (v >> 24) & 0xFF;
}

// Convert __DATE__ "Apr 25 2026" + __TIME__ "11:05:35" -> "2026:04:25 11:05:35\0" (20 bytes)
static void formatExifDateTime(char *out) {
  const char *d = __DATE__;  // "Mmm dd yyyy"
  const char *t = __TIME__;  // "HH:MM:SS"
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int mon = 1;
  for (int i = 0; i < 12; i++) {
    if (d[0] == months[i*3] && d[1] == months[i*3+1] && d[2] == months[i*3+2]) {
      mon = i + 1;
      break;
    }
  }
  int day = (d[4] == ' ') ? (d[5] - '0') : (d[4] - '0') * 10 + (d[5] - '0');
  // Year is at d[7..10]
  snprintf(out, 20, "%.4s:%02d:%02d %s", d + 7, mon, day, t);
}

void buildExifBlock() {
  // ── String values ──
  const char *strMake     = "Retro Cam";
  const char *strModel    = "XIAO ESP32-S3 Sense";
  char strSoftware[40];
  snprintf(strSoftware, sizeof(strSoftware), "Retro Cam firmware %s", FIRMWARE_VERSION);
  const char *strArtist   = "github.com/barkinsarikartal";
  char strDateTime[20];
  formatExifDateTime(strDateTime);

  size_t lenMake = strlen(strMake) + 1;       // includes null terminator
  size_t lenModel = strlen(strModel) + 1;
  size_t lenSoftware = strlen(strSoftware) + 1;
  size_t lenArtist = strlen(strArtist) + 1;
  size_t lenDateTime = 20;  // "YYYY:MM:DD HH:MM:SS\0" — always 20 bytes per EXIF spec

  // ── Layout calculation ──
  // TIFF header starts at offset 0 (relative to TIFF start).
  // "II" + 0x002A + IFD0 offset (4+2+4 = 10 bytes... but standard is 8)
  // TIFF header: "II" (2) + magic 0x002A (2) + offset to IFD0 (4) = 8 bytes
  // IFD0 at offset 8.
  // IFD0: count (2) + 5 × IFD0 entries (60) + ExifIFD pointer entry (12)
  //       + next IFD offset (4) = 2 + 5*12 + 1*12 + 4 = 78 bytes
  // IFD0 entries: Make, Model, Software, Artist, ExifIFD pointer = 5 entries
  // Actually we need 5 IFD0 tags + 1 ExifIFD pointer = 6 entries total in IFD0
  // Wait — DateTimeOriginal is an Exif IFD tag, not IFD0. So:
  //   IFD0: Make, Model, Software, Artist, ExifIFD pointer = 5 entries
  //   ExifIFD: DateTimeOriginal = 1 entry
  //
  // IFD0 size: 2 + 5*12 + 4 = 66 bytes
  // ExifIFD follows IFD0, then string pool.
  // ExifIFD size: 2 + 1*12 + 4 = 18 bytes

  const int numIFD0 = 5;
  const int numExifIFD = 1;

  int tiffStart = 0;  // reference point for all offsets
  int ifd0Offset = 8;
  int ifd0Size = 2 + numIFD0 * 12 + 4;  // count + entries + next IFD ptr
  int exifIFDOffset = ifd0Offset + ifd0Size;
  int exifIFDSize = 2 + numExifIFD * 12 + 4;
  int stringPoolOffset = exifIFDOffset + exifIFDSize;

  // String pool layout: Make, Model, Software, Artist, DateTime
  int offMake = stringPoolOffset;
  int offModel = offMake + lenMake;
  int offSoftware = offModel + lenModel;
  int offArtist = offSoftware + lenSoftware;
  int offDateTime = offArtist + lenArtist;
  int totalTiffSize = offDateTime + lenDateTime;

  // APP1 segment: "Exif\0\0" (6) + TIFF data
  int app1DataLen = 6 + totalTiffSize;
  // FFE1 marker + 2-byte length + data
  int totalExifLen = 2 + 2 + app1DataLen;

  exifBlock = (uint8_t *)ps_malloc(totalExifLen);
  if (!exifBlock) {
    Serial.println("[ERR] EXIF block allocation failed!");
    exifBlockLen = 0;
    return;
  }
  memset(exifBlock, 0, totalExifLen);

  int p = 0;

  // APP1 marker
  exifBlock[p++] = 0xFF;
  exifBlock[p++] = 0xE1;

  // APP1 length (excludes the marker bytes, includes the 2 length bytes themselves)
  uint16_t app1Len = (uint16_t)(app1DataLen + 2);
  exifBlock[p++] = (app1Len >> 8) & 0xFF;  // big-endian per JPEG spec
  exifBlock[p++] = app1Len & 0xFF;

  // "Exif\0\0"
  exifBlock[p++] = 'E'; exifBlock[p++] = 'x';
  exifBlock[p++] = 'i'; exifBlock[p++] = 'f';
  exifBlock[p++] = 0x00; exifBlock[p++] = 0x00;

  // ── TIFF data starts here ──
  int tiffBase = p;  // all TIFF offsets are relative to this position

  // TIFF header: little-endian ("II"), magic 0x002A, offset to IFD0
  exifBlock[p++] = 'I'; exifBlock[p++] = 'I';
  put16(exifBlock, p, 0x002A); p += 2;
  put32(exifBlock, p, ifd0Offset); p += 4;

  // ── IFD0 ──
  put16(exifBlock, p, numIFD0); p += 2;

  // Helper lambda to write a 12-byte IFD entry
  auto writeIFDEntry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t valueOrOffset) {
    put16(exifBlock, tiffBase + (p - tiffBase), tag);   p += 2;
    put16(exifBlock, tiffBase + (p - tiffBase), type);  p += 2;
    put32(exifBlock, tiffBase + (p - tiffBase), count); p += 4;
    put32(exifBlock, tiffBase + (p - tiffBase), valueOrOffset); p += 4;
  };

  // IFD entries must be sorted by tag number (TIFF requirement)
  // TAG_MAKE (0x010F), TAG_MODEL (0x0110), TAG_SOFTWARE (0x0131), TAG_ARTIST (0x013B), TAG_EXIF_IFD_POINTER (0x8769)
  writeIFDEntry(TAG_MAKE,             TIFF_ASCII, lenMake,     offMake);
  writeIFDEntry(TAG_MODEL,            TIFF_ASCII, lenModel,    offModel);
  writeIFDEntry(TAG_SOFTWARE,         TIFF_ASCII, lenSoftware, offSoftware);
  writeIFDEntry(TAG_ARTIST,           TIFF_ASCII, lenArtist,   offArtist);
  writeIFDEntry(TAG_EXIF_IFD_POINTER, TIFF_LONG,  1,           exifIFDOffset);

  // Next IFD pointer (none)
  put32(exifBlock, tiffBase + (p - tiffBase), 0); p += 4;

  // ── Exif Sub-IFD ──
  put16(exifBlock, tiffBase + (p - tiffBase), numExifIFD); p += 2;
  writeIFDEntry(TAG_DATETIME_ORIGINAL, TIFF_ASCII, lenDateTime, offDateTime);
  // Next IFD pointer (none)
  put32(exifBlock, tiffBase + (p - tiffBase), 0); p += 4;

  // ── String pool ──
  memcpy(exifBlock + tiffBase + offMake,     strMake,     lenMake);
  memcpy(exifBlock + tiffBase + offModel,    strModel,    lenModel);
  memcpy(exifBlock + tiffBase + offSoftware, strSoftware, lenSoftware);
  memcpy(exifBlock + tiffBase + offArtist,   strArtist,   lenArtist);
  memcpy(exifBlock + tiffBase + offDateTime, strDateTime, lenDateTime);

  exifBlockLen = totalExifLen;

  Serial.printf("[EXIF] Block built: %u bytes (Make=%s, Software=%s, DateTime=%s)\n",
                exifBlockLen, strMake, strSoftware, strDateTime);
}

size_t getExifBlock(const uint8_t **out) {
  *out = exifBlock;
  return exifBlockLen;
}
