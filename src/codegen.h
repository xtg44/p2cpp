// codegen.h -- Python AST -> C++ source
#pragma once

#include <string>
#include <vector>

#include "ast.h"

struct CodeGenOptions {
    // The emitted code calls into the py:: runtime.
    bool includeRuntime = true;
    // Write the runtime to a sibling header and #include it, rather than
    // inlining ~1400 lines of runtime into every generated translation unit.
    // Inlining buries the translated program under a wall of library code and
    // forces a full re-parse of the runtime on every compile.
    bool splitRuntime = true;
    bool emitComments = true;
};

struct GenResult {
    std::string code;
    // Non-empty when splitRuntime is on: the text of the runtime header. The
    // caller is expected to write this next to `code` (see kRuntimeHeaderName).
    std::string runtimeHeader;
    std::vector<std::string> warnings;
};

// Name of the header that generated code #includes when splitRuntime is on.
extern const char* const kRuntimeHeaderName;

GenResult generateCpp(const std::vector<StmtP>& program, const CodeGenOptions& opt);

// Rename Python identifiers that collide with C++ keywords or with names the
// runtime/emitted code already uses (e.g. a function named `double` becomes
// `double_`). Must be called on the AST before generateCpp().
void mangleReservedNames(std::vector<StmtP>& program);
