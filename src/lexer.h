// lexer.h -- indentation aware tokenizer for the Python subset
#pragma once

#include <string>
#include <vector>

enum class Tok {
    End,
    Newline,
    Indent,
    Dedent,
    Name,
    Int,
    Float,
    Str,     // regular string literal, text = decoded value
    FStr,    // f-string, text = raw source between the quotes
    Op,
    Keyword,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;   // name / op / keyword text, or string payload
    long long i = 0;    // integer literal value
    double d = 0;       // float literal value
    int line = 1;
    int col = 1;

    bool is(Tok k) const { return kind == k; }
    bool is(Tok k, const std::string& t) const { return kind == k && text == t; }
};

class Lexer {
public:
    explicit Lexer(std::string source) : src(std::move(source)) {}

    std::vector<Token> run();

    bool failed() const { return hasError; }
    const std::string& error() const { return err; }

private:
    std::string src;
    size_t pos = 0;
    int line = 1;
    int col = 1;
    int parenDepth = 0;
    std::vector<int> indents;
    bool hasError = false;
    std::string err;

    void fail(const std::string& msg);

    char peek(size_t off = 0) const {
        return pos + off < src.size() ? src[pos + off] : '\0';
    }
};
