#include "Wspr.h"

#include <ctype.h>
#include <string.h>

static const uint8_t syncVec[WSPR_NN] = {
  1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0,
  1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0,
  0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1,
  0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0,
  1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1,
  0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1,
  1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0
};

static int code36(char c) {   // 0-9 -> 0-9, A-Z -> 10-35, space -> 36
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 36;
}

static uint32_t parity32(uint32_t v) {
  v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
  return v & 1;
}

bool wsprEncode(const char *call, const char *grid, int dbm, uint8_t symbols[WSPR_NN]) {
  if (!call || !grid || dbm < 0 || dbm > 60) return false;
  size_t len = strlen(call);
  if (len < 3 || len > 6 || strlen(grid) != 4) return false;

  // 6-character callsign field: the digit must sit in position 3 (index 2)
  char c6[7] = "      ";
  char up[7];
  for (size_t i = 0; i < len; i++) {
    up[i] = (char)toupper((unsigned char)call[i]);
    if (!isalnum((unsigned char)up[i])) return false;
  }
  up[len] = 0;
  if (isdigit((unsigned char)up[2])) memcpy(c6, up, len);
  else if (isdigit((unsigned char)up[1])) memcpy(c6 + 1, up, len > 5 ? 5 : len);   // "K1ABC" -> " K1ABC"
  else return false;
  if (!isdigit((unsigned char)c6[2]) || len > 6 || (c6[1] == ' ' && c6[0] != ' ')) return false;
  for (int i = 3; i < 6; i++) if (c6[i] != ' ' && !isalpha((unsigned char)c6[i])) return false;

  char g[5];
  for (int i = 0; i < 4; i++) g[i] = (char)toupper((unsigned char)grid[i]);
  if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3]))
    return false;

  uint32_t n = code36(c6[0]);
  n = n * 36 + code36(c6[1]);
  n = n * 10 + code36(c6[2]);
  n = n * 27 + (code36(c6[3]) - 10);
  n = n * 27 + (code36(c6[4]) - 10);
  n = n * 27 + (code36(c6[5]) - 10);

  uint32_t m = ((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180) + 10 * (g[1] - 'A') + (g[3] - '0');
  m = m * 128 + (uint32_t)dbm + 64;

  // 50 data bits + 31 zero tail bits = 81 bits -> 162 coded bits
  uint8_t c[11] = {0};
  c[0] = n >> 20; c[1] = n >> 12; c[2] = n >> 4;
  c[3] = (uint8_t)(((n & 0x0f) << 4) | ((m >> 18) & 0x0f));
  c[4] = m >> 10; c[5] = m >> 2; c[6] = (uint8_t)((m & 0x03) << 6);

  uint8_t s[WSPR_NN];
  uint32_t r0 = 0, r1 = 0;
  int cnt = 0;
  for (int i = 0; i < 11 && cnt < WSPR_NN; i++) {
    for (int j = 0; j < 8 && cnt < WSPR_NN; j++) {
      uint32_t bit = (c[i] >> (7 - j)) & 1;
      r0 = (r0 << 1) | bit;
      r1 = (r1 << 1) | bit;
      s[cnt++] = (uint8_t)parity32(r0 & 0xf2d05351UL);
      s[cnt++] = (uint8_t)parity32(r1 & 0xe4613c47UL);
    }
  }

  // Interleave by 8-bit reversed index, then merge the sync vector: tone = sync + 2 * data
  uint8_t d[WSPR_NN];
  int p = 0;
  for (int j = 0; j < 256 && p < WSPR_NN; j++) {
    uint8_t rev = 0;
    for (int k = 0; k < 8; k++) if (j & (1 << k)) rev |= 1 << (7 - k);
    if (rev < WSPR_NN) d[rev] = s[p++];
  }
  for (int i = 0; i < WSPR_NN; i++) symbols[i] = syncVec[i] + 2 * d[i];
  return true;
}
