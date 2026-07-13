#include "doctest.h"

#include "Transcript.hpp"

#include <string>

TEST_CASE("transcript: JSON carries onset + words") {
    STranscript t;
    t.text       = "make it bigger";
    t.onsetMs    = 123456;
    t.endMs      = 124000;
    t.activation = EActivation::WakeWord;
    t.wakePhrase = "hey hypr";
    t.words.push_back({"make", 123500, 123700, 0.9f});
    t.words.push_back({"it", 123720, 123800, 0.8f});
    t.words.push_back({"bigger", 123820, 124000, 0.95f});

    std::string j = t.toJson();
    CHECK(j.find("\"onsetMs\":123456") != std::string::npos);
    CHECK(j.find("\"activation\":\"wake\"") != std::string::npos);
    CHECK(j.find("\"wakePhrase\":\"hey hypr\"") != std::string::npos);
    CHECK(j.find("\"text\":\"make\"") != std::string::npos);
    CHECK(j.find("\"startMs\":123500") != std::string::npos);
}

TEST_CASE("transcript: JSON escapes special characters") {
    STranscript t;
    t.text = "say \"hi\"\nnow";
    std::string j = t.toJson();
    CHECK(j.find("\\\"hi\\\"") != std::string::npos);
    CHECK(j.find("\\n") != std::string::npos);
}
