// parser.cpp -- recursive descent parser for the supported Python subset
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static std::string trimCopy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// Render a simple expression back to source-ish text (decorators, except types).
static std::string exprToText(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case EK::Name:
            return e->s;
        case EK::Attr:
            return exprToText(e->a.get()) + "." + e->s;
        case EK::Call: {
            std::string r = exprToText(e->a.get()) + "(";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) r += ", ";
                r += exprToText(e->items[i].get());
            }
            for (auto& kv : e->kwargs) r += ", " + kv.first + "=" + exprToText(kv.second.get());
            return r + ")";
        }
        case EK::TupleLit: {
            std::string r = "(";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) r += ", ";
                r += exprToText(e->items[i].get());
            }
            return r + ")";
        }
        default:
            return "";
    }
}

static const char* AUG_OPS[] = {"+=", "-=", "*=", "/=", "//=", "%=",
                                "**=", "&=", "|=", "^=", ">>=", "<<=", nullptr};

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------
std::vector<StmtP> Parser::parseProgram() {
    std::vector<StmtP> res;
    skipNewlines();
    while (cur().kind != Tok::End) {
        auto stmts = parseStatement();
        for (auto& s : stmts) res.push_back(std::move(s));
        skipNewlines();
    }
    return res;
}

std::vector<StmtP> Parser::parseSource(const std::string& src) {
    Lexer lx(src);
    auto toks = lx.run();
    if (lx.failed()) throw ParseError(lx.error(), 0);
    Parser p(std::move(toks));
    return p.parseProgram();
}

ExprP Parser::parseExprSource(const std::string& src, int line) {
    Lexer lx(src);
    auto toks = lx.run();
    if (lx.failed()) throw ParseError(lx.error(), line);
    Parser p(std::move(toks));
    ExprP e = p.parseTest();
    p.skipNewlines();
    if (p.cur().kind != Tok::End)
        throw ParseError("cannot parse expression '" + src + "'", line);
    return e;
}

std::string Parser::expName() {
    if (cur().kind != Tok::Name) err("expected an identifier but found '" + cur().text + "'");
    std::string s = cur().text;
    adv();
    return s;
}

void Parser::expNewline() {
    if (cur().kind == Tok::Newline) {
        adv();
        return;
    }
    if (cur().kind == Tok::End || cur().kind == Tok::Dedent) return;
    err("unexpected '" + cur().text + "' at end of statement");
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------
std::vector<StmtP> Parser::parseStatement() {
    std::vector<StmtP> res;
    if (cur().kind == Tok::Keyword) {
        const std::string k = cur().text;
        if (k == "if") {
            res.push_back(parseIf());
            return res;
        }
        if (k == "while") {
            res.push_back(parseWhile());
            return res;
        }
        if (k == "for") {
            res.push_back(parseFor());
            return res;
        }
        if (k == "try") {
            res.push_back(parseTry());
            return res;
        }
        if (k == "with") {
            res.push_back(parseWith());
            return res;
        }
        if (k == "def") {
            res.push_back(parseFuncDef({}));
            return res;
        }
        if (k == "class") {
            res.push_back(parseClassDef({}));
            return res;
        }
        if (k == "import") {
            res.push_back(parseImport());
            return res;
        }
        if (k == "from") {
            res.push_back(parseFromImport());
            return res;
        }
        if (k == "async") {
            adv();
            return parseStatement();
        }
        if (k == "elif" || k == "else" || k == "except" || k == "finally")
            err("unexpected '" + k + "'");
    }
    if (isOp("@")) {
        res.push_back(parseDecorated());
        return res;
    }
    return parseSimpleLine();
}

StmtP Parser::parseDecorated() {
    std::vector<std::string> decs;
    while (isOp("@")) {
        adv();
        ExprP d = parseTest();
        decs.push_back(exprToText(d.get()));
        expNewline();
    }
    if (isKw("def")) return parseFuncDef(decs);
    if (isKw("class")) return parseClassDef(decs);
    err("expected 'def' or 'class' after a decorator");
}

std::vector<StmtP> Parser::parseSimpleLine() {
    std::vector<StmtP> res;
    while (true) {
        res.push_back(parseSimpleStatement());
        if (accOp(";")) {
            if (cur().kind == Tok::Newline || cur().kind == Tok::End) break;
            continue;
        }
        break;
    }
    expNewline();
    return res;
}

StmtP Parser::parseRaise() {
    int ln = cur().line;
    adv();
    auto s = mkStmt(SK::Raise, ln);
    if (cur().kind != Tok::Newline && cur().kind != Tok::End && cur().kind != Tok::Dedent &&
        !isOp(";"))
        s->a = parseTest();
    return s;
}

StmtP Parser::parseSimpleStatement() {
    const int ln = cur().line;

    if (cur().kind == Tok::Keyword) {
        const std::string k = cur().text;

        if (k == "pass") {
            adv();
            return mkStmt(SK::Pass, ln);
        }
        if (k == "break") {
            adv();
            return mkStmt(SK::Break, ln);
        }
        if (k == "continue") {
            adv();
            return mkStmt(SK::Continue, ln);
        }
        if (k == "return") {
            adv();
            auto s = mkStmt(SK::Return, ln);
            if (cur().kind != Tok::Newline && cur().kind != Tok::End &&
                cur().kind != Tok::Dedent && !isOp(";")) {
                auto v = parseStarExprList("return");
                s->a = wrapTuple(std::move(v), ln);
            }
            return s;
        }
        if (k == "raise") return parseRaise();
        if (k == "global" || k == "nonlocal") {
            adv();
            auto s = mkStmt(k == "global" ? SK::Global : SK::Nonlocal, ln);
            while (true) {
                if (!s->s.empty()) s->s += ",";
                s->s += expName();
                if (!accOp(",")) break;
            }
            return s;
        }
        if (k == "del") {
            adv();
            auto s = mkStmt(SK::Del, ln);
            s->a = parseTest();
            while (accOp(",")) parseTest();
            return s;
        }
        if (k == "assert") {
            adv();
            auto s = mkStmt(SK::Assert, ln);
            s->a = parseTest();
            if (accOp(",")) s->b = parseTest();
            return s;
        }
        if (k == "import") return parseImport();
        if (k == "from") return parseFromImport();
        if (k == "yield") {
            adv();
            auto s = mkStmt(SK::ExprStmt, ln);
            s->s = "yield";  // codegen reports generators as unsupported
            if (cur().kind != Tok::Newline && cur().kind != Tok::End) s->a = parseTest();
            return s;
        }
        if (k == "await") {
            adv();
            auto s = mkStmt(SK::ExprStmt, ln);
            s->a = parseTest();
            return s;
        }
    }

    // expression / assignment
    auto first = parseTargetList();
    ExprP lhs = wrapTuple(std::move(first), ln);

    if (isOp(":")) {
        adv();
        auto s = mkStmt(SK::AnnAssign, ln);
        s->a = std::move(lhs);
        s->b = parseTest();
        if (accOp("=")) {
            auto v = parseStarExprList("=");
            s->c = wrapTuple(std::move(v), ln);
        }
        return s;
    }

    for (int i = 0; AUG_OPS[i]; ++i) {
        if (isOp(AUG_OPS[i])) {
            std::string op = AUG_OPS[i];
            adv();
            auto s = mkStmt(SK::AugAssign, ln);
            s->s = op;
            s->a = std::move(lhs);
            auto v = parseStarExprList("=");
            s->b = wrapTuple(std::move(v), ln);
            return s;
        }
    }

    if (isOp("=")) {
        auto s = mkStmt(SK::Assign, ln);
        s->targets.push_back(std::move(lhs));
        while (accOp("=")) {
            auto more = parseTargetList();
            ExprP e = wrapTuple(std::move(more), ln);
            if (isOp("="))
                s->targets.push_back(std::move(e));
            else
                s->values.push_back(std::move(e));
        }
        return s;
    }

    auto s = mkStmt(SK::ExprStmt, ln);
    s->a = std::move(lhs);
    return s;
}

std::vector<StmtP> Parser::parseBlock() {
    std::vector<StmtP> res;
    if (cur().kind == Tok::Newline) {
        adv();
        if (cur().kind != Tok::Indent) {
            // empty block at EOF
            return res;
        }
        adv();
        while (cur().kind != Tok::Dedent && cur().kind != Tok::End) {
            if (cur().kind == Tok::Newline) {
                adv();
                continue;
            }
            auto s = parseStatement();
            for (auto& x : s) res.push_back(std::move(x));
        }
        if (cur().kind == Tok::Dedent) adv();
    } else {
        auto s = parseSimpleLine();
        for (auto& x : s) res.push_back(std::move(x));
    }
    return res;
}

StmtP Parser::parseIf() {
    const int ln = cur().line;
    adv();  // 'if' or 'elif'
    auto s = mkStmt(SK::If, ln);
    s->a = parseTest();
    expOp(":");
    s->body = parseBlock();
    if (isKw("elif")) {
        s->orelse.push_back(parseIf());
    } else if (isKw("else")) {
        adv();
        expOp(":");
        s->orelse = parseBlock();
    }
    return s;
}

StmtP Parser::parseWhile() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::While, ln);
    s->a = parseTest();
    expOp(":");
    s->body = parseBlock();
    if (isKw("else")) {
        adv();
        expOp(":");
        s->orelse = parseBlock();
    }
    return s;
}

StmtP Parser::parseFor() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::For, ln);
    auto targets = parseStarExprList("for");
    s->a = wrapTuple(std::move(targets), ln);
    if (!accKw("in")) err("expected 'in' in for statement");
    auto it = parseStarExprList("for");
    s->iter = wrapTuple(std::move(it), ln);
    expOp(":");
    s->body = parseBlock();
    if (isKw("else")) {
        adv();
        expOp(":");
        s->orelse = parseBlock();
    }
    return s;
}

std::vector<Param> Parser::parseParamList(bool& hasSelf) {
    std::vector<Param> out;
    while (!isOp(")")) {
        Param p;
        if (isOp("*")) {
            adv();
            p.isStar = true;
            if (cur().kind == Tok::Name) {
                p.name = cur().text;
                adv();
                if (accOp(":")) parseTest();
                if (accOp("=")) p.def = parseTest();
            }
        } else if (isOp("**")) {
            adv();
            p.isKwStar = true;
            if (cur().kind == Tok::Name) {
                p.name = cur().text;
                adv();
                if (accOp(":")) parseTest();
            }
        } else if (isOp("/")) {
            adv();
            continue;
        } else {
            p.name = expName();
            if (accOp(":")) parseTest();
            if (accOp("=")) p.def = parseTest();
        }
        if (out.empty() && p.name == "self") hasSelf = true;
        out.push_back(std::move(p));
        if (!accOp(",")) break;
    }
    return out;
}

StmtP Parser::parseFuncDef(std::vector<std::string> decorators) {
    const int ln = cur().line;
    adv();  // 'def'
    auto s = mkStmt(SK::FuncDef, ln);
    s->decorators = std::move(decorators);
    s->s = expName();
    expOp("(");
    bool hasSelf = false;
    s->params = parseParamList(hasSelf);
    expOp(")");
    if (accOp("->")) parseTest();  // return annotation, ignored
    expOp(":");
    s->body = parseBlock();
    return s;
}

StmtP Parser::parseClassDef(std::vector<std::string> decorators) {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::ClassDef, ln);
    s->decorators = std::move(decorators);
    s->s = expName();
    if (accOp("(")) {
        while (!isOp(")")) {
            ExprP b = parseTest();
            if (b->kind == EK::Name) s->bases.push_back(b->s);
            if (!accOp(",")) break;
        }
        expOp(")");
    }
    expOp(":");
    s->body = parseBlock();
    return s;
}

StmtP Parser::parseTry() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::Try, ln);
    expOp(":");
    s->body = parseBlock();
    while (isKw("except")) {
        adv();
        Handler h;
        if (isOp("*")) adv();
        if (!isOp(":")) {
            ExprP t = parseTest();
            h.type = exprToText(t.get());
        }
        if (accKw("as")) h.name = expName();
        expOp(":");
        h.body = parseBlock();
        s->handlers.push_back(std::move(h));
    }
    if (isKw("else")) {
        adv();
        expOp(":");
        s->orelse = parseBlock();
    }
    if (isKw("finally")) {
        adv();
        expOp(":");
        s->finalbody = parseBlock();
    }
    return s;
}

StmtP Parser::parseImport() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::Import, ln);
    while (true) {
        std::string mod;
        if (isOp(".")) {  // relative import
            while (accOp(".")) mod += ".";
        }
        mod += expName();
        while (accOp(".")) mod += "." + expName();
        std::string alias;
        if (accKw("as")) alias = expName();
        s->imports.push_back({mod, alias});
        if (!accOp(",")) break;
    }
    return s;
}

StmtP Parser::parseFromImport() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::FromImport, ln);
    std::string mod;
    while (accOp(".")) mod += ".";
    if (cur().kind == Tok::Name) {
        mod += expName();
        while (accOp(".")) mod += "." + expName();
    }
    s->s = mod;
    if (!accKw("import")) err("expected 'import'");
    if (isOp("*")) {
        adv();
        s->imports.push_back({"*", ""});
        return s;
    }
    bool paren = accOp("(");
    while (true) {
        std::string nm = expName();
        std::string alias;
        if (accKw("as")) alias = expName();
        s->imports.push_back({nm, alias});
        if (!accOp(",")) break;
        if (isOp(")")) break;
    }
    if (paren) expOp(")");
    return s;
}

StmtP Parser::parseWith() {
    const int ln = cur().line;
    adv();
    auto s = mkStmt(SK::With, ln);
    s->a = parseTest();
    if (accKw("as")) s->s = expName();
    expOp(":");
    s->body = parseBlock();
    return s;
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
ExprP Parser::parseTest() {
    if (isKw("lambda")) return parseLambda();
    ExprP e = parseOr();
    return finishConditional(std::move(e));
}

ExprP Parser::finishConditional(ExprP e) {
    if (!isKw("if")) return e;
    const int ln = cur().line;
    adv();
    ExprP cond = parseOr();
    if (!accKw("else")) err("expected 'else' in conditional expression");
    ExprP orelse = parseTest();
    auto r = mkExpr(EK::IfExp, ln);
    r->a = std::move(e);
    r->b = std::move(cond);
    r->c = std::move(orelse);
    return r;
}

ExprP Parser::parseOr() {
    const int ln = cur().line;
    ExprP left = parseAnd();
    if (!isKw("or")) return left;
    auto r = mkExpr(EK::BoolOp, ln);
    r->s = "or";
    r->items.push_back(std::move(left));
    while (accKw("or")) r->items.push_back(parseAnd());
    return r;
}

ExprP Parser::parseAnd() {
    const int ln = cur().line;
    ExprP left = parseNot();
    if (!isKw("and")) return left;
    auto r = mkExpr(EK::BoolOp, ln);
    r->s = "and";
    r->items.push_back(std::move(left));
    while (accKw("and")) r->items.push_back(parseNot());
    return r;
}

ExprP Parser::parseNot() {
    if (isKw("not")) {
        const int ln = cur().line;
        adv();
        auto e = mkExpr(EK::UnaryOp, ln);
        e->s = "not";
        e->a = parseNot();
        return e;
    }
    return parseComparison();
}

ExprP Parser::parseComparison() {
    const int ln = cur().line;
    ExprP left = parseBitOr();
    std::vector<ExprP> items;
    std::vector<std::string> ops;

    while (true) {
        std::string op;
        if (isOp("==") || isOp("!=") || isOp("<") || isOp("<=") || isOp(">") || isOp(">=")) {
            op = cur().text;
            adv();
        } else if (!noInOperator && isKw("in")) {
            op = "in";
            adv();
        } else if (!noInOperator && isKw("not") && ahead().kind == Tok::Keyword &&
                   ahead().text == "in") {
            op = "not in";
            adv();
            adv();
        } else if (isKw("is")) {
            adv();
            if (isKw("not")) {
                adv();
                op = "is not";
            } else {
                op = "is";
            }
        } else {
            break;
        }
        ExprP right = parseBitOr();
        if (items.empty()) items.push_back(std::move(left));
        items.push_back(std::move(right));
        ops.push_back(op);
    }
    if (ops.empty()) return left;
    auto r = mkExpr(EK::Compare, ln);
    r->items = std::move(items);
    r->ops = std::move(ops);
    return r;
}

ExprP Parser::parseBitOr() {
    const int ln = cur().line;
    ExprP left = parseBitXor();
    while (isOp("|")) {
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = "|";
        n->a = std::move(left);
        n->b = parseBitXor();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseBitXor() {
    const int ln = cur().line;
    ExprP left = parseBitAnd();
    while (isOp("^")) {
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = "^";
        n->a = std::move(left);
        n->b = parseBitAnd();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseBitAnd() {
    const int ln = cur().line;
    ExprP left = parseShift();
    while (isOp("&")) {
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = "&";
        n->a = std::move(left);
        n->b = parseShift();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseShift() {
    const int ln = cur().line;
    ExprP left = parseArith();
    while (isOp("<<") || isOp(">>")) {
        std::string op = cur().text;
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = op;
        n->a = std::move(left);
        n->b = parseArith();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseArith() {
    const int ln = cur().line;
    ExprP left = parseTerm();
    while (isOp("+") || isOp("-")) {
        std::string op = cur().text;
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = op;
        n->a = std::move(left);
        n->b = parseTerm();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseTerm() {
    const int ln = cur().line;
    ExprP left = parseFactor();
    while (isOp("*") || isOp("/") || isOp("//") || isOp("%") || isOp("@")) {
        std::string op = cur().text;
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = op;
        n->a = std::move(left);
        n->b = parseFactor();
        left = std::move(n);
    }
    return left;
}

ExprP Parser::parseFactor() {
    const int ln = cur().line;
    if (isOp("+") || isOp("-") || isOp("~")) {
        std::string op = cur().text;
        adv();
        auto e = mkExpr(EK::UnaryOp, ln);
        e->s = op;
        e->a = parseFactor();
        return e;
    }
    return parsePower();
}

ExprP Parser::parsePower() {
    const int ln = cur().line;
    ExprP base = parsePostfix();
    if (isOp("**")) {
        adv();
        auto n = mkExpr(EK::BinOp, ln);
        n->s = "**";
        n->a = std::move(base);
        n->b = parseFactor();
        return n;
    }
    return base;
}

ExprP Parser::parsePostfix() {
    ExprP e = parseAtom();
    while (true) {
        const int ln = cur().line;
        if (isOp(".")) {
            adv();
            auto a = mkExpr(EK::Attr, ln);
            a->a = std::move(e);
            a->s = expName();
            e = std::move(a);
        } else if (isOp("(")) {
            adv();
            auto c = mkExpr(EK::Call, ln);
            c->a = std::move(e);
            parseArgList(c);
            expOp(")");
            e = std::move(c);
        } else if (isOp("[")) {
            adv();
            e = parseSubscript(std::move(e));
        } else {
            break;
        }
    }
    return e;
}

void Parser::parseArgList(ExprP& call) {
    while (!isOp(")")) {
        const int ln = cur().line;
        if (isOp("*")) {
            adv();
            auto st = mkExpr(EK::Starred, ln);
            st->a = parseTest();
            call->items.push_back(std::move(st));
        } else if (isOp("**")) {
            adv();
            auto st = mkExpr(EK::Starred, ln);
            st->b = parseTest();
            call->items.push_back(std::move(st));
        } else if (cur().kind == Tok::Name && ahead().kind == Tok::Op && ahead().text == "=") {
            std::string nm = cur().text;
            adv();
            adv();
            call->kwargs.push_back({nm, parseTest()});
        } else {
            auto first = finishConditional(parseTest());
            // generator expression used as a call argument: sum(x for x in xs)
            if (isKw("for"))
                call->items.push_back(buildComprehension(std::move(first), EK::ListComp, ln));
            else
                call->items.push_back(std::move(first));
        }
        if (!accOp(",")) break;
    }
}

ExprP Parser::parseSubscript(ExprP base) {
    const int ln = cur().line;
    ExprP lo, hi, step;
    bool isSlice = false;

    if (!isOp(":")) lo = parseTest();
    if (isOp(":")) {
        isSlice = true;
        adv();
        if (!isOp(":") && !isOp("]")) hi = parseTest();
        if (accOp(":")) {
            if (!isOp("]")) step = parseTest();
        }
    }

    if (!isSlice) {
        if (isOp(",")) {
            auto t = mkExpr(EK::TupleLit, ln);
            t->items.push_back(std::move(lo));
            while (accOp(",")) {
                if (isOp("]")) break;
                t->items.push_back(parseTest());
            }
            expOp("]");
            auto s = mkExpr(EK::Subscript, ln);
            s->a = std::move(base);
            s->b = std::move(t);
            return s;
        }
        expOp("]");
        auto s = mkExpr(EK::Subscript, ln);
        s->a = std::move(base);
        s->b = std::move(lo);
        return s;
    }

    expOp("]");
    auto s = mkExpr(EK::Slice, ln);
    s->a = std::move(base);
    s->b = std::move(lo);
    s->c = std::move(hi);
    s->d = std::move(step);
    return s;
}

ExprP Parser::parseAtom() {
    const int ln = cur().line;

    if (cur().kind == Tok::Int) {
        auto e = mkExpr(EK::IntLit, ln);
        e->i = cur().i;
        adv();
        return e;
    }
    if (cur().kind == Tok::Float) {
        auto e = mkExpr(EK::FloatLit, ln);
        e->numLit = cur().d;
        adv();
        return e;
    }
    if (cur().kind == Tok::FStr) {
        Token t = cur();
        adv();
        return parseFString(t);
    }
    if (cur().kind == Tok::Str) {
        std::string val = cur().text;
        adv();
        while (cur().kind == Tok::Str) {  // implicit concatenation
            val += cur().text;
            adv();
        }
        auto e = mkExpr(EK::StrLit, ln);
        e->s = val;
        return e;
    }
    if (cur().kind == Tok::Name) {
        auto e = mkExpr(EK::Name, ln);
        e->s = cur().text;
        adv();
        return e;
    }
    if (cur().kind == Tok::Keyword) {
        const std::string k = cur().text;
        if (k == "True" || k == "False") {
            adv();
            auto e = mkExpr(EK::BoolLit, ln);
            e->boolLit = (k == "True");
            return e;
        }
        if (k == "None") {
            adv();
            return mkExpr(EK::NoneLit, ln);
        }
        if (k == "lambda") return parseLambda();
        if (k == "not") {
            adv();
            auto e = mkExpr(EK::UnaryOp, ln);
            e->s = "not";
            e->a = parseNot();
            return e;
        }
        if (k == "await") {
            adv();
            return parseAtom();
        }
        err("unexpected keyword '" + k + "'");
    }
    if (isOp("(")) return parseParenAtom();
    if (isOp("[")) return parseListOrComp();
    if (isOp("{")) return parseDictOrSetOrComp();
    if (isOp("...")) {
        adv();
        return mkExpr(EK::NoneLit, ln);
    }
    err("unexpected token '" + cur().text + "'");
}

ExprP Parser::parseLambda() {
    const int ln = cur().line;
    adv();  // lambda
    auto e = mkExpr(EK::Lambda, ln);
    while (!isOp(":")) {
        if (isOp("*")) {
            adv();
            std::string nm;
            if (cur().kind == Tok::Name) {
                nm = cur().text;
                adv();
            }
            e->argNames.push_back("*" + nm);
            e->defaults.push_back(nullptr);
        } else {
            e->argNames.push_back(expName());
            if (accOp("="))
                e->defaults.push_back(parseTest());
            else
                e->defaults.push_back(nullptr);
        }
        if (!accOp(",")) break;
    }
    expOp(":");
    e->a = parseTest();
    return e;
}

ExprP Parser::parseParenAtom() {
    const int ln = cur().line;
    expOp("(");
    if (accOp(")")) return mkExpr(EK::TupleLit, ln);
    if (isKw("yield")) {
        adv();
        if (cur().kind != Tok::Newline && !isOp(")")) parseTest();
        expOp(")");
        return mkExpr(EK::NoneLit, ln);
    }
    if (isOp("*")) {  // star-unpacking tuple
        std::vector<ExprP> items;
        while (true) {
            const int l2 = cur().line;
            if (isOp("*")) {
                adv();
                auto st = mkExpr(EK::Starred, l2);
                st->a = parseTest();
                items.push_back(std::move(st));
            } else {
                items.push_back(parseTest());
            }
            if (!accOp(",")) break;
            if (isOp(")")) break;
        }
        expOp(")");
        auto t = mkExpr(EK::TupleLit, ln);
        t->items = std::move(items);
        return t;
    }

    ExprP first = parseOr();
    first = finishConditional(std::move(first));

    if (isOp(",")) {
        std::vector<ExprP> items;
        items.push_back(std::move(first));
        while (accOp(",")) {
            if (isOp(")")) break;
            items.push_back(parseTest());
        }
        expOp(")");
        auto t = mkExpr(EK::TupleLit, ln);
        t->items = std::move(items);
        return t;
    }
    if (isKw("for")) {
        ExprP comp = buildComprehension(std::move(first), EK::ListComp, ln);
        expOp(")");
        return comp;
    }
    expOp(")");
    return first;
}

ExprP Parser::buildComprehension(ExprP element, EK kind, int ln) {
    auto comp = mkExpr(kind, ln);
    comp->a = std::move(element);
    while (isKw("for")) {
        adv();
        auto targets = parseStarExprList("for");
        if (!accKw("in")) err("expected 'in' inside comprehension");
        ExprP iter = parseOr();
        comp->compTargets.push_back(wrapTuple(std::move(targets), ln));
        comp->compIters.push_back(std::move(iter));
        std::vector<ExprP> ifs;
        while (isKw("if")) {
            adv();
            ifs.push_back(parseOr());
        }
        comp->compIfsNested.push_back(std::move(ifs));
    }
    return comp;
}

ExprP Parser::parseListOrComp() {
    const int ln = cur().line;
    expOp("[");
    if (accOp("]")) return mkExpr(EK::ListLit, ln);

    if (isOp("*")) {  // [*a, *b]
        std::vector<ExprP> items;
        while (true) {
            const int l2 = cur().line;
            if (isOp("*")) {
                adv();
                auto st = mkExpr(EK::Starred, l2);
                st->a = parseTest();
                items.push_back(std::move(st));
            } else {
                items.push_back(parseTest());
            }
            if (!accOp(",")) break;
            if (isOp("]")) break;
        }
        expOp("]");
        auto e = mkExpr(EK::ListLit, ln);
        e->items = std::move(items);
        return e;
    }

    ExprP first = parseOr();
    first = finishConditional(std::move(first));
    if (isKw("for")) {
        ExprP comp = buildComprehension(std::move(first), EK::ListComp, ln);
        expOp("]");
        return comp;
    }
    std::vector<ExprP> items;
    items.push_back(std::move(first));
    while (accOp(",")) {
        if (isOp("]")) break;
        items.push_back(parseTest());
    }
    expOp("]");
    auto e = mkExpr(EK::ListLit, ln);
    e->items = std::move(items);
    return e;
}

ExprP Parser::parseDictOrSetOrComp() {
    const int ln = cur().line;
    expOp("{");
    if (accOp("}")) return mkExpr(EK::DictLit, ln);

    ExprP first = parseOr();
    first = finishConditional(std::move(first));

    if (isOp(":")) {
        adv();
        ExprP val = parseOr();
        val = finishConditional(std::move(val));
        if (isKw("for")) {
            ExprP comp = buildComprehension(std::move(val), EK::DictComp, ln);
            comp->b = std::move(first);
            expOp("}");
            return comp;
        }
        auto d = mkExpr(EK::DictLit, ln);
        d->items.push_back(std::move(first));
        d->items.push_back(std::move(val));
        while (accOp(",")) {
            if (isOp("}")) break;
            ExprP k = parseTest();
            expOp(":");
            ExprP v = parseTest();
            d->items.push_back(std::move(k));
            d->items.push_back(std::move(v));
        }
        expOp("}");
        return d;
    }

    if (isKw("for")) {
        ExprP comp = buildComprehension(std::move(first), EK::SetComp, ln);
        expOp("}");
        return comp;
    }

    auto s = mkExpr(EK::SetLit, ln);
    s->items.push_back(std::move(first));
    while (accOp(",")) {
        if (isOp("}")) break;
        s->items.push_back(parseTest());
    }
    expOp("}");
    return s;
}

// ---------------------------------------------------------------------------
// star expression lists
// ---------------------------------------------------------------------------
std::vector<ExprP> Parser::parseStarExprList(const char* closer) {
    std::vector<ExprP> v;
    const bool stopAtIn = closer && std::strcmp(closer, "for") == 0;
    struct Guard {
        bool* flag;
        bool saved;
        Guard(bool* f, bool value) : flag(f), saved(*f) { *f = value; }
        ~Guard() { *flag = saved; }
    } guard(&noInOperator, stopAtIn);

    auto atEnd = [&]() {
        return cur().kind == Tok::Newline || cur().kind == Tok::End ||
               cur().kind == Tok::Dedent || isOp(")") || isOp("]") || isOp("}") ||
               isOp("=") || isOp(";") || isOp(":") || (stopAtIn && isKw("in"));
    };
    if (atEnd()) return v;

    while (true) {
        if (isOp("*")) {
            const int ln = cur().line;
            adv();
            auto st = mkExpr(EK::Starred, ln);
            st->a = parseBitOr();
            v.push_back(std::move(st));
        } else {
            v.push_back(parseTest());
        }
        if (!accOp(",")) break;
        if (atEnd()) break;
    }
    return v;
}

std::vector<ExprP> Parser::parseTargetList() { return parseStarExprList(""); }

ExprP Parser::wrapTuple(std::vector<ExprP> v, int line) {
    if (v.empty()) return mkExpr(EK::NoneLit, line);
    if (v.size() == 1) return std::move(v[0]);
    auto t = mkExpr(EK::TupleLit, line);
    t->items = std::move(v);
    return t;
}

// ---------------------------------------------------------------------------
// f-strings
// ---------------------------------------------------------------------------
ExprP Parser::parseFString(const Token& t) {
    const std::string& raw = t.text;
    const int ln = t.line;
    auto e = mkExpr(EK::FStr, ln);
    std::string lit;

    auto flush = [&]() {
        if (!lit.empty()) {
            FPart p;
            p.isExpr = false;
            p.literal = lit;
            e->parts.push_back(std::move(p));
            lit.clear();
        }
    };

    size_t i = 0;
    while (i < raw.size()) {
        char c = raw[i];
        if (c == '{') {
            if (i + 1 < raw.size() && raw[i + 1] == '{') {
                lit.push_back('{');
                i += 2;
                continue;
            }
            flush();
            size_t j = i + 1;
            int depth = 0;
            bool inStr = false;
            char q = 0;
            std::string inner;
            bool closed = false;
            while (j < raw.size()) {
                char d = raw[j];
                if (inStr) {
                    if (d == '\\') {
                        inner.push_back(d);
                        if (j + 1 < raw.size()) inner.push_back(raw[++j]);
                        j++;
                        continue;
                    }
                    if (d == q) inStr = false;
                    inner.push_back(d);
                    j++;
                    continue;
                }
                if (d == '\'' || d == '"') {
                    inStr = true;
                    q = d;
                    inner.push_back(d);
                    j++;
                    continue;
                }
                if (d == '(' || d == '[' || d == '{') {
                    depth++;
                    inner.push_back(d);
                    j++;
                    continue;
                }
                if (d == ')' || d == ']' || d == '}') {
                    if (d == '}' && depth == 0) {
                        closed = true;
                        j++;
                        break;
                    }
                    depth--;
                    inner.push_back(d);
                    j++;
                    continue;
                }
                inner.push_back(d);
                j++;
            }
            if (!closed) err("unterminated expression in f-string");
            i = j;

            // split off !conv and :spec at the top level
            std::string exprSrc = inner, spec, conv;
            {
                int d2 = 0;
                bool inStr2 = false;
                char q2 = 0;
                size_t colon = std::string::npos, bang = std::string::npos;
                for (size_t k = 0; k < inner.size(); ++k) {
                    char d = inner[k];
                    if (inStr2) {
                        if (d == '\\') {
                            k++;
                            continue;
                        }
                        if (d == q2) inStr2 = false;
                        continue;
                    }
                    if (d == '\'' || d == '"') {
                        inStr2 = true;
                        q2 = d;
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        d2++;
                    else if (d == ')' || d == ']' || d == '}')
                        d2--;
                    else if (d == ':' && d2 == 0 && colon == std::string::npos)
                        colon = k;
                    else if (d == '!' && d2 == 0 && bang == std::string::npos &&
                             !(k + 1 < inner.size() && inner[k + 1] == '='))
                        bang = k;
                }
                if (colon != std::string::npos) {
                    spec = inner.substr(colon + 1);
                    exprSrc = inner.substr(0, colon);
                }
                if (bang != std::string::npos &&
                    (colon == std::string::npos || bang < colon)) {
                    conv = trimCopy(inner.substr(bang + 1, colon == std::string::npos
                                                              ? std::string::npos
                                                              : colon - bang - 1));
                    exprSrc = inner.substr(0, bang);
                }
            }
            FPart p;
            p.isExpr = true;
            p.spec = spec;
            p.conv = conv;
            p.expr = Parser::parseExprSource(trimCopy(exprSrc), ln);
            e->parts.push_back(std::move(p));
            continue;
        }
        if (c == '}') {
            if (i + 1 < raw.size() && raw[i + 1] == '}') {
                lit.push_back('}');
                i += 2;
                continue;
            }
            lit.push_back('}');
            i++;
            continue;
        }
        if (c == '\\' && i + 1 < raw.size()) {
            char n = raw[i + 1];
            switch (n) {
                case 'n': lit.push_back('\n'); break;
                case 't': lit.push_back('\t'); break;
                case 'r': lit.push_back('\r'); break;
                case '\\': lit.push_back('\\'); break;
                case '\'': lit.push_back('\''); break;
                case '"': lit.push_back('"'); break;
                default:
                    lit.push_back('\\');
                    lit.push_back(n);
                    break;
            }
            i += 2;
            continue;
        }
        lit.push_back(c);
        i++;
    }
    flush();
    return e;
}
