// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "common/memory_ref.h"
#include "core/hle/ipc.h"

namespace Memory {
class MemorySystem;
}

namespace Kernel {

class KernelSystem;
class KThread;

struct MappedBufferContext {
    IPC::MappedBufferPermissions permissions;
    u32 size;
    VAddr source_address;
    VAddr target_address;

    std::shared_ptr<BackingMem> buffer;

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

/// Performs IPC command buffer translation from one process to another.
Result TranslateCommandBuffer(KernelSystem& system, Memory::MemorySystem& memory,
                              KThread* src_thread, KThread* dst_thread, VAddr src_address,
                              VAddr dst_address,
                              std::vector<MappedBufferContext>& mapped_buffer_context, bool reply);
} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::MappedBufferContext)
