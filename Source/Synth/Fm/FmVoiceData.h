#pragma once

// DX7 voice data model + sysex import/export for CoplanDX.
//
// The 155-byte DX7 voice layout and the 32-voice bulk packing are ported
// from Dexed (https://github.com/asb2m10/dexed, GPL v3) — core/Cartridge.cpp
// and core/Program.cpp. Fanfare is GPL v3 as well; see NOTICE.md.
//
// Internal patch layout (kPatchSize bytes):
//   [0..154]   DX7 voice, 6 op blocks of 21 bytes. Op block for internal
//              op index i (0 = Yamaha OP6 ... 5 = OP1) starts at i*21:
//                +0..3  EG rates R1-R4      +11 left curve  (0..3)
//                +4..7  EG levels L1-L4     +12 right curve (0..3)
//                +8     break point         +13 rate scaling (0..7)
//                +9     left depth          +14 amp mod sens (0..3)
//                +10    right depth         +15 key vel sens (0..7)
//                                        +16 output level (0..99)
//                +17    osc mode (0 ratio, 1 fixed)
//                +18    coarse freq (0..31) +19 fine freq (0..99)
//                +20    detune (0..14, 7 = center)
//   [126..129] pitch EG rates   [130..133] pitch EG levels
//   [134] algorithm (0..31)    [135] feedback (0..7)
//   [136] osc key sync         [137] LFO rate   [138] LFO delay
//   [139] LFO PMD              [140] LFO AMD    [141] LFO key sync
//   [142] LFO waveform (0 tri,1 saw dn,2 saw up,3 square,4 sine,5 S&H)
//   [143] pitch mod sens (0..7) [144] transpose (0..48, 24 = center)
//   [145..154] voice name (10 chars)
//   [155]      operator on/off flags (bit 5 = OP1 ... bit 0 = OP6)
//   [156..161] per-op waveform 0..7 (TX81Z mode; internal op order)

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace FanfareFm {

inline constexpr int kVoiceBytes = 155;   // DX7 voice on the wire
inline constexpr int kPatchSize = 162;    // Fanfare internal patch
inline constexpr int kBulkDataBytes = 4096; // 32 voices x 128 packed bytes
inline constexpr int kBulkSysexSize = 4104; // header 6 + 4096 + checksum + F7
inline constexpr int kSingleSysexSize = 163; // header 6 + 155 + checksum + F7

struct FmPatch {
  uint8_t d[kPatchSize];

  uint8_t &operator[](int i) { return d[i]; }
  uint8_t operator[](int i) const { return d[i]; }

  std::string name() const {
    char buf[11];
    for (int i = 0; i < 10; ++i) {
      unsigned char c = d[145 + i] & 0x7F;
      buf[i] = (c < 32 || c > 126) ? ' ' : (char)c;
    }
    buf[10] = 0;
    // trim trailing spaces
    int len = 10;
    while (len > 0 && buf[len - 1] == ' ')
      --len;
    return std::string(buf, (size_t)len);
  }

  void setName(const std::string &n) {
    for (int i = 0; i < 10; ++i) {
      char c = i < (int)n.size() ? n[(size_t)i] : ' ';
      if (c < 32 || c > 127)
        c = ' ';
      d[145 + i] = (uint8_t)c;
    }
  }
};

inline uint8_t sysexChecksum(const uint8_t *data, int size) {
  int sum = 0;
  for (int i = 0; i < size; ++i)
    sum -= data[i];
  return (uint8_t)(sum & 0x7F);
}

// The DX7 "INIT VOICE" (as shipped by Dexed's Program::initialProgram).
inline void initPatch(FmPatch &p) {
  static const uint8_t kInit[kVoiceBytes] = {
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
      99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 99, 0, 1, 0, 7,
      99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24,
      73, 78, 73, 84, 32, 86, 79, 73, 67, 69};
  memset(p.d, 0, kPatchSize);
  memcpy(p.d, kInit, kVoiceBytes);
  p.d[155] = 63; // all operators on
  for (int i = 0; i < 6; ++i)
    p.d[156 + i] = 0; // sine
}

// Unpack one 128-byte packed bulk voice into the 155-byte layout.
// Ported from Dexed's Cartridge::unpackProgram.
inline void unpackBulkVoice(const uint8_t *bulk, uint8_t ret[kVoiceBytes]) {
  for (int op = 0; op < 6; op++) {
    memcpy(ret + op * 21, bulk + op * 17, 11);
    const uint8_t leftrightcurves = bulk[op * 17 + 11] & 0xF;
    ret[op * 21 + 11] = leftrightcurves & 3;
    ret[op * 21 + 12] = (leftrightcurves >> 2) & 3;
    const uint8_t detune_rs = bulk[op * 17 + 12] & 0x7F;
    ret[op * 21 + 13] = detune_rs & 7;
    const uint8_t kvs_ams = bulk[op * 17 + 13] & 0x1F;
    ret[op * 21 + 14] = kvs_ams & 3;
    ret[op * 21 + 15] = (kvs_ams >> 2) & 7;
    ret[op * 21 + 16] = bulk[op * 17 + 14] & 0x7F;
    const uint8_t fcoarse_mode = bulk[op * 17 + 15] & 0x3F;
    ret[op * 21 + 17] = fcoarse_mode & 1;
    ret[op * 21 + 18] = (fcoarse_mode >> 1) & 0x1F;
    ret[op * 21 + 19] = bulk[op * 17 + 16] & 0x7F;
    ret[op * 21 + 20] = (detune_rs >> 3) & 0x7F;
  }
  for (int i = 0; i < 8; i++)
    ret[126 + i] = bulk[102 + i] & 0x7F;
  ret[134] = bulk[110] & 0x1F;
  const uint8_t oks_fb = bulk[111] & 0xF;
  ret[135] = oks_fb & 7;
  ret[136] = oks_fb >> 3;
  ret[137] = bulk[112] & 0x7F;
  ret[138] = bulk[113] & 0x7F;
  ret[139] = bulk[114] & 0x7F;
  ret[140] = bulk[115] & 0x7F;
  const uint8_t lpms_lfw_lks = bulk[116] & 0x7F;
  ret[141] = lpms_lfw_lks & 1;
  ret[142] = (lpms_lfw_lks >> 1) & 7;
  ret[143] = lpms_lfw_lks >> 4;
  ret[144] = bulk[117] & 0x7F;
  for (int i = 0; i < 10; i++)
    ret[145 + i] = bulk[118 + i] & 0x7F;
}

// Pack a 155-byte voice into a 128-byte bulk slot (inverse of the above).
inline void packBulkVoice(const uint8_t src[kVoiceBytes], uint8_t *bulk) {
  for (int op = 0; op < 6; op++) {
    const int pp = op * 17, up = op * 21;
    memcpy(bulk + pp, src + up, 11);
    bulk[pp + 11] = (src[up + 11] & 0x03) | ((src[up + 12] & 0x03) << 2);
    bulk[pp + 12] = (src[up + 13] & 0x07) | ((src[up + 20] & 0x0F) << 3);
    bulk[pp + 13] = (src[up + 14] & 0x03) | ((src[up + 15] & 0x07) << 2);
    bulk[pp + 14] = src[up + 16];
    bulk[pp + 15] = (src[up + 17] & 0x01) | ((src[up + 18] & 0x1F) << 1);
    bulk[pp + 16] = src[up + 19];
  }
  memcpy(bulk + 102, src + 126, 9);
  bulk[111] = (src[135] & 0x07) | ((src[136] & 0x01) << 3);
  memcpy(bulk + 112, src + 137, 4);
  bulk[116] = (src[141] & 0x01) | (((src[142] & 0x07) << 1) | ((src[143] & 0x07) << 4));
  bulk[117] = src[144];
  for (int i = 0; i < 10; i++)
    bulk[118 + i] = src[145 + i];
}

// Parse a .syx / raw dump. Accepts:
//   - 32-voice bulk dump (F0 43 0n 09 20 00 + 4096 + checksum + F7, or a raw
//     4096-byte payload) — DX7 6-op VMEM
//   - single voice (F0 43 0n 00 01 1B + 155 + checksum + F7, or a raw 155-byte
//     voice)
//   - concatenated streams of the above
// Native 4-op VMEM dumps (format 0x04: DX21/DX100/TX81Z hardware) are NOT
// convertible here — they are skipped and counted in *skipped4op so the UI
// can point at the dx7-converted/ twins.
// Returns the list of parsed patches; checksum mismatches are tolerated
// (reported via *checksumErrors if non-null).
inline std::vector<FmPatch> parseSysex(const uint8_t *data, size_t size,
                                       int *checksumErrors = nullptr,
                                       int *skipped4op = nullptr) {
  std::vector<FmPatch> out;
  if (checksumErrors)
    *checksumErrors = 0;
  if (skipped4op)
    *skipped4op = 0;
  size_t pos = 0;
  while (pos < size) {
    // find next F0 43
    while (pos + 1 < size && !(data[pos] == 0xF0 && data[pos + 1] == 0x43))
      ++pos;
    if (pos + 6 > size)
      break;
    // header: F0 43 <device> <substatus+ch> <format> <countMS> <countLS>
    const uint8_t fmt = data[pos + 3];
    if (fmt == 0x09 && pos + 6 + kBulkDataBytes <= size) {
      // 32-voice bulk
      const uint8_t *payload = data + pos + 6;
      const bool hasChecksum = (pos + 6 + kBulkDataBytes + 2 <= size) &&
                               (data[pos + 6 + kBulkDataBytes + 1] == 0xF7);
      if (hasChecksum &&
          sysexChecksum(payload, kBulkDataBytes) != data[pos + 6 + kBulkDataBytes]) {
        if (checksumErrors)
          ++*checksumErrors;
      }
      for (int v = 0; v < 32; ++v) {
        FmPatch p;
        memset(p.d, 0, kPatchSize);
        unpackBulkVoice(payload + v * 128, p.d);
        p.d[155] = 63;
        out.push_back(p);
      }
      pos += 6 + kBulkDataBytes + (hasChecksum ? 2 : 0);
    } else if (fmt == 0x04 && pos + 6 + kBulkDataBytes <= size) {
      // native 4-op VMEM (DX21/DX100/TX81Z): skip, count for the UI hint
      if (skipped4op)
        ++*skipped4op;
      pos += 6 + kBulkDataBytes + 2;
    } else if ((fmt == 0x00 || fmt == 0x01) && pos + 6 + kVoiceBytes <= size) {
      // single voice
      const uint8_t *payload = data + pos + 6;
      const bool hasChecksum = (pos + 6 + kVoiceBytes + 2 <= size) &&
                               (data[pos + 6 + kVoiceBytes + 1] == 0xF7);
      if (hasChecksum &&
          sysexChecksum(payload, kVoiceBytes) != data[pos + 6 + kVoiceBytes]) {
        if (checksumErrors)
          ++*checksumErrors;
      }
      FmPatch p;
      memset(p.d, 0, kPatchSize);
      memcpy(p.d, payload, kVoiceBytes);
      p.d[155] = 63;
      out.push_back(p);
      pos += 6 + kVoiceBytes + (hasChecksum ? 2 : 0);
    } else if (size - pos >= kBulkDataBytes && data[pos] == 0xF0) {
      break; // malformed header; stop
    } else {
      ++pos; // skip this F0 and keep scanning
    }
  }
  // raw payloads without sysex header (only if no sysex message was seen at
  // all — a skipped 0x04 message must not fall through to raw unpacking)
  if (out.empty() && (skipped4op == nullptr || *skipped4op == 0) &&
      size >= kVoiceBytes) {
    if (size >= (size_t)kBulkDataBytes) {
      for (int v = 0; v < 32 && (size_t)(v + 1) * 128 <= size; ++v) {
        FmPatch p;
        memset(p.d, 0, kPatchSize);
        unpackBulkVoice(data + v * 128, p.d);
        p.d[155] = 63;
        out.push_back(p);
      }
    } else {
      FmPatch p;
      memset(p.d, 0, kPatchSize);
      memcpy(p.d, data, kVoiceBytes);
      p.d[155] = 63;
      out.push_back(p);
    }
  }
  return out;
}

// Serialize one patch as a single-voice sysex message.
inline void packSingleSysex(const FmPatch &p, uint8_t out[kSingleSysexSize]) {
  static const uint8_t kHeader[6] = {0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B};
  memcpy(out, kHeader, 6);
  memcpy(out + 6, p.d, kVoiceBytes);
  out[161] = sysexChecksum(out + 6, kVoiceBytes);
  out[162] = 0xF7;
}

// Serialize 32 patches as a 32-voice bulk dump (padded with the first patch).
inline void packBulkSysex(const FmPatch *patches, int count,
                          uint8_t out[kBulkSysexSize]) {
  static const uint8_t kHeader[6] = {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00};
  memcpy(out, kHeader, 6);
  for (int v = 0; v < 32; ++v) {
    const FmPatch &p = patches[(count > 0) ? (v % count) : 0];
    packBulkVoice(p.d, out + 6 + v * 128);
  }
  out[4102] = sysexChecksum(out + 6, kBulkDataBytes);
  out[4103] = 0xF7;
}

// Best-effort mapping of a DX7 algorithm (0..31) onto the 4-op set (0..12)
// for DX21/TX81Z modes when a DX7 patch is loaded, and a rough inverse used
// when switching back to a 6-op mode.
inline int dx7AlgTo4op(int alg) {
  static const int kMap[32] = {
      0, 0, 1, 1, 0, 1, 2, 2, // 1-8
      3, 3, 4, 4, 5, 5, 6, 2, // 9-16
      3, 4, 0, 4, 5, 0, 4, 5, // 17-24
      6, 0, 4, 2, 4, 5, 7, 7, // 25-32
  };
  return kMap[alg & 31];
}

inline int alg4opToDx7(int alg) {
  static const int kMap[13] = {0, 1, 2, 3, 5, 6, 8, 31, 9, 9, 10, 11, 12};
  return kMap[alg < 13 ? alg : 0];
}

} // namespace FanfareFm
