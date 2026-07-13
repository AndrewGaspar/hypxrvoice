#pragma once

#include <string>
#include <vector>

// WP-V4: GBNF grammar generation for the local-LLM intent backend. The grammar makes
// it IMPOSSIBLE for the model to emit anything outside the command schema: a
// fixed-key JSON object whose `verb` is one of the closed verb set and whose `target`
// is one of the ENUMERATED live monitor names (or "active"/""). Everything the model
// produces therefore parses, and it can never name a monitor that does not exist.
//
// The emitted object (fixed key order, all keys present) is the backend-neutral raw
// intent; LlamaIntent maps it to SRawIntent and runs the SHARED finalizeAction so
// deixis + validation stay identical to the rule backend.
namespace Grammar {
    // Build a GBNF grammar constraining output to the raw-intent JSON. `monitorNames`
    // are the live, enumerated targets; each is emitted as a quoted literal alternative.
    std::string buildIntentGrammar(const std::vector<std::string>& monitorNames);
}
