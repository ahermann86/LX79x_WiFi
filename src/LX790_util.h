#include <stdint.h>
#include <string.h>

char DecodeChar (char raw);
int DecodeChars_IsRun (uint8_t raw[4]);
int DecodeChars_IsRunReady (uint8_t raw[4]);
uint8_t EncodeSeg (uint8_t c);
const char * DecodeMsg (char raw[4]);
