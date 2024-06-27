// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "core/global.h"
#include "core/hle/kernel/slab_helpers.h"

namespace Kernel {

enum class ResourceLimitCategory : u8 {
    Application = 0,
    SysApplet = 1,
    LibApplet = 2,
    Other = 3,
};

enum class ResourceLimitType : u32 {
    Priority = 0,
    Commit = 1,
    Thread = 2,
    Event = 3,
    Mutex = 4,
    Semaphore = 5,
    Timer = 6,
    SharedMemory = 7,
    AddressArbiter = 8,
    CpuTime = 9,
    Max = 10,
};

class KResourceLimit final : public KAutoObjectWithSlabHeapAndContainer<KResourceLimit> {
    KERNEL_AUTOOBJECT_TRAITS(KResourceLimit, KAutoObject);

public:
    explicit KResourceLimit(KernelSystem& kernel);
    ~KResourceLimit() override;

    void Initialize(std::string name) {}

    s32 GetCurrentValue(ResourceLimitType type) const;
    s32 GetLimitValue(ResourceLimitType type) const;

    void SetLimitValue(ResourceLimitType name, s32 value);

    bool Reserve(ResourceLimitType type, s32 amount);
    bool Release(ResourceLimitType type, s32 amount);

    static void PostDestroy(uintptr_t arg) {}

private:
    using ResourceArray = std::array<s32, static_cast<std::size_t>(ResourceLimitType::Max)>;
    ResourceArray m_limit_values{};
    ResourceArray m_current_values{};
    std::string m_name{};

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version);
};

class ResourceLimitList {
public:
    explicit ResourceLimitList(KernelSystem& kernel);
    ~ResourceLimitList();

    /**
     * Retrieves the resource limit associated with the specified resource limit category.
     * @param category The resource limit category
     * @returns The resource limit associated with the category
     */
    KResourceLimit* GetForCategory(ResourceLimitCategory category);

private:
    std::array<KResourceLimit*, 4> resource_limits;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KResourceLimit)
BOOST_CLASS_EXPORT_KEY(Kernel::ResourceLimitList)
CONSTRUCT_KERNEL_OBJECT(Kernel::KResourceLimit)
CONSTRUCT_KERNEL_OBJECT(Kernel::ResourceLimitList)
