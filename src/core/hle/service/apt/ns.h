// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class Process;
}

namespace Service::FS {
enum class MediaType : u32;
}

namespace Service::NS {

/// Loads and launches the title identified by title_id in the specified media type.
Kernel::Process* LaunchTitle(Core::System& system, FS::MediaType media_type, u64 title_id);

/// Reboots the system to the specified title.
void RebootToTitle(Core::System& system, FS::MediaType media_type, u64 title_id);

} // namespace Service::NS
