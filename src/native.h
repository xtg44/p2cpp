// native.h -- direct Python -> C++ backend
//
// Unlike the default backend, this one emits plain C++ that uses nothing but
// the standard library: no py:: runtime, no support header, no helper file.
// What you get is the program as a C++ programmer would have written it.
//
// The price is that a few Python behaviours cannot be reproduced by the
// standard library alone (float repr, dict insertion order, ...). Those are
// reported as warnings, and constructs that would silently change behaviour
// are reported as errors instead of being translated incorrectly.
#pragma once

#include <string>
#include <vector>

#include "ast.h"

struct NativeResult {
    // False when at least one construct could not be translated faithfully.
    // `code` is then incomplete and must not be used.
    bool ok = true;
    std::string code;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;  // each entry is "line N: reason"
};

NativeResult generateNative(const std::vector<StmtP>& program, bool emitComments);
