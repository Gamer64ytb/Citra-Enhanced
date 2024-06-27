// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include <boost/serialization/vector.hpp>

namespace Kernel {

class CodeSet {
public:
    CodeSet() = default;
    ~CodeSet() = default;

    struct Segment {
        std::size_t offset = 0;
        VAddr addr = 0;
        u32 size = 0;

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar& offset;
            ar& addr;
            ar& size;
        }
    };

    Segment& CodeSegment() {
        return segments[0];
    }

    const Segment& CodeSegment() const {
        return segments[0];
    }

    Segment& RODataSegment() {
        return segments[1];
    }

    const Segment& RODataSegment() const {
        return segments[1];
    }

    Segment& DataSegment() {
        return segments[2];
    }

    const Segment& DataSegment() const {
        return segments[2];
    }

    std::vector<u8> memory;

    std::array<Segment, 3> segments;
    VAddr entrypoint;

    u64 program_id;
    std::string name;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        ar& memory;
        ar& segments;
        ar& entrypoint;
        ar& program_id;
        ar& name;
    }
};

} // namespace Kernel
