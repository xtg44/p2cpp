// ast.h -- syntax tree for the supported Python subset
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

enum class EK {
    IntLit,
    FloatLit,
    StrLit,
    FStr,
    BoolLit,
    NoneLit,
    Name,
    ListLit,
    TupleLit,
    SetLit,
    DictLit,
    BinOp,
    UnaryOp,
    BoolOp,
    Compare,
    Call,
    Attr,
    Subscript,
    Slice,
    IfExp,
    Lambda,
    ListComp,
    DictComp,
    SetComp,
    Starred,
};

// One piece of an f-string: either literal text or a {expr!conv:spec} hole.
struct FPart {
    bool isExpr = false;
    std::string literal;
    ExprP expr;
    std::string spec;
    std::string conv;
};

struct Expr {
    EK kind = EK::NoneLit;
    int line = 0;

    long long i = 0;
    double numLit = 0;
    bool boolLit = false;
    std::string s;    // name / operator / attribute / string value

    ExprP a, b, c, d; // generic sub expressions
    std::vector<ExprP> items;
    std::vector<std::string> ops;                       // Compare operators
    std::vector<std::pair<std::string, ExprP>> kwargs;  // keyword arguments

    // f-strings
    std::vector<FPart> parts;

    // lambda
    std::vector<std::string> argNames;
    std::vector<ExprP> defaults;

    // comprehensions: clause i is (compTargets[i], compIters[i], compIfsNested[i])
    std::vector<ExprP> compTargets;
    std::vector<ExprP> compIters;
    std::vector<std::vector<ExprP>> compIfsNested;
};

enum class SK {
    ExprStmt,
    Assign,
    AugAssign,
    AnnAssign,
    If,
    While,
    For,
    FuncDef,
    ClassDef,
    Return,
    Break,
    Continue,
    Pass,
    Import,
    FromImport,
    Try,
    Raise,
    Global,
    Nonlocal,
    Del,
    Assert,
    With,
    Comment,
};

struct Param {
    std::string name;
    ExprP def;
    bool isStar = false;
    bool isKwStar = false;
    std::string annotation;
};

struct Handler {
    std::string type;   // "" == bare except
    std::string name;   // "as name"
    std::vector<StmtP> body;
};

struct Stmt {
    SK kind = SK::Pass;
    int line = 0;

    std::string s;                 // name / operator / module
    ExprP a, b, c;
    std::vector<ExprP> targets;
    std::vector<ExprP> values;

    std::vector<StmtP> body;
    std::vector<StmtP> orelse;
    std::vector<StmtP> finalbody;
    std::vector<Handler> handlers;

    // for
    ExprP iter;

    // def / class
    std::vector<Param> params;
    std::vector<std::string> decorators;
    std::vector<std::string> bases;

    // import
    std::vector<std::pair<std::string, std::string>> imports;  // (name, alias)
};

inline ExprP mkExpr(EK k, int line) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = line;
    return e;
}

inline StmtP mkStmt(SK k, int line) {
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->line = line;
    return s;
}
