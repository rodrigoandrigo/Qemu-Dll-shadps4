/* UWP-safe settings used by the standalone shader bridge. */
#pragma once

struct ShadPS4BridgeSettings {
    constexpr bool IsDirectMemoryAccessEnabled() const
    {
        return false;
    }

    constexpr bool IsDumpShaders() const
    {
        return false;
    }
};

inline constexpr ShadPS4BridgeSettings EmulatorSettings{};
