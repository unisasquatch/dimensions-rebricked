#pragma once
#include <cstddef>
#include <cstdint>

namespace host {

enum Protect : uint32_t { kRead = 1, kWrite = 2, kExec = 4 };

// Reserves and commits a read/write region anywhere in the address space.
uint8_t* AllocateImage(size_t size);
void FreeImage(uint8_t* base, size_t size);
bool SetProtection(uint8_t* addr, size_t size, uint32_t prot);
// Copies n bytes from memory that may not be mapped; false (dst unspecified) if any of it is not readable.
bool SafeCopy(void* dst, const void* src, size_t n);

}  // namespace host
