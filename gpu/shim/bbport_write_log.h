// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_WRITE_LOG=1 — a ring of the writes the GPU emulation makes to guest memory (fences,
// WriteData, buffer and image downloads), dumped when the guest faults: which of them landed
// where the guest's data got corrupted. Diagnostic only.
#pragma once

#include <cstdint>

namespace BbWriteLog {
enum Source : std::uint32_t { Backing, WriteData, Fence };
bool Enabled();
void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source);
}

extern "C" void bbgpu_dump_guest_writes(void* ucontext);
