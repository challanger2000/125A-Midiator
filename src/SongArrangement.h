#pragma once

#include "RiffEngine.h"

#include <array>
#include <cstddef>

namespace midiator {

// One unit equals the current Section Length. The forms deliberately stay
// compact and deterministic: changing Style changes the macro-arrangement,
// while Variation never unexpectedly reorders the song.
inline constexpr std::array<SectionType, 16> kNdhIndustrialSongForm{{
    SectionType::Intro,
    SectionType::Verse, SectionType::Verse,
    SectionType::PreChorus,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Verse, SectionType::Verse,
    SectionType::PreChorus,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Breakdown, SectionType::Breakdown,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Outro
}};

inline constexpr std::array<SectionType, 16> kDarkRockGothicSongForm{{
    SectionType::Intro, SectionType::Intro,
    SectionType::Verse, SectionType::Verse,
    SectionType::PreChorus,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Verse, SectionType::Verse,
    SectionType::PreChorus,
    SectionType::Breakdown, SectionType::Breakdown,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Outro, SectionType::Outro
}};

inline constexpr std::array<SectionType, 16> kHeavyIndustrialSongForm{{
    SectionType::Intro,
    SectionType::Verse, SectionType::Verse,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Verse,
    SectionType::Breakdown, SectionType::Breakdown,
    SectionType::Verse,
    SectionType::PreChorus,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Breakdown,
    SectionType::Chorus, SectionType::Chorus,
    SectionType::Outro
}};

inline const std::array<SectionType, 16>& songFormForStyle(StyleId style) noexcept {
    switch (style) {
        case StyleId::DarkRockGothic:
            return kDarkRockGothicSongForm;
        case StyleId::HeavyIndustrial:
            return kHeavyIndustrialSongForm;
        case StyleId::NDHIndustrial:
        case StyleId::Count:
            return kNdhIndustrialSongForm;
    }
    return kNdhIndustrialSongForm;
}

inline SectionType songSectionForUnit(long long unit, StyleId style) noexcept {
    const auto& form = songFormForStyle(style);
    const auto count = static_cast<long long>(form.size());
    long long wrapped = unit % count;
    if (wrapped < 0)
        wrapped += count;
    return form[static_cast<std::size_t>(wrapped)];
}

} // namespace midiator
