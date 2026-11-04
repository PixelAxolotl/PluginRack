#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace pluginrack {

// WASAPI and ASIO name the same hardware differently:
//
//   WASAPI: "Speakers (Focusrite USB Audio)"
//   ASIO:   "Focusrite USB ASIO"
//
// "Force ASIO" has to bridge that gap without asking the user to retype a
// device name, but it must never guess wildly — picking the wrong interface is
// worse than saying "choose one". This returns the best candidate, or an empty
// string when nothing is convincing.
//
// Scoring: shared meaningful words, longer words weighted more, with the words
// every endpoint has ("audio", "usb", "speaker", ...) ignored so two unrelated
// USB interfaces do not match on those alone.
inline std::string bestAsioDeviceMatch(std::string_view currentDeviceName,
                                       const std::vector<std::string>& asioDevices) {
    const auto isWord = [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0;
    };
    const auto tokenize = [&isWord](std::string_view text) {
        std::vector<std::string> tokens;
        std::string word;
        for (const char c : text) {
            if (isWord(c)) {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            } else if (!word.empty()) {
                tokens.push_back(word);
                word.clear();
            }
        }
        if (!word.empty()) tokens.push_back(word);
        return tokens;
    };
    const auto isGeneric = [](const std::string& word) {
        static const std::vector<std::string> generic{
            "asio", "audio", "usb", "driver", "windows", "speakers", "speaker",
            "headphones", "microphone", "line", "output", "input", "device",
            "default", "primary", "digital", "sound", "high", "definition"};
        return std::find(generic.begin(), generic.end(), word) != generic.end();
    };

    const auto currentTokens = tokenize(currentDeviceName);
    int bestScore = 0;
    std::string best;

    for (const auto& candidate : asioDevices) {
        int score = 0;
        for (const auto& token : tokenize(candidate)) {
            if (token.size() < 3 || isGeneric(token)) continue;
            if (std::find(currentTokens.begin(), currentTokens.end(), token) != currentTokens.end()) {
                score += static_cast<int>(token.size());
            }
        }
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return bestScore > 0 ? best : std::string{};
}

} // namespace pluginrack
