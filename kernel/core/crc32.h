/*
 * CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), as used by GPT.
 */

#ifndef CORE_CRC32_H
#define CORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32(const void *data, size_t length);

#endif
