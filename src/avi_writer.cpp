#include "shared.h"

// ================= AVI WRITER GLOBALS =================
// These are allocated once at boot in allocateBuffers() (main.cpp) and
// memset on each recording start. Never freed — prevents PSRAM fragmentation.
long avi_movi_size = 0;
unsigned long avi_start_time = 0;
int avi_total_frames = 0;
uint32_t *avi_frame_sizes = nullptr;
uint32_t *avi_audio_sizes = nullptr;
int avi_total_audio_chunks = 0;
uint8_t *avi_chunk_order = nullptr;  // 0=video, 1=audio — tracks actual file layout for correct idx1
int avi_total_chunks = 0;

// ================= AVI WRITER =================
void startAVI(File &file, int fps, int width, int height) {
  avi_movi_size = 0;
  avi_total_frames = 0;
  avi_total_audio_chunks = 0;
  avi_total_chunks = 0;
  avi_start_time = millis();

  // Arrays are pre-allocated at boot — just zero them to clear stale data from last session.
  // This avoids PSRAM malloc/free cycles that cause heap fragmentation over multiple recordings.
  if (avi_frame_sizes) memset(avi_frame_sizes, 0, MAX_AVI_FRAMES * sizeof(uint32_t));
  if (avi_audio_sizes) memset(avi_audio_sizes, 0, MAX_AVI_FRAMES * sizeof(uint32_t));
  if (avi_chunk_order) memset(avi_chunk_order, 0, MAX_AVI_CHUNKS);

  uint8_t zero_buf[AVI_HEADER_SIZE];
  memset(zero_buf, 0, AVI_HEADER_SIZE);
  file.write(zero_buf, AVI_HEADER_SIZE);
}

bool writeAVIFrameFromBuf(File &file, uint8_t *buf, size_t len) {
  uint8_t dc_buf[4] = {0x30, 0x30, 0x64, 0x63}; // "00dc"
  size_t w1 = file.write(dc_buf, 4);

  uint32_t rem = len % 4;
  uint32_t padding = (rem == 0) ? 0 : 4 - rem;
  uint32_t totalLen = len + padding;

  size_t w2 = file.write((uint8_t*)&totalLen, 4);
  size_t w3 = file.write(buf, len);

  if (padding > 0) {
    uint8_t pad[3] = {0, 0, 0};
    file.write(pad, padding);
  }

  if (w1 != 4 || w2 != 4 || w3 != len) {
    return false;
  }

  avi_movi_size += (totalLen + 8);
  if (avi_frame_sizes && avi_total_frames < MAX_AVI_FRAMES) {
    avi_frame_sizes[avi_total_frames] = totalLen;
  }
  avi_total_frames++;
  if (avi_chunk_order && avi_total_chunks < MAX_AVI_CHUNKS) {
    avi_chunk_order[avi_total_chunks++] = 0;
  }
  return true;
}

bool writeAVIAudioChunk(File &file, uint8_t *buf, size_t bytes) {
  uint8_t wb_tag[4] = {0x30, 0x31, 0x77, 0x62}; // "01wb"
  size_t w1 = file.write(wb_tag, 4);

  uint32_t len = bytes;
  uint32_t rem = len % 4;
  uint32_t pad = (rem == 0) ? 0 : 4 - rem;
  uint32_t totalLen = len + pad;

  size_t w2 = file.write((uint8_t*)&totalLen, 4);
  size_t w3 = file.write(buf, bytes);

  if (pad > 0) {
    uint8_t zeros[3] = {0, 0, 0};
    file.write(zeros, pad);
  }

  if (w1 != 4 || w2 != 4 || w3 != bytes) return false;

  avi_movi_size += (totalLen + 8);
  if (avi_audio_sizes && avi_total_audio_chunks < MAX_AVI_FRAMES) {
    avi_audio_sizes[avi_total_audio_chunks] = totalLen;
  }
  avi_total_audio_chunks++;
  if (avi_chunk_order && avi_total_chunks < MAX_AVI_CHUNKS) {
    avi_chunk_order[avi_total_chunks++] = 1;
  }
  return true;
}

void endAVI(File &file, int fps, int width, int height) {
  // Defensive guard: if SD was removed mid-recording, the file handle is invalid.
  // Attempting to seek/write would crash or corrupt memory. Bail out gracefully.
  if (!file) {
    Serial.println("[REC] endAVI: file handle invalid (SD removed?), skipping finalization.");
    return;
  }

  unsigned long duration = millis() - avi_start_time;
  float real_fps = (float)avi_total_frames / (duration / 1000.0f);
  if (real_fps <= 0) real_fps = (float)TARGET_FPS;
  Serial.printf("[REC] Actual FPS: %.2f, frames: %d, audio chunks: %d\n",
                real_fps, avi_total_frames, avi_total_audio_chunks);



  // Each audio chunk is exactly AUDIO_BYTES_PER_FRAME bytes (zero-padded when
  // needed), so total samples is exactly chunks × samples-per-chunk.
  uint32_t total_audio_samples = (uint32_t)avi_total_audio_chunks * AUDIO_SAMPLES_PER_FRAME;

  // step 1: write INFO LIST chunk at end of file (before idx1)
  // Contains metadata visible in media players: artist, software, comment.
  file.seek(0, SeekEnd);

  const char *infoArtist   = "github.com/barkinsarikartal";
  const char *infoSoftware = "Retro Cam firmware " FIRMWARE_VERSION;
  const char *infoComment  = "Retro Cam - XIAO ESP32-S3 Sense";

  // Each INFO sub-chunk: 4-byte tag + 4-byte size + string (null-terminated, padded to even)
  auto infoChunkSize = [](const char *str) -> uint32_t {
    uint32_t len = strlen(str) + 1;  // include null terminator
    if (len % 2 != 0) len++;         // pad to even boundary (RIFF requirement)
    return len;
  };
  uint32_t iartSize = infoChunkSize(infoArtist);
  uint32_t isftSize = infoChunkSize(infoSoftware);
  uint32_t icmtSize = infoChunkSize(infoComment);
  // LIST header: "LIST" (4) + listSize (4) + "INFO" (4)
  // Sub-chunks: tag (4) + size (4) + data per chunk
  uint32_t infoPayload = 4 + (4 + 4 + iartSize) + (4 + 4 + isftSize) + (4 + 4 + icmtSize);

  file.write((const uint8_t*)"LIST", 4);
  file.write((uint8_t*)&infoPayload, 4);
  file.write((const uint8_t*)"INFO", 4);

  // Helper lambda: write a single INFO sub-chunk
  auto writeInfoChunk = [&](const char *tag, const char *str, uint32_t paddedSize) {
    file.write((const uint8_t*)tag, 4);
    file.write((uint8_t*)&paddedSize, 4);
    file.write((const uint8_t*)str, strlen(str) + 1);
    // Pad to even boundary if needed
    if ((strlen(str) + 1) % 2 != 0) {
      uint8_t pad = 0;
      file.write(&pad, 1);
    }
  };

  writeInfoChunk("IART", infoArtist, iartSize);
  writeInfoChunk("ISFT", infoSoftware, isftSize);
  writeInfoChunk("ICMT", infoComment, icmtSize);



  // step 2: write idx1 at end of file
  file.write((const uint8_t*)"idx1", 4);
  int total_idx_entries = avi_total_chunks;
  uint32_t idx1Size = total_idx_entries * 16;
  file.write((uint8_t*)&idx1Size, 4);

  // Build idx1 in actual chunk write order (not assumed V-A interleaving)
  uint32_t chunkOffset = 4;
  int vi = 0, ai = 0;
  for (int i = 0; i < avi_total_chunks; i++) {
    if (avi_chunk_order && avi_chunk_order[i] == 0) {
      uint32_t fSize = (avi_frame_sizes && vi < MAX_AVI_FRAMES) ? avi_frame_sizes[vi] : 0;
      file.write((const uint8_t*)"00dc", 4);
      uint32_t kf = 0x10;  // AVIIF_KEYFRAME
      file.write((uint8_t*)&kf, 4);
      file.write((uint8_t*)&chunkOffset, 4);
      file.write((uint8_t*)&fSize, 4);
      chunkOffset += fSize + 8;
      vi++;
    } else {
      uint32_t aSize = (avi_audio_sizes && ai < MAX_AVI_FRAMES) ? avi_audio_sizes[ai] : 0;
      file.write((const uint8_t*)"01wb", 4);
      uint32_t noKf = 0x00;
      file.write((uint8_t*)&noKf, 4);
      file.write((uint8_t*)&chunkOffset, 4);
      file.write((uint8_t*)&aSize, 4);
      chunkOffset += aSize + 8;
      ai++;
    }
  }

  // step 3: capture exact file position right after idx1 — file.size() may return
  // stale data before FAT commits, which caused riffSize = 0xFFFFFFF8 in MediaInfo.
  uint32_t totalSize = (uint32_t)file.position();
  uint32_t riffSize  = totalSize - 8;

  file.seek(0);
  // microSecPerFrame is derived from TARGET_FPS (set in endAVI preamble), not real_fps.
  uint32_t microSecPerFrame_hdr = 1000000UL / TARGET_FPS;
  uint32_t maxBytesPerSec = (uint32_t)((width * height * 2) * TARGET_FPS) + AUDIO_SAMPLE_RATE * 2;

  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&riffSize, 4);
  file.write((const uint8_t*)"AVI ", 4);

  file.write((const uint8_t*)"LIST", 4);
  uint32_t hdrlSize = 292;
  file.write((uint8_t*)&hdrlSize, 4);
  file.write((const uint8_t*)"hdrl", 4);

  file.write((const uint8_t*)"avih", 4);
  uint32_t avihSize = 56;
  file.write((uint8_t*)&avihSize, 4);

  file.write((uint8_t*)&microSecPerFrame_hdr, 4);
  file.write((uint8_t*)&maxBytesPerSec, 4);
  uint32_t padding = 0;
  file.write((uint8_t*)&padding, 4);
  uint32_t avih_flags = 0x10;  // AVIF_HASINDEX
  file.write((uint8_t*)&avih_flags, 4);
  file.write((uint8_t*)&avi_total_frames, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t streams = 2;  // video + audio
  file.write((uint8_t*)&streams, 4);
  uint32_t bufSize = width * height * 2;
  file.write((uint8_t*)&bufSize, 4);
  file.write((uint8_t*)&width, 4);
  file.write((uint8_t*)&height, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  file.write((const uint8_t*)"LIST", 4);
  uint32_t strlSize = 116;
  file.write((uint8_t*)&strlSize, 4);
  file.write((const uint8_t*)"strl", 4);

  file.write((const uint8_t*)"strh", 4);
  uint32_t strhSize = 56;
  file.write((uint8_t*)&strhSize, 4);
  file.write((const uint8_t*)"vids", 4);
  file.write((const uint8_t*)"MJPG", 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t scale = 1;
  file.write((uint8_t*)&scale, 4);
  uint32_t rate = TARGET_FPS;
  if (rate == 0) rate = 10;
  file.write((uint8_t*)&rate, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&avi_total_frames, 4);
  file.write((uint8_t*)&bufSize, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  file.write((const uint8_t*)"strf", 4);
  uint32_t strfSize = 40;
  file.write((uint8_t*)&strfSize, 4);
  file.write((uint8_t*)&strfSize, 4);
  file.write((uint8_t*)&width, 4);
  file.write((uint8_t*)&height, 4);
  uint16_t planes = 1;
  file.write((uint8_t*)&planes, 2);
  uint16_t bitCount = 24;
  file.write((uint8_t*)&bitCount, 2);
  file.write((const uint8_t*)"MJPG", 4);
  uint32_t imageSize = width * height * 3;
  file.write((uint8_t*)&imageSize, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  // audio stream LIST (strl)
  file.write((const uint8_t*)"LIST", 4);
  uint32_t strlSize_aud = 92;
  file.write((uint8_t*)&strlSize_aud, 4);
  file.write((const uint8_t*)"strl", 4);

  // audio strh
  file.write((const uint8_t*)"strh", 4);
  file.write((uint8_t*)&strhSize, 4);
  file.write((const uint8_t*)"auds", 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t audio_scale = 1;
  file.write((uint8_t*)&audio_scale, 4);
  uint32_t audio_rate = AUDIO_SAMPLE_RATE;
  file.write((uint8_t*)&audio_rate, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&total_audio_samples, 4);
  uint32_t audio_buf_suggest = AUDIO_SAMPLE_RATE * 2;
  file.write((uint8_t*)&audio_buf_suggest, 4);
  file.write((uint8_t*)&padding, 4);
  uint32_t audio_sample_size = 2;
  file.write((uint8_t*)&audio_sample_size, 4);
  file.write((uint8_t*)&padding, 4);
  file.write((uint8_t*)&padding, 4);

  // audio strf (PCMWAVEFORMAT)
  file.write((const uint8_t*)"strf", 4);
  uint32_t strfSize_aud = 16;
  file.write((uint8_t*)&strfSize_aud, 4);
  uint16_t wFormatTag = 1;  // PCM
  file.write((uint8_t*)&wFormatTag, 2);
  uint16_t nChannels = 1;
  file.write((uint8_t*)&nChannels, 2);
  uint32_t nSamplesPerSec = AUDIO_SAMPLE_RATE;
  file.write((uint8_t*)&nSamplesPerSec, 4);
  uint32_t nAvgBytesPerSec = AUDIO_SAMPLE_RATE * 2;
  file.write((uint8_t*)&nAvgBytesPerSec, 4);
  uint16_t nBlockAlign = 2;
  file.write((uint8_t*)&nBlockAlign, 2);
  uint16_t wBitsPerSample = 16;
  file.write((uint8_t*)&wBitsPerSample, 2);

  // movi LIST header
  file.write((const uint8_t*)"LIST", 4);
  uint32_t moviListSize = 4 + avi_movi_size;
  file.write((uint8_t*)&moviListSize, 4);
  file.write((const uint8_t*)"movi", 4);

  // AVI metadata arrays are intentionally kept alive (never freed).
  // They are boot-allocated and reused across sessions via memset in startAVI().

  file.close();
}
