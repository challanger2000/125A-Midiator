#pragma once

#include "RiffEngine.h"

#include <array>
#include <cstddef>

namespace midiator {

// One unit equals the current Section Length. Repeated units deliberately
// extend Verse/Chorus/Breakdown without inventing a second unrelated seed.
inline constexpr std::array<SectionType, 16> kDefaultSongFormUnits{{
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

inline SectionType songSectionForUnit(long long unit) noexcept {
    const auto count = static_cast<long long>(kDefaultSongFormUnits.size());
    long long wrapped = unit % count;
    if (wrapped < 0)
        wrapped += count;
    return kDefaultSongFormUnits[static_cast<std::size_t>(wrapped)];
}

} // namespace midiator
