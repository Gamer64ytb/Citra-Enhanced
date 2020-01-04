#pragma once

#include <memory>
#include <vector>
#include <boost/serialization/export.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include "common/assert.h"
#include "common/common_types.h"

/// Abstract host-side memory - for example a static buffer, or local vector
class BackingMem {
public:
    virtual ~BackingMem() = default;
    virtual u8* GetPtr() = 0;
    virtual u32 GetSize() const = 0;
};

/// Backing memory implemented by a local buffer
class BufferMem : public BackingMem {
public:
    BufferMem() = default;
    BufferMem(u32 size) : data(std::vector<u8>(size)) {}

    virtual u8* GetPtr() {
        return data.data();
    }

    virtual u32 GetSize() const {
        return static_cast<u32>(data.size());
    }

    std::vector<u8>& Vector() {
        return data;
    }

private:
    std::vector<u8> data;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& data;
    }
    friend class boost::serialization::access;
};

BOOST_CLASS_EXPORT_KEY(BufferMem);

/// A managed reference to host-side memory. Fast enough to be used everywhere instead of u8*
/// Supports serialization.
class MemoryRef {
public:
    MemoryRef() = default;
    MemoryRef(std::nullptr_t) {}
    MemoryRef(std::shared_ptr<BackingMem> backing_mem_)
        : backing_mem(std::move(backing_mem_)), offset(0) {
        Init();
    }
    MemoryRef(std::shared_ptr<BackingMem> backing_mem_, u32 offset_)
        : backing_mem(std::move(backing_mem_)), offset(offset_) {
        ASSERT(offset < backing_mem->GetSize());
        Init();
    }
    inline operator u8*() {
        return cptr;
    }
    inline u8* GetPtr() {
        return cptr;
    }
    inline operator bool() const {
        return cptr != nullptr;
    }
    inline const u8* GetPtr() const {
        return cptr;
    }
    inline u32 GetSize() const {
        return csize;
    }
    inline void operator+=(u32 offset_by) {
        ASSERT(offset_by < csize);
        offset += offset_by;
        Init();
    }
    inline MemoryRef operator+(u32 offset_by) const {
        ASSERT(offset_by < csize);
        return MemoryRef(backing_mem, offset + offset_by);
    }
    inline u8* operator+(std::size_t offset_by) const {
        ASSERT(offset_by < csize);
        return cptr + offset_by;
    }

private:
    std::shared_ptr<BackingMem> backing_mem;
    u32 offset;
    // Cached values for speed
    u8* cptr;
    u32 csize;

    void Init() {
        cptr = backing_mem->GetPtr() + offset;
        csize = static_cast<u32>(backing_mem->GetSize() - offset);
    }

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& backing_mem;
        ar& offset;
        Init();
    }
    friend class boost::serialization::access;
};
