/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FCITX5_BAMBOO_SURROUNDINGHISTORY_H
#define FCITX5_BAMBOO_SURROUNDINGHISTORY_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <fcitx/surroundingtext.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fcitx {

// Exact snapshots of recent edits, sharing the document outside the active
// word. A large field needs one prefix/suffix copy rather than 32 full copies.
class SurroundingHistory {
public:
    bool empty() const { return states_.empty(); }

    void clear() {
        prefix_.clear();
        suffix_.clear();
        states_.clear();
        next_ = 0;
    }

    // Release the document when focus changes or the fallback takes over.
    void reset() { *this = SurroundingHistory(); }

    void remember(const SurroundingText &surrounding, size_t prefixBytes,
                  size_t suffixStart) {
        const std::string_view text = surrounding.text();
        assert(prefixBytes <= suffixStart && suffixStart <= text.size());
        const auto prefix = text.substr(0, prefixBytes);
        const auto suffix = text.substr(suffixStart);
        if (empty() || prefix != prefix_ || suffix != suffix_) {
            clear();
            prefix_.assign(prefix);
            suffix_.assign(suffix);
            states_.reserve(MaxStates);
        }
        State state{
            std::string(text.substr(prefixBytes, suffixStart - prefixBytes)),
            surrounding.cursor(), surrounding.anchor()};
        if (states_.size() < MaxStates) {
            states_.push_back(std::move(state));
        } else {
            states_[next_] = std::move(state);
            next_ = (next_ + 1) % MaxStates;
        }
    }

    bool matches(const SurroundingText &surrounding) const {
        if (empty() || !surrounding.isValid()) {
            return false;
        }
        const std::string_view text = surrounding.text();
        if (text.size() < prefix_.size() + suffix_.size() ||
            text.substr(0, prefix_.size()) != prefix_ ||
            text.substr(text.size() - suffix_.size()) != suffix_) {
            return false;
        }
        const auto middle = text.substr(
            prefix_.size(), text.size() - prefix_.size() - suffix_.size());
        return std::any_of(states_.begin(), states_.end(),
                           [&surrounding, middle](const State &state) {
                               return state.cursor == surrounding.cursor() &&
                                      state.anchor == surrounding.anchor() &&
                                      middle == state.middle;
                           });
    }

private:
    static constexpr size_t MaxStates = 32;
    struct State {
        std::string middle;
        unsigned int cursor;
        unsigned int anchor;
    };
    std::string prefix_;
    std::string suffix_;
    std::vector<State> states_;
    size_t next_ = 0;
};

} // namespace fcitx
#endif
