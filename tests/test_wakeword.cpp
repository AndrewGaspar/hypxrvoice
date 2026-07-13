#include "doctest.h"

#include "WakeWord.hpp"

TEST_CASE("wakeword: exact prefix match strips the phrase") {
    std::string cmd;
    CHECK(WakeWord::matchPrefix("hey hypr make it bigger", "hey hypr", 2, cmd));
    CHECK(cmd == "make it bigger");
}

TEST_CASE("wakeword: fuzzy match tolerates ASR slips") {
    std::string cmd;
    // "hey hyper" (transcription of "hypr") within the fuzz budget.
    CHECK(WakeWord::matchPrefix("hey hyper drop this monitor here", "hey hypr", 2, cmd));
    CHECK(cmd == "drop this monitor here");
}

TEST_CASE("wakeword: unrelated speech is rejected") {
    std::string cmd;
    CHECK_FALSE(WakeWord::matchPrefix("what time is it", "hey hypr", 2, cmd));
}

TEST_CASE("wakeword: too-short transcript rejected") {
    std::string cmd;
    CHECK_FALSE(WakeWord::matchPrefix("hey", "hey hypr", 2, cmd));
}

TEST_CASE("wakeword: empty phrase passes everything through") {
    std::string cmd;
    CHECK(WakeWord::matchPrefix("just do it", "", 2, cmd));
    CHECK(cmd == "just do it");
}

TEST_CASE("wakeword: edit distance basics") {
    CHECK(WakeWord::editDistance("hypr", "hyper") == 1);
    CHECK(WakeWord::editDistance("cat", "cat") == 0);
    CHECK(WakeWord::editDistance("kitten", "sitting") == 3);
}

TEST_CASE("wakeword: normalize lowercases and splits on punctuation") {
    // "Make-it" splits on the hyphen -> two tokens.
    auto t = WakeWord::normalize("Hey, Hypr! Make-it BIGGER.");
    REQUIRE(t.size() == 5);
    CHECK(t[0] == "hey");
    CHECK(t[1] == "hypr");
    CHECK(t[2] == "make");
    CHECK(t[3] == "it");
    CHECK(t[4] == "bigger");
}
