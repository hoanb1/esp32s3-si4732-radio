// Encode one FT8 message with ft8_lib and print its 79 tones, comma separated (for ft8station)
#include <stdio.h>
#include "ft8/message.h"
#include "ft8/encode.h"
#include "ft8/constants.h"
int main(int c, char **v) { ftx_message_t m; int rc = ftx_message_encode(&m, NULL, v[1]); uint8_t t[79];
  if (rc) { printf("rc=%d\n", rc); return 1; } ft8_encode(m.payload, t);
  for (int i = 0; i < 79; i++) printf("%d%s", t[i], i < 78 ? "," : "\n"); return 0; }
