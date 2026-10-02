// lexer.cpp -- indentation aware tokenizer for the Python subset
#include "lexer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace {

const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> k = {
        "False", "None",  "True",  "and",    "as",     "assert", "async",
        "await", "break", "class", "continue", "def",  "del",    "elif",
        "else",  "except", "finally", "for",  "from",   "global", "if",
        "import", "in",   "is",    "lambda", "nonlocal", "not",  "or",
        "pass",  "raise", "return", "try",   "while",  "with",   "yield",
    };
    return k;
}

const std::vector<std::string>& operators() {
    // ordered longest first so the greedy match is correct
    static const std::vector<std::string> ops = {
        "**=", "//=", ">>=", "<<=", "...", "!=", "==", ">=", "<=", "->",
        ":=",  "**",  "//",  "<<",  ">>",  "+=", "-=", "*=", "/=", "%=",
        "&=",  "|=",  "^=",  "@=",  "+",   "-",  "*",  "/",  "%",  "@",
        "&",   "|",   "^",   "~",   "<",   ">",  "=",  "(",  ")",  "[",
        "]",   "{",   "}",   ",",   ":",   ".",  ";",
    };
    return ops;
}

bool isIdStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

}  // namespace

void Lexer::fail(const std::string& msg) {
    if (!hasError) {
        hasError = true;
        err = "line " + std::to_string(line) + ": " + msg;
    }
}

std::vector<Token> Lexer::run() {
    std::vector<Token> out;
    indents.push_back(0);

    bool lineStart = true;

    auto push = [&](Tok k, std::string text, int ln) {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.line = ln;
        out.push_back(std::move(t));
    };

    while (pos < src.size()) {
        if (lineStart && parenDepth == 0) {
            size_t p = pos;
            int indent = 0;
            while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) {
                if (src[p] == '\t')
                    indent = (indent / 8 + 1) * 8;
                else
                    indent++;
                p++;
            }
            if (p >= src.size()) {
                pos = p;
                break;
            }
            if (src[p] == '\n' || src[p] == '\r' || src[p] == '#') {
                // blank line or comment-only line: no INDENT/DEDENT effect
                while (p < src.size() && src[p] != '\n') p++;
                if (p < src.size()) {
                    p++;
                    line++;
                }
                pos = p;
                continue;
            }
            if (indent > indents.back()) {
                indents.push_back(indent);
                push(Tok::Indent, "", line);
            } else if (indent < indents.back()) {
                while (indents.size() > 1 && indents.back() > indent) {
                    indents.pop_back();
                    push(Tok::Dedent, "", line);
                }
                if (indents.back() != indent) fail("inconsistent indentation");
            }
            pos = p;
            col = indent + 1;
            lineStart = false;
        }

        if (pos >= src.size()) break;
        char c = src[pos];

        if (c == '\n') {
            pos++;
            if (parenDepth == 0) {
                if (!out.empty() && out.back().kind != Tok::Newline &&
                    out.back().kind != Tok::Indent && out.back().kind != Tok::Dedent) {
                    Token t;
                    t.kind = Tok::Newline;
                    t.line = line;
                    out.push_back(t);
                }
                lineStart = true;
                line++;
            }
            col = 1;
            continue;
        }
        if (c == '\r') {
            pos++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            pos++;
            col++;
            continue;
        }
        if (c == '\\' && peek(1) == '\n') {
            pos += 2;
            line++;
            col = 1;
            continue;
        }
        if (c == '#') {
            while (pos < src.size() && src[pos] != '\n') pos++;
            continue;
        }

        const int tokLine = line;

        // ---------- string literals (with optional prefixes) ----------
        {
            size_t sp = pos;
            bool isRaw = false, isF = false, isBytes = false;
            size_t q = pos;
            while (q < src.size() && q - pos < 2 &&
                   (src[q] == 'r' || src[q] == 'R' || src[q] == 'f' || src[q] == 'F' ||
                    src[q] == 'b' || src[q] == 'B' || src[q] == 'u' || src[q] == 'U')) {
                if (src[q] == 'r' || src[q] == 'R') isRaw = true;
                if (src[q] == 'f' || src[q] == 'F') isF = true;
                if (src[q] == 'b' || src[q] == 'B') isBytes = true;
                q++;
            }
            // guard against a plain identifier such as `f` or `from`
            bool quoteAhead = (q < src.size() && (src[q] == '"' || src[q] == '\''));
            if (quoteAhead && (isRaw || isF || isBytes || q == pos)) {
                char quote = src[q];
                bool triple = (q + 2 < src.size() && src[q + 1] == quote && src[q + 2] == quote);
                size_t bodyStart = q + (triple ? 3 : 1);
                size_t i = bodyStart;
                std::string rawText;
                bool closed = false;
                while (i < src.size()) {
                    if (!isRaw && src[i] == '\\') {
                        rawText.push_back(src[i]);
                        if (i + 1 < src.size()) rawText.push_back(src[i + 1]);
                        i += 2;
                        continue;
                    }
                    if (src[i] == quote) {
                        if (triple) {
                            if (i + 2 < src.size() && src[i + 1] == quote && src[i + 2] == quote) {
                                i += 3;
                                closed = true;
                                break;
                            }
                            rawText.push_back(src[i]);
                            i++;
                            continue;
                        }
                        i += 1;
                        closed = true;
                        break;
                    }
                    if (src[i] == '\n') {
                        if (!triple) {
                            fail("unterminated string literal");
                            break;
                        }
                        line++;
                    }
                    rawText.push_back(src[i]);
                    i++;
                }
                if (!closed && !hasError) fail("unterminated string literal");

                Token t;
                t.line = tokLine;
                t.text = rawText;
                t.kind = isF ? Tok::FStr : Tok::Str;
                t.col = static_cast<int>(sp - pos) + 1;
                if (!isF && !isRaw && !isBytes) {
                    // decode escapes now
                    std::string decoded;
                    for (size_t k = 0; k < rawText.size(); ++k) {
                        if (rawText[k] != '\\' || k + 1 >= rawText.size()) {
                            decoded.push_back(rawText[k]);
                            continue;
                        }
                        char e = rawText[++k];
                        switch (e) {
                            case 'n': decoded.push_back('\n'); break;
                            case 't': decoded.push_back('\t'); break;
                            case 'r': decoded.push_back('\r'); break;
                            case '0': decoded.push_back('\0'); break;
                            case 'a': decoded.push_back('\a'); break;
                            case 'b': decoded.push_back('\b'); break;
                            case 'f': decoded.push_back('\f'); break;
                            case 'v': decoded.push_back('\v'); break;
                            case '\\': decoded.push_back('\\'); break;
                            case '\'': decoded.push_back('\''); break;
                            case '"': decoded.push_back('"'); break;
                            case '\n': break;  // line continuation
                            case 'x': {
                                int v = 0, n = 0;
                                while (n < 2 && k + 1 < rawText.size() &&
                                       std::isxdigit(static_cast<unsigned char>(rawText[k + 1]))) {
                                    char h = rawText[++k];
                                    v = v * 16 + (std::isdigit(static_cast<unsigned char>(h))
                                                      ? h - '0'
                                                      : (std::tolower(h) - 'a' + 10));
                                    n++;
                                }
                                decoded.push_back(static_cast<char>(v));
                                break;
                            }
                            default: decoded.push_back('\\'); decoded.push_back(e); break;
                        }
                    }
                    t.text = decoded;
                }
                out.push_back(std::move(t));
                pos = i;
                col += static_cast<int>(i - sp);
                continue;
            }
        }

        // ---------- numbers ----------
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            size_t start = pos;
            bool isFloat = false;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' ||
                             peek(1) == 'O' || peek(1) == 'b' || peek(1) == 'B')) {
                int base = (peek(1) == 'x' || peek(1) == 'X') ? 16
                           : (peek(1) == 'o' || peek(1) == 'O') ? 8
                                                                : 2;
                pos += 2;
                long long v = 0;
                while (pos < src.size() && (std::isalnum(static_cast<unsigned char>(src[pos])) ||
                                            src[pos] == '_')) {
                    char h = src[pos++];
                    if (h == '_') continue;
                    int digit = std::isdigit(static_cast<unsigned char>(h))
                                    ? h - '0'
                                    : (std::tolower(h) - 'a' + 10);
                    if (digit >= base) {
                        fail("invalid digit in numeric literal");
                        break;
                    }
                    v = v * base + digit;
                }
                Token t;
                t.kind = Tok::Int;
                t.i = v;
                t.text = src.substr(start, pos - start);
                t.line = tokLine;
                out.push_back(std::move(t));
                col += static_cast<int>(pos - start);
                continue;
            }
            while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                                        src[pos] == '_')) {
                if (src[pos] == '.') break;
                pos++;
            }
            if (pos < src.size() && src[pos] == '.') {
                isFloat = true;
                pos++;
                while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                                            src[pos] == '_'))
                    pos++;
            }
            if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
                size_t save = pos;
                size_t p2 = pos + 1;
                if (p2 < src.size() && (src[p2] == '+' || src[p2] == '-')) p2++;
                if (p2 < src.size() && std::isdigit(static_cast<unsigned char>(src[p2]))) {
                    isFloat = true;
                    pos = p2;
                    while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos])))
                        pos++;
                } else {
                    pos = save;
                }
            }
            std::string raw = src.substr(start, pos - start);
            std::string clean;
            for (char ch : raw)
                if (ch != '_') clean.push_back(ch);
            Token t;
            t.line = tokLine;
            if (isFloat || (!clean.empty() && clean.back() == 'j')) {
                t.kind = Tok::Float;
                t.d = std::strtod(clean.c_str(), nullptr);
            } else {
                t.kind = Tok::Int;
                t.i = std::strtoll(clean.c_str(), nullptr, 10);
            }
            t.text = raw;
            out.push_back(std::move(t));
            col += static_cast<int>(pos - start);
            continue;
        }

        // ---------- identifiers ----------
        if (isIdStart(c) || static_cast<unsigned char>(c) >= 0x80) {
            size_t start = pos;
            while (pos < src.size() && (isIdChar(src[pos]) ||
                                        static_cast<unsigned char>(src[pos]) >= 0x80))
                pos++;
            std::string name = src.substr(start, pos - start);
            Tok k = keywords().count(name) ? Tok::Keyword : Tok::Name;
            push(k, name, tokLine);
            col += static_cast<int>(pos - start);
            continue;
        }

        // ---------- operators ----------
        {
            bool matched = false;
            for (const auto& op : operators()) {
                if (src.compare(pos, op.size(), op) == 0) {
                    if (op == "(" || op == "[" || op == "{") parenDepth++;
                    if (op == ")" || op == "]" || op == "}") parenDepth = std::max(0, parenDepth - 1);
                    push(Tok::Op, op, tokLine);
                    pos += op.size();
                    col += static_cast<int>(op.size());
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        fail(std::string("unexpected character '") + c + "'");
        pos++;
    }

    // final NEWLINE + DEDENTs
    if (!out.empty() && out.back().kind != Tok::Newline && out.back().kind != Tok::Indent &&
        out.back().kind != Tok::Dedent) {
        Token t;
        t.kind = Tok::Newline;
        t.line = line;
        out.push_back(t);
    }
    while (indents.size() > 1) {
        indents.pop_back();
        Token t;
        t.kind = Tok::Dedent;
        t.line = line;
        out.push_back(t);
    }
    Token e;
    e.kind = Tok::End;
    e.line = line;
    out.push_back(e);
    return out;
}
