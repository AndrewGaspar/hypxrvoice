#include "WakeWord.hpp"

#include <algorithm>
#include <cctype>

namespace WakeWord {
    std::vector<std::string> normalize(const std::string& text) {
        std::vector<std::string> toks;
        std::string              cur;
        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc)) {
                cur += static_cast<char>(std::tolower(uc));
            } else if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            toks.push_back(cur);
        return toks;
    }

    int editDistance(const std::string& a, const std::string& b) {
        const size_t     n = a.size(), m = b.size();
        std::vector<int> prev(m + 1), cur(m + 1);
        for (size_t j = 0; j <= m; j++)
            prev[j] = static_cast<int>(j);
        for (size_t i = 1; i <= n; i++) {
            cur[0] = static_cast<int>(i);
            for (size_t j = 1; j <= m; j++) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                cur[j]   = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, cur);
        }
        return prev[m];
    }

    bool matchPrefix(const std::string& transcript, const std::string& phrase, int fuzz, std::string& stripped) {
        std::vector<std::string> tToks = normalize(transcript);
        std::vector<std::string> pToks = normalize(phrase);
        if (pToks.empty()) {
            // No wake phrase configured -> treat everything as command text.
            stripped = transcript;
            return true;
        }
        if (tToks.size() < pToks.size())
            return false;

        // Compare the leading pToks.size() tokens (concatenated) against the phrase.
        std::string lead, want;
        for (const auto& t : pToks)
            want += t;
        for (size_t i = 0; i < pToks.size(); i++)
            lead += tToks[i];

        int budget = std::max(0, fuzz) * static_cast<int>(pToks.size());
        if (editDistance(lead, want) > budget)
            return false;

        // Rebuild the command text from the remaining tokens.
        stripped.clear();
        for (size_t i = pToks.size(); i < tToks.size(); i++) {
            if (!stripped.empty())
                stripped += ' ';
            stripped += tToks[i];
        }
        return true;
    }
}
