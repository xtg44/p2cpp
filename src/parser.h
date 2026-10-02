// parser.h -- recursive descent parser for the supported Python subset
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& msg, int ln) : std::runtime_error(msg), line(ln) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks(std::move(toks)) {}

    std::vector<StmtP> parseProgram();

    // helpers used for f-string holes
    static std::vector<StmtP> parseSource(const std::string& src);
    static ExprP parseExprSource(const std::string& src, int line);

private:
    std::vector<Token> toks;
    size_t p = 0;
    bool noInOperator = false;  // set while parsing `for ... in` target lists

    // ---- token helpers ----
    const Token& cur() const { return toks[p]; }
    const Token& ahead(size_t n = 1) const {
        size_t i = p + n;
        return toks[i < toks.size() ? i : toks.size() - 1];
    }
    void adv() {
        if (p + 1 < toks.size()) p++;
    }
    bool isOp(const std::string& s) const { return cur().kind == Tok::Op && cur().text == s; }
    bool isKw(const std::string& s) const {
        return cur().kind == Tok::Keyword && cur().text == s;
    }
    bool isName() const { return cur().kind == Tok::Name; }
    bool accOp(const std::string& s) {
        if (isOp(s)) {
            adv();
            return true;
        }
        return false;
    }
    bool accKw(const std::string& s) {
        if (isKw(s)) {
            adv();
            return true;
        }
        return false;
    }
    [[noreturn]] void err(const std::string& m) const { throw ParseError(m, cur().line); }
    void expOp(const std::string& s) {
        if (!accOp(s)) err("expected '" + s + "' but found '" + cur().text + "'");
    }
    std::string expName();
    void expNewline();
    void skipNewlines() {
        while (cur().kind == Tok::Newline) adv();
    }

    // ---- statements ----
    std::vector<StmtP> parseStatement();
    std::vector<StmtP> parseSimpleLine();
    StmtP parseSimpleStatement();
    std::vector<StmtP> parseBlock();

    StmtP parseIf();
    StmtP parseWhile();
    StmtP parseFor();
    StmtP parseFuncDef(std::vector<std::string> decorators);
    StmtP parseClassDef(std::vector<std::string> decorators);
    StmtP parseTry();
    StmtP parseImport();
    StmtP parseFromImport();
    StmtP parseWith();
    StmtP parseDecorated();
    StmtP parseRaise();

    std::vector<Param> parseParamList(bool& hasSelf);

    // ---- expressions ----
    ExprP parseTest();
    ExprP parseOr();
    ExprP parseAnd();
    ExprP parseNot();
    ExprP parseComparison();
    ExprP parseBitOr();
    ExprP parseBitXor();
    ExprP parseBitAnd();
    ExprP parseShift();
    ExprP parseArith();
    ExprP parseTerm();
    ExprP parseFactor();
    ExprP parsePower();
    ExprP parsePostfix();
    ExprP parseAtom();
    ExprP parseLambda();
    ExprP parseListOrComp();
    ExprP parseDictOrSetOrComp();
    ExprP parseParenAtom();

    std::vector<ExprP> parseStarExprList(const char* closer);
    std::vector<ExprP> parseTargetList();
    ExprP wrapTuple(std::vector<ExprP> v, int line);
    ExprP finishConditional(ExprP e);
    ExprP buildComprehension(ExprP element, EK kind, int ln);

    void parseArgList(ExprP& call);
    ExprP parseFString(const Token& t);
    ExprP parseSubscript(ExprP base);
};
