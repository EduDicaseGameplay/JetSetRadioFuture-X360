// Xbox 360 guest memory: one reserved 256 MB host window, every guest address folds into it with
// X86_GUEST_MASK (see cpu/x86_ops.h). Window layout (guest address -> window offset):
//   0x00010000.. 0x03FFFFFF   guest physical RAM, 64 MB. User virtual memory is identity mapped onto it
//                             (va == pa), so 0x80000000|pa and 0xF0000000|pa alias the same bytes as the console.
//   0xB4000000 -> 0x04000000  system pool (ExAllocatePool, kernel objects), 32 MB
//   0xD6000000 -> 0x06000000  guest thread stacks, 31 MB
//   0xB7F00000 -> 0x07F00000  kernel data exports, 1 MB
//   0x08000000..0x0CFFFFFF    JSRF: memoria virtual do jogo, 80 MB, alem dos 64 MB fisicos (memory.cpp)
//   0x0D000000..0x0FFFFFFF    sinks for the NV2A (0xFD...), APU (0xFE8...) and flash (0xFF...) registers
#pragma once
#include <stdint.h>

namespace mem {

static const uint32_t kPage = 0x1000;
static const uint32_t kPhysicalSize = 64u << 20;
static const uint32_t kUserLow = 0x00010000;
static const uint32_t kContiguousBase = 0x80000000;  // guest VA = kContiguousBase | physical
static const uint32_t kWriteCombineBase = 0xF0000000;
static const uint32_t kSystemBase = 0xB4000000;
static const uint32_t kSystemSize = 0x02000000;
static const uint32_t kStackBase = 0xD6000000;
static const uint32_t kStackSize = 0x01F00000;
static const uint32_t kKernelDataBase = 0xB7F00000;
static const uint32_t kKernelDataSize = 0x00100000;
static const uint32_t kWindowSize = 0x10000000;

// NT allocation flags
static const uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
                      kMemTopDown = 0x100000, kMemNoZero = 0x800000;

bool Init();
uint8_t* Base();
bool Commit(uint32_t va, uint32_t size);
void Decommit(uint32_t va, uint32_t size);
bool IsCommitted(uint32_t va);

// user virtual space, NTSTATUS results
uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect);
uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type);
struct RegionInfo {
    uint32_t base, alloc_base, alloc_protect, size, state, protect, type;
};
bool QueryVirtual(uint32_t addr, RegionInfo& out);
void ReserveImage(uint32_t base, uint32_t size);

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align);
void FreeContiguous(uint32_t va);
uint32_t AllocationSize(uint32_t va);  // any allocator, 0 if unknown

uint32_t AllocSystem(uint32_t size, uint32_t align = 16);
void FreeSystem(uint32_t va);

uint32_t AllocStack(uint32_t size);  // returns the top
void FreeStack(uint32_t top);

uint32_t PhysicalFromVirtual(uint32_t va);
uint32_t VirtualFromPhysical(uint32_t pa);

struct Stats {
    uint32_t total_pages, available_pages, virtual_committed, contiguous_used, system_used;
};
Stats Query();

}  // namespace mem
