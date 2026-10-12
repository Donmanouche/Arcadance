#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace midipad {

// Tap tempo: the tempo of the last taps, from the second one on. It takes the
// median of up to four intervals, so a tap clearly off counts for nothing; a
// pause longer than kResetNs starts over.
class TapTempo {
public:
    static constexpr std::uint64_t kResetNs = 2'000'000'000;
    static constexpr double kMinTempo = 20, kMaxTempo = 999;  // Ableton Link's range

    std::optional<double> tap(std::uint64_t nowNs) {
        if (!any_ || nowNs <= last_ || nowNs - last_ > kResetNs) {
            any_ = true;
            last_ = nowNs;
            count_ = 0;
            return std::nullopt;
        }
        // The newest interval first; the oldest falls off.
        for (std::size_t i = intervals_.size() - 1; i > 0; --i)
            intervals_[i] = intervals_[i - 1];
        intervals_[0] = nowNs - last_;
        last_ = nowNs;
        count_ = std::min<int>(count_ + 1, static_cast<int>(intervals_.size()));
        std::array<std::uint64_t, 4> sorted = intervals_;
        for (int i = 1; i < count_; ++i)  // a few values: insertion
            for (int k = i; k > 0 && sorted[static_cast<std::size_t>(k)] < sorted[static_cast<std::size_t>(k - 1)]; --k)
                std::swap(sorted[static_cast<std::size_t>(k)], sorted[static_cast<std::size_t>(k - 1)]);
        const double median = count_ % 2 ? static_cast<double>(sorted[static_cast<std::size_t>(count_ / 2)])
                                         : (static_cast<double>(sorted[static_cast<std::size_t>(count_ / 2 - 1)]) +
                                            static_cast<double>(sorted[static_cast<std::size_t>(count_ / 2)])) / 2.0;
        return std::clamp(60e9 / median, kMinTempo, kMaxTempo);
    }

private:
    std::array<std::uint64_t, 4> intervals_{};
    int count_ = 0;
    std::uint64_t last_ = 0;
    bool any_ = false;
};

}  // namespace midipad
