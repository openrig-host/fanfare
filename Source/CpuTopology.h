#pragma once

#include <cstdint>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace Fanfare {

/**
 * Hybrid CPU topology masks (Windows P-core / E-core awareness + physical core
 * masks) queried from GetLogicalProcessorInformationEx.
 */
struct CpuTopologyMasks {
    uint64_t sysMask = 0;
    uint64_t pCoreMask = 0;
    uint64_t eCoreMask = 0;
    std::vector<uint64_t> physicalCoreMasks;
};

inline int countBitsInMask(uint64_t m) noexcept {
    int c = 0;
    while (m != 0) {
        m &= (m - 1);
        ++c;
    }
    return c;
}

inline CpuTopologyMasks queryCpuTopology() {
    CpuTopologyMasks topo;
#ifdef _WIN32
    DWORD_PTR procMask = 0, sysMask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &procMask, &sysMask))
        topo.sysMask = static_cast<uint64_t>(sysMask);

    DWORD length = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0)
        return topo;
    std::vector<char> buffer(length);
    auto* info =
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
        return topo;

    BYTE maxEfficiency = 0;
    DWORD offset = 0;
    while (offset < length) {
        auto* cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data() + offset);
        if (cur->Relationship == RelationProcessorCore) {
            if (cur->Processor.EfficiencyClass > maxEfficiency)
                maxEfficiency = cur->Processor.EfficiencyClass;
        }
        offset += cur->Size;
    }

    offset = 0;
    while (offset < length) {
        auto* cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data() + offset);
        if (cur->Relationship == RelationProcessorCore &&
            cur->Processor.GroupCount > 0) {
            const uint64_t coreMask =
                static_cast<uint64_t>(cur->Processor.GroupMask[0].Mask);
            topo.physicalCoreMasks.push_back(coreMask);
            if (maxEfficiency == 0 ||
                cur->Processor.EfficiencyClass == maxEfficiency) {
                topo.pCoreMask |= coreMask;
            } else {
                topo.eCoreMask |= coreMask;
            }
        }
        offset += cur->Size;
    }
#endif
    return topo;
}

} // namespace Fanfare
