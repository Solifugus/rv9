/*
 * CRC-32, zlib polynomial (0xEDB88320 reflected).
 *
 * Written out rather than using esp_rom_crc32_le because the host tool
 * (tools/mkmodule.py) computes this with Python's zlib, and the two must
 * agree bit for bit. A ROM routine with different seed conventions would
 * be a subtle and miserable bug.
 *
 * Bitwise, no table: modules are verified at boot and at link time, not in
 * a hot loop, and 1 KB of table is 1 KB we would rather have.
 */
#include "rv9/module.h"

uint32_t rv9_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}
