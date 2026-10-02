// main.cpp -- command line front end for the Python -> C++ transpiler
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "codegen.h"
#include "lexer.h"
#include "native.h"
#include "parser.h"

namespace {

const char* kUsage =
    "p2cpp - translate a Python subset into C++\n"
    "\n"
    "usage: p2cpp <input.py> [options]\n"
    "\n"
    "By default p2cpp emits ONE file: plain, standard-library C++ with no\n"
    "runtime and no header. It reads the way a programmer would have written\n"
    "it; anything it cannot translate faithfully is reported as an error.\n"
    "\n"
    "options:\n"
    "  -o <file>        write the C++ source to <file> (default: <input>.cpp)\n"
    "  -n, --native     direct C++ (this is now the default; the flag is kept\n"
    "                   for compatibility)\n"
    "  --runtime        maximum-fidelity mode: emit a py:: runtime in a separate\n"
    "                   py_runtime.h next to the .cpp (wider coverage: classes,\n"
    "                   inheritance, **kwargs). This is the old default.\n"
    "  --single-file    inline the whole py:: runtime into the .cpp instead of\n"
    "                   writing py_runtime.h (self-contained output, but ~1400\n"
    "                   lines of runtime around your code)\n"
    "  --no-runtime     do not write py_runtime.h; assume you already have it\n"
    "  --stdout         write the generated C++ to standard output\n"
    "  --run            compile and run the generated program immediately\n"
    "  -c <compiler>    C++ compiler to use with --run (default: c++)\n"
    "  --no-comments    do not turn docstrings into comments\n"
    "  --check          only parse the input, do not generate code\n"
    "  -h, --help       show this message\n"
    "\n"
    "example:\n"
    "  p2cpp script.py                # direct C++, one file\n"
    "  p2cpp script.py --runtime      # maximum fidelity, with py_runtime.h\n"
    "  p2cpp examples/hello.py --run\n";

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

std::string replaceExtension(const std::string& path, const std::string& ext) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + ext;
    return path.substr(0, dot) + ext;
}

// Directory part of `path`, including the trailing slash ("" if there is none).
// The runtime header is written here so that the generated .cpp can reach it
// with a plain #include "py_runtime.h".
std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << text;
    return true;
}

std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'')
            r += "'\\''";
        else
            r += c;
    }
    return r + "'";
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    bool toStdout = false;
    bool runIt = false;
    bool checkOnly = false;
    bool writeHeader = true;
    bool nativeMode = true;  // direct C++ is the default; --runtime opts out
    std::string compiler = "c++";

    CodeGenOptions opt;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << kUsage;
            return 0;
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "p2cpp: -o requires an argument\n";
                return 2;
            }
            output = argv[++i];
        } else if (a == "--stdout") {
            toStdout = true;
        } else if (a == "--run") {
            runIt = true;
        } else if (a == "--check") {
            checkOnly = true;
        } else if (a == "--no-comments") {
            opt.emitComments = false;
        } else if (a == "--single-file") {
            opt.splitRuntime = false;
        } else if (a == "-n" || a == "--native") {
            nativeMode = true;
        } else if (a == "--runtime") {
            nativeMode = false;
        } else if (a == "--no-runtime") {
            // Still #include it -- the user is supplying the header themselves.
            writeHeader = false;
        } else if (a == "-c") {
            if (i + 1 >= argc) {
                std::cerr << "p2cpp: -c requires an argument\n";
                return 2;
            }
            compiler = argv[++i];
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            std::cerr << "p2cpp: unknown option '" << a << "'\n\n" << kUsage;
            return 2;
        } else {
            if (!input.empty()) {
                std::cerr << "p2cpp: only one input file is supported\n";
                return 2;
            }
            input = a;
        }
    }

    if (input.empty()) {
        std::cerr << kUsage;
        return 2;
    }

    bool ok = false;
    std::string source = readFile(input, ok);
    if (!ok) {
        std::cerr << "p2cpp: cannot read '" << input << "'\n";
        return 1;
    }

    // ---- lex ----
    Lexer lexer(source);
    auto tokens = lexer.run();
    if (lexer.failed()) {
        std::cerr << input << ":" << lexer.error() << "\n";
        return 1;
    }

    // ---- parse ----
    std::vector<StmtP> program;
    try {
        Parser parser(std::move(tokens));
        program = parser.parseProgram();
    } catch (const ParseError& e) {
        std::cerr << input << ":" << e.line << ": syntax error: " << e.what() << "\n";
        return 1;
    }

    if (checkOnly) {
        std::cout << "p2cpp: " << input << " parsed successfully\n";
        return 0;
    }

    // ---- rename identifiers that clash with C++ keywords ----
    mangleReservedNames(program);

    // ---- generate ----
    GenResult result;
    try {
        if (nativeMode) {
            NativeResult nr = generateNative(program, opt.emitComments);
            for (const auto& w : nr.warnings) std::cerr << "p2cpp: warning: " << w << "\n";
            if (!nr.ok) {
                for (const auto& e : nr.errors) std::cerr << "p2cpp: error: " << e << "\n";
                std::cerr << "p2cpp: cannot translate this program faithfully; "
                             "add --runtime for maximum-fidelity mode\n";
                return 1;
            }
            result.code = nr.code;
        } else {
            result = generateCpp(program, opt);
        }
    } catch (const std::exception& e) {
        std::cerr << "p2cpp: internal error: " << e.what() << "\n";
        return 1;
    }

    for (const auto& w : result.warnings) std::cerr << "p2cpp: warning: " << w << "\n";

    if (toStdout) {
        std::cout << result.code;
        if (!result.runtimeHeader.empty())
            std::cerr << "p2cpp: note: this source #includes " << kRuntimeHeaderName
                      << "; run without --stdout to write it out\n";
        return 0;
    }

    if (output.empty()) output = replaceExtension(input, ".cpp");
    if (!writeFile(output, result.code)) {
        std::cerr << "p2cpp: cannot write '" << output << "'\n";
        return 1;
    }
    std::cout << "p2cpp: wrote " << output << "\n";

    if (!result.runtimeHeader.empty() && writeHeader) {
        std::string hdr = dirOf(output) + kRuntimeHeaderName;
        if (!writeFile(hdr, result.runtimeHeader)) {
            std::cerr << "p2cpp: cannot write '" << hdr << "'\n";
            return 1;
        }
        std::cout << "p2cpp: wrote " << hdr << "\n";
    }

    if (!runIt) return 0;

    // ---- compile & run ----
    std::string exe = replaceExtension(output, "");
    if (exe.empty()) exe = "a.out";
    exe = "/tmp/p2cpp_build_" + std::to_string(static_cast<long long>(std::rand())) + "_out";

    std::string cmd = compiler + " -std=c++20 -O2 -o " + shellQuote(exe) + " " +
                      shellQuote(output);
    std::cout << "p2cpp: " << cmd << "\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "p2cpp: compilation failed\n";
        return 1;
    }
    std::cout << "p2cpp: ---- program output ----\n";
    rc = std::system(shellQuote(exe).c_str());
    std::remove(exe.c_str());
    if (rc != 0) {
        std::cerr << "p2cpp: program exited with status " << rc << "\n";
        return 1;
    }
    return 0;
}
