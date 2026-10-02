// codegen.cpp -- Python AST -> C++ source
#include "codegen.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "prelude.h"

namespace {

// ===========================================================================
// small string / type helpers
// ===========================================================================
bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool isVectorT(const std::string& t) { return startsWith(t, "std::vector<"); }
bool isMapT(const std::string& t) {
    return startsWith(t, "std::map<") || startsWith(t, "py::dict<");
}
bool isSetT(const std::string& t) { return startsWith(t, "std::set<"); }
bool isPairT(const std::string& t) { return startsWith(t, "std::pair<"); }
bool isStrT(const std::string& t) { return t == "std::string"; }
bool isIntT(const std::string& t) { return t == "long long" || t == "int"; }
bool isFloatT(const std::string& t) { return t == "double" || t == "float"; }
bool isNumT(const std::string& t) { return isIntT(t) || isFloatT(t) || t == "bool"; }
bool isNoneT(const std::string& t) { return t == "py::NoneType"; }

std::string tplInner(const std::string& t, const std::string& head) {
    if (!startsWith(t, head) || t.empty() || t.back() != '>') return "";
    if (t.size() < head.size() + 1) return "";
    return trim(t.substr(head.size(), t.size() - head.size() - 1));
}

std::vector<std::string> splitTopLevel(const std::string& inner) {
    std::vector<std::string> r;
    int depth = 0;
    std::string cur;
    for (char c : inner) {
        if (c == '<') depth++;
        if (c == '>') depth--;
        if (c == ',' && depth == 0) {
            r.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!trim(cur).empty()) r.push_back(trim(cur));
    return r;
}

std::string elemOf(const std::string& t) {
    if (isStrT(t)) return "std::string";
    if (isVectorT(t)) return tplInner(t, "std::vector<");
    if (isSetT(t)) return tplInner(t, "std::set<");
    return "";
}
std::string mapHead(const std::string& t) {
    if (startsWith(t, "py::dict<")) return "py::dict<";
    if (startsWith(t, "std::map<")) return "std::map<";
    return "";
}
std::string keyOf(const std::string& t) {
    if (!isMapT(t)) return "";
    auto parts = splitTopLevel(tplInner(t, mapHead(t)));
    return parts.size() == 2 ? parts[0] : "";
}
std::string valOf(const std::string& t) {
    if (!isMapT(t)) return "";
    auto parts = splitTopLevel(tplInner(t, mapHead(t)));
    return parts.size() == 2 ? parts[1] : "";
}
// Element types of `std::tuple<A, B>` / `std::pair<A, B>` (empty otherwise).
std::vector<std::string> tupleParts(const std::string& t) {
    if (startsWith(t, "std::tuple<")) return splitTopLevel(tplInner(t, "std::tuple<"));
    if (isPairT(t)) return splitTopLevel(tplInner(t, "std::pair<"));
    return {};
}

std::string promote(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return "";
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a == b) return a;
    if (isFloatT(a) || isFloatT(b)) {
        if (isNumT(a) || isNumT(b)) return "double";
        return "";
    }
    if (isIntT(a) && isIntT(b)) return "long long";
    if (isIntT(a) && b == "bool") return "long long";
    if (a == "bool" && isIntT(b)) return "long long";
    if (a == "bool" && b == "bool") return "bool";
    if (isVectorT(a) && a == b) return a;
    return "";
}

std::string unify(const std::vector<std::string>& ts) {
    std::vector<std::string> known;
    for (const auto& t : ts)
        if (!t.empty() && !isNoneT(t)) known.push_back(t);
    if (known.empty()) return "";
    std::string r = known[0];
    for (size_t i = 1; i < known.size(); ++i) {
        if (known[i] == r) continue;
        std::string p = promote(r, known[i]);
        if (!p.empty()) {
            r = p;
            continue;
        }
        if (isVectorT(known[i]) && !isVectorT(r)) r = known[i];
    }
    return r;
}

std::string joinStr(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

std::string esc(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "\\%03o", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    return r + "\"";
}

const std::map<std::string, std::string>& mathFuncs() {
    static const std::map<std::string, std::string> m = {
        {"sqrt", "std::sqrt"},   {"sin", "std::sin"},     {"cos", "std::cos"},
        {"tan", "std::tan"},     {"asin", "std::asin"},   {"acos", "std::acos"},
        {"atan", "std::atan"},   {"atan2", "std::atan2"}, {"exp", "std::exp"},
        {"log", "std::log"},     {"log2", "std::log2"},   {"log10", "std::log10"},
        {"ceil", "std::ceil"},   {"floor", "std::floor"}, {"trunc", "std::trunc"},
        {"fabs", "std::fabs"},   {"fmod", "std::fmod"},   {"hypot", "std::hypot"},
        {"sinh", "std::sinh"},   {"cosh", "std::cosh"},   {"tanh", "std::tanh"},
        {"pow", "std::pow"},     {"isnan", "std::isnan"}, {"isinf", "std::isinf"},
        {"isfinite", "std::isfinite"},
    };
    return m;
}

const std::set<std::string>& excNames() {
    static const std::set<std::string> s = {"ValueError",      "TypeError",   "IndexError",
                                            "KeyError",        "ZeroDivisionError",
                                            "RuntimeError",    "StopIteration"};
    return s;
}

std::string excCppType(const std::string& t) {
    if (t.empty()) return "";
    if (excNames().count(t)) return "py::" + t;
    return "std::exception";
}

// ===========================================================================
// precedence
// ===========================================================================
int opPrec(const std::string& op) {
    if (op == "or") return 2;
    if (op == "and") return 3;
    if (op == "not") return 4;
    if (op == "|") return 6;
    if (op == "^") return 7;
    if (op == "&") return 8;
    if (op == "<<" || op == ">>") return 9;
    if (op == "+" || op == "-") return 10;
    if (op == "*" || op == "/" || op == "//" || op == "%" || op == "@") return 11;
    if (op == "**") return 13;
    return 10;
}

// ===========================================================================
// name collection
// ===========================================================================
struct SubWrite {
    const Expr* index = nullptr;
    const Expr* value = nullptr;
};

struct NameInfo {
    std::set<std::string> assigned;
    std::set<std::string> loopBound;
    std::map<std::string, int> reads;
    std::map<std::string, int> readsInsideOwnLoop;
    std::map<std::string, std::vector<const Expr*>> rhs;
    std::map<std::string, const Expr*> firstAssign;
    // Subscript stores: name[k] = v  ->  index/value expression pairs.
    std::map<std::string, std::vector<SubWrite>> subWrites;
    // `for x in <iter>:` -> the iterable plus the target's position, so the
    // target's element type can be inferred even when x is not hoisted (it is
    // declared by the for statement itself).
    struct LoopBind {
        const Expr* iter = nullptr;
        int index = 0;  // position within the target list (`for a, b in ...`)
        int total = 1;  // number of targets
    };
    std::map<std::string, LoopBind> loopIter;
    // name[k].append(v) / name[k].add(v): element expression for the *value*
    // container of a dict-of-containers that started life as `name = {}`.
    std::map<std::string, std::vector<const Expr*>> subAppends;
    // Names bound by tuple/list unpacking (a, b = ...): their declaration is
    // emitted at the unpacking statement, so they must not be hoisted with the
    // type of the whole right-hand side.
    std::set<std::string> destructured;
};

void countReads(const Expr* e, std::map<std::string, int>& m) {
    if (!e) return;
    if (e->kind == EK::Name) {
        m[e->s]++;
        return;
    }
    for (const auto& x : e->items) countReads(x.get(), m);
    for (const auto& kv : e->kwargs) countReads(kv.second.get(), m);
    countReads(e->a.get(), m);
    countReads(e->b.get(), m);
    countReads(e->c.get(), m);
    countReads(e->d.get(), m);
    for (const auto& p : e->parts) countReads(p.expr.get(), m);
    for (const auto& it : e->compIters) countReads(it.get(), m);
    for (const auto& cl : e->compIfsNested)
        for (const auto& c : cl) countReads(c.get(), m);
    for (const auto& d : e->defaults) countReads(d.get(), m);
}

void countReadsBody(const std::vector<StmtP>& body, std::map<std::string, int>& m);

void countReadsStmt(const Stmt* s, std::map<std::string, int>& m) {
    switch (s->kind) {
        case SK::ExprStmt:
        case SK::Raise:
        case SK::Assert:
            countReads(s->a.get(), m);
            countReads(s->b.get(), m);
            break;
        case SK::Assign:
            for (const auto& t : s->targets)
                if (t->kind != EK::Name) countReads(t.get(), m);
            for (const auto& v : s->values) countReads(v.get(), m);
            break;
        case SK::AugAssign:
            countReads(s->a.get(), m);
            countReads(s->b.get(), m);
            break;
        case SK::AnnAssign:
            countReads(s->a.get(), m);
            countReads(s->c.get(), m);
            break;
        case SK::If:
        case SK::While:
            countReads(s->a.get(), m);
            countReadsBody(s->body, m);
            countReadsBody(s->orelse, m);
            break;
        case SK::For:
            countReads(s->iter.get(), m);
            countReadsBody(s->body, m);
            countReadsBody(s->orelse, m);
            break;
        case SK::Return:
            countReads(s->a.get(), m);
            break;
        case SK::Try:
            countReadsBody(s->body, m);
            for (const auto& h : s->handlers) countReadsBody(h.body, m);
            countReadsBody(s->orelse, m);
            countReadsBody(s->finalbody, m);
            break;
        case SK::With:
            countReads(s->a.get(), m);
            countReadsBody(s->body, m);
            break;
        case SK::Del:
            countReads(s->a.get(), m);
            break;
        default:
            break;
    }
}
void countReadsBody(const std::vector<StmtP>& body, std::map<std::string, int>& m) {
    for (const auto& s : body) countReadsStmt(s.get(), m);
}

void collectTargetNames(const Expr* e, std::vector<const Expr*>& names) {
    if (!e) return;
    if (e->kind == EK::Name) {
        names.push_back(e);
        return;
    }
    if (e->kind == EK::TupleLit || e->kind == EK::ListLit) {
        for (const auto& x : e->items) collectTargetNames(x.get(), names);
        return;
    }
    if (e->kind == EK::Starred) collectTargetNames(e->a.get(), names);
}

void collectInfoBody(const std::vector<StmtP>& body, NameInfo& info);
void collectSubAppendStmt(const Stmt* s, std::map<std::string, std::vector<const Expr*>>& out);

// Record a subscript store such as  name[key] = value  so that an otherwise
// untyped container (e.g. `d = {}`) can be inferred as map/list.
void collectSubWrite(const Expr* target, const Expr* value, NameInfo& info) {
    if (!target || target->kind != EK::Subscript) return;
    if (!target->a || target->a->kind != EK::Name) return;
    SubWrite w;
    w.index = target->b.get();
    w.value = value;
    info.subWrites[target->a->s].push_back(w);
}

void collectInfoStmt(const Stmt* s, NameInfo& info) {
    switch (s->kind) {
        case SK::Assign: {
            for (size_t i = 0; i < s->targets.size(); ++i) {
                std::vector<const Expr*> names;
                collectTargetNames(s->targets[i].get(), names);
                const Expr* v = i < s->values.size() ? s->values[i].get() : nullptr;
                collectSubWrite(s->targets[i].get(), v, info);
                const Expr* tgt = s->targets[i].get();
                bool unpack = tgt && (tgt->kind == EK::TupleLit || tgt->kind == EK::ListLit);
                for (auto n : names) {
                    info.assigned.insert(n->s);
                    if (unpack) {
                        // a, b = ...  -> declared at the unpacking statement
                        info.destructured.insert(n->s);
                        continue;
                    }
                    if (v) info.rhs[n->s].push_back(v);
                    if (v && !info.firstAssign.count(n->s)) info.firstAssign[n->s] = v;
                }
            }
            for (const auto& v : s->values) countReads(v.get(), info.reads);
            for (const auto& t : s->targets)
                if (t->kind != EK::Name) countReads(t.get(), info.reads);
            return;
        }
        case SK::AugAssign: {
            std::vector<const Expr*> names;
            collectTargetNames(s->a.get(), names);
            collectSubWrite(s->a.get(), s->b.get(), info);
            for (auto n : names) {
                info.assigned.insert(n->s);
                info.rhs[n->s].push_back(s->b.get());
                if (!info.firstAssign.count(n->s)) info.firstAssign[n->s] = s->b.get();
            }
            countReads(s->a.get(), info.reads);
            countReads(s->b.get(), info.reads);
            return;
        }
        case SK::AnnAssign: {
            std::vector<const Expr*> names;
            collectTargetNames(s->a.get(), names);
            for (auto n : names) {
                info.assigned.insert(n->s);
                if (s->c) {
                    info.rhs[n->s].push_back(s->c.get());
                    if (!info.firstAssign.count(n->s)) info.firstAssign[n->s] = s->c.get();
                }
            }
            countReads(s->c.get(), info.reads);
            return;
        }
        case SK::For: {
            std::vector<const Expr*> names;
            collectTargetNames(s->a.get(), names);
            std::map<std::string, int> inner;
            countReads(s->iter.get(), inner);
            countReadsBody(s->body, inner);
            countReadsBody(s->orelse, inner);
            for (size_t ni = 0; ni < names.size(); ++ni) {
                auto n = names[ni];
                info.loopBound.insert(n->s);
                info.readsInsideOwnLoop[n->s] += inner.count(n->s) ? inner[n->s] : 0;
                if (!info.loopIter.count(n->s)) {
                    NameInfo::LoopBind lb;
                    lb.iter = s->iter.get();
                    lb.index = static_cast<int>(ni);
                    lb.total = static_cast<int>(names.size());
                    info.loopIter[n->s] = lb;
                }
            }
            countReads(s->iter.get(), info.reads);
            collectInfoBody(s->body, info);
            collectInfoBody(s->orelse, info);
            return;
        }
        case SK::If:
        case SK::While:
            countReads(s->a.get(), info.reads);
            collectInfoBody(s->body, info);
            collectInfoBody(s->orelse, info);
            return;
        case SK::Try:
            collectInfoBody(s->body, info);
            for (const auto& h : s->handlers) {
                if (!h.name.empty()) {
                    info.assigned.insert(h.name);
                    if (!info.firstAssign.count(h.name)) info.firstAssign[h.name] = nullptr;
                }
                collectInfoBody(h.body, info);
            }
            collectInfoBody(s->orelse, info);
            collectInfoBody(s->finalbody, info);
            return;
        case SK::With:
            countReads(s->a.get(), info.reads);
            if (!s->s.empty()) {
                info.assigned.insert(s->s);
                info.rhs[s->s].push_back(s->a.get());
                if (!info.firstAssign.count(s->s)) info.firstAssign[s->s] = s->a.get();
            }
            collectInfoBody(s->body, info);
            return;
        default:
            countReadsStmt(s, info.reads);
            return;
    }
}
void collectInfoBody(const std::vector<StmtP>& body, NameInfo& info) {
    for (const auto& s : body) {
        if (s->kind == SK::FuncDef || s->kind == SK::ClassDef) continue;
        collectInfoStmt(s.get(), info);
        collectSubAppendStmt(s.get(), info.subAppends);
    }
}

void collectAppend(const Expr* e, std::map<std::string, std::vector<const Expr*>>& out) {
    if (!e) return;
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Attr && e->a->a &&
        e->a->a->kind == EK::Name) {
        const std::string& nm = e->a->a->s;
        const std::string& m = e->a->s;
        if (m == "append" || m == "add") {
            for (const auto& arg : e->items)
                if (arg->kind != EK::Starred) out[nm].push_back(arg.get());
        }
    }
    for (const auto& x : e->items) collectAppend(x.get(), out);
    for (const auto& kv : e->kwargs) collectAppend(kv.second.get(), out);
    collectAppend(e->a.get(), out);
    collectAppend(e->b.get(), out);
    collectAppend(e->c.get(), out);
    collectAppend(e->d.get(), out);
    for (const auto& p : e->parts) collectAppend(p.expr.get(), out);
    for (const auto& it : e->compIters) collectAppend(it.get(), out);
    for (const auto& cl : e->compIfsNested)
        for (const auto& c : cl) collectAppend(c.get(), out);
}
void collectAppendBody(const std::vector<StmtP>& body,
                       std::map<std::string, std::vector<const Expr*>>& out);
void collectAppendStmt(const Stmt* s, std::map<std::string, std::vector<const Expr*>>& out) {
    if (s->kind == SK::FuncDef || s->kind == SK::ClassDef) return;
    collectAppend(s->a.get(), out);
    collectAppend(s->b.get(), out);
    for (const auto& t : s->targets) collectAppend(t.get(), out);
    for (const auto& v : s->values) collectAppend(v.get(), out);
    collectAppendBody(s->body, out);
    collectAppendBody(s->orelse, out);
    collectAppendBody(s->finalbody, out);
    for (const auto& h : s->handlers) collectAppendBody(h.body, out);
}
void collectAppendBody(const std::vector<StmtP>& body,
                       std::map<std::string, std::vector<const Expr*>>& out) {
    for (const auto& s : body) collectAppendStmt(s.get(), out);
}

// Record  name[key].append(v)  /  name[key].add(v)  so that a dict of
// containers that began life as `name = {}` can be typed from its elements.
void collectSubAppendExpr(const Expr* e, std::map<std::string, std::vector<const Expr*>>& out) {
    if (!e) return;
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Attr && e->a->a &&
        e->a->a->kind == EK::Subscript && e->a->a->a && e->a->a->a->kind == EK::Name) {
        const std::string& m = e->a->s;
        if (m == "append" || m == "add")
            for (const auto& arg : e->items)
                if (arg->kind != EK::Starred) out[e->a->a->a->s].push_back(arg.get());
    }
    for (const auto& x : e->items) collectSubAppendExpr(x.get(), out);
    for (const auto& kv : e->kwargs) collectSubAppendExpr(kv.second.get(), out);
    collectSubAppendExpr(e->a.get(), out);
    collectSubAppendExpr(e->b.get(), out);
    collectSubAppendExpr(e->c.get(), out);
    collectSubAppendExpr(e->d.get(), out);
    for (const auto& p : e->parts) collectSubAppendExpr(p.expr.get(), out);
    for (const auto& it : e->compIters) collectSubAppendExpr(it.get(), out);
    for (const auto& cl : e->compIfsNested)
        for (const auto& c : cl) collectSubAppendExpr(c.get(), out);
}
void collectSubAppendStmt(const Stmt* s, std::map<std::string, std::vector<const Expr*>>& out) {
    collectSubAppendExpr(s->a.get(), out);
    collectSubAppendExpr(s->b.get(), out);
    collectSubAppendExpr(s->c.get(), out);
    for (const auto& t : s->targets) collectSubAppendExpr(t.get(), out);
    for (const auto& v : s->values) collectSubAppendExpr(v.get(), out);
}

void collectCalls(const Expr* e, std::vector<const Expr*>& out) {
    if (!e) return;
    if (e->kind == EK::Call) out.push_back(e);
    for (const auto& x : e->items) collectCalls(x.get(), out);
    for (const auto& kv : e->kwargs) collectCalls(kv.second.get(), out);
    collectCalls(e->a.get(), out);
    collectCalls(e->b.get(), out);
    collectCalls(e->c.get(), out);
    collectCalls(e->d.get(), out);
    for (const auto& p : e->parts) collectCalls(p.expr.get(), out);
    for (const auto& it : e->compIters) collectCalls(it.get(), out);
    for (const auto& cl : e->compIfsNested)
        for (const auto& c : cl) collectCalls(c.get(), out);
}
void collectCallsBody(const std::vector<StmtP>& body, std::vector<const Expr*>& out);
void collectCallsStmt(const Stmt* s, std::vector<const Expr*>& out) {
    if (s->kind == SK::FuncDef || s->kind == SK::ClassDef) return;
    collectCalls(s->a.get(), out);
    collectCalls(s->b.get(), out);
    for (const auto& t : s->targets) collectCalls(t.get(), out);
    for (const auto& v : s->values) collectCalls(v.get(), out);
    collectCallsBody(s->body, out);
    collectCallsBody(s->orelse, out);
    collectCallsBody(s->finalbody, out);
    for (const auto& h : s->handlers) collectCallsBody(h.body, out);
}
void collectCallsBody(const std::vector<StmtP>& body, std::vector<const Expr*>& out) {
    for (const auto& s : body) collectCallsStmt(s.get(), out);
}

// ===========================================================================
// generator state
// ===========================================================================
struct Frame {
    std::string key;
    std::string cls;
    const Stmt* fn = nullptr;
    std::set<std::string> params;
    std::map<std::string, std::string> env;
    std::set<std::string> declared;
    std::set<std::string> globalNames;
    std::vector<std::string> hoist;
    std::set<std::string> hoistFallback;
    NameInfo info;
    std::map<std::string, std::vector<const Expr*>> appends;
};

struct Callable {
    const Stmt* fn = nullptr;
    std::string key;
    std::string cls;
    bool isCtor = false;
    bool inCycle = false;
    std::set<std::string> calls;
};

class Gen {
public:
    Gen(const std::vector<StmtP>& prog, CodeGenOptions o) : prog(prog), opt(std::move(o)) {}

    GenResult run();

private:
    const std::vector<StmtP>& prog;
    CodeGenOptions opt;

    std::ostringstream out;
    int ind = 0;
    std::vector<std::string> warnings;
    // Filled in when opt.splitRuntime is on; handed back to the caller so it can
    // be written next to the generated .cpp.
    std::string runtimeHeader;

    std::map<std::string, const Stmt*> funcs;
    std::map<std::string, const Stmt*> klasses;
    std::vector<std::string> classOrder;
    std::set<std::string> classSet;
    std::map<std::string, std::map<std::string, std::string>> members;
    std::map<std::string, std::vector<std::string>> memberOrder;
    std::map<std::string, const Stmt*> ctors;
    std::map<std::string, std::string> moduleAlias;
    std::map<std::string, std::string> fromImport;

    NameInfo modInfo;
    std::map<std::string, std::vector<const Expr*>> modAppends;
    std::vector<std::string> moduleDeclOrder;
    // Module-level globals whose type is not default-constructible (closures in
    // particular): they are declared as std::optional and dereferenced on use.
    std::set<std::string> lazyGlobals;

    std::map<std::string, Callable> callables;
    std::vector<std::string> callableOrder;
    std::map<std::string, Frame> frames;
    std::map<std::string, std::vector<std::string>> paramTypes;
    std::map<std::string, std::string> returnTypes;
    std::set<std::string> returnInProgress;
    std::vector<std::string> emitOrder;

    Frame* F = nullptr;
    // True while emitting a namespace-scope `decltype(...)`: lambdas there must
    // not carry a capture-default.
    bool inGlobalDecltype = false;
    // A module-level lambda is hoisted to a namespace-scope variable so that its
    // *type* has a single identity: `decltype(<lambda expression>)` names one
    // type while a second, textually identical lambda expression is a different
    // type, which would make the later assignment impossible.
    std::map<const Expr*, std::string> lambdaAlias;

    void line(const std::string& s) {
        if (s.empty()) {
            out << "\n";
            return;
        }
        for (int i = 0; i < ind; ++i) out << "    ";
        out << s << "\n";
    }
    void blank() { out << "\n"; }
    void warn(const std::string& w) { warnings.push_back(w); }

    std::string envOf(const std::string& n) const {
        auto it = F->env.find(n);
        return it == F->env.end() ? std::string() : it->second;
    }

    void collect();
    void collectCallables();
    void analyze();
    void computeParamTypes();
    void inferClassMembers(const std::string& cls);
    std::map<std::string, std::string> inferLocalTypes(
        const NameInfo& info, const std::map<std::string, std::vector<const Expr*>>& appends,
        std::map<std::string, std::string> env, const std::set<std::string>& skip,
        std::set<std::string>& declaredOut, std::vector<std::string>& hoistOut);
    void analyzeFrame(const std::string& key);
    void orderCallables();

    std::string infer(const Expr* e);
    std::string inferWith(const Expr* e, const std::map<std::string, std::string>& E);
    std::string inferReturn(const std::string& key);
    std::string builtinReturn(const std::string& name, const Expr* call);
    std::string methodReturn(const std::string& baseType, const std::string& method,
                             const Expr* call);
    std::string inferIterElem(const Expr* iter);

    std::string gen(const Expr* e, int parent = 0);
    std::string genFString(const Expr* e);
    std::string genCall(const Expr* e);
    std::string genAttr(const Expr* e);
    std::string genSubscript(const Expr* e);
    std::string genSlice(const Expr* e);
    std::string genBinOp(const Expr* e, int parent);
    std::string genCompare(const Expr* e);
    std::string genLambda(const Expr* e);

    void emitBody(const std::vector<StmtP>& body);
    void emitStmt(const Stmt* s);
    void emitOneAssign(const Expr* target, const Expr* value);
    void emitAssign(const Stmt* s);
    void emitFor(const Stmt* s);
    void emitTry(const Stmt* s);
    void emitPrint(const Expr* call);
    void emitExprStmt(const Expr* e);
    void emitRaise(const Stmt* s);
    void emitNestedFunc(const Stmt* s);
    void emitWith(const Stmt* s);

    void emitForwardDecls();
    void emitStructs();
    void emitCallable(const Callable& c);
    void emitGlobals();
    void emitHoists();
    void emitMain();

    std::string templateHeadOf(const Callable& c, bool withDefault);
    std::string paramListOf(const Callable& c, bool withDefault);
    bool isRangeCall(const Expr* e) const;
    // True when `n` names a `*args` / `**kwargs` parameter of the function
    // currently being emitted (such a name is a C++ parameter pack).
    bool isStarParam(const std::string& n) const;
};

// ===========================================================================
// collect
// ===========================================================================
void Gen::collect() {
    collectInfoBody(prog, modInfo);
    collectAppendBody(prog, modAppends);

    for (const auto& s : prog) {
        if (s->kind == SK::FuncDef) {
            funcs[s->s] = s.get();
        } else if (s->kind == SK::ClassDef) {
            klasses[s->s] = s.get();
            classSet.insert(s->s);
            classOrder.push_back(s->s);
            for (const auto& m : s->body)
                if (m->kind == SK::FuncDef && m->s == "__init__") ctors[s->s] = m.get();
        } else if (s->kind == SK::Import) {
            for (const auto& im : s->imports)
                moduleAlias[im.second.empty() ? im.first : im.second] = im.first;
        } else if (s->kind == SK::FromImport) {
            for (const auto& im : s->imports) {
                if (im.first == "*") continue;
                fromImport[im.second.empty() ? im.first : im.second] = s->s;
            }
        }
    }
}

// ===========================================================================
// callable registry
// ===========================================================================
// Build the list of every user-defined callable (free functions and class
// methods) so that later passes can look them up by a stable key:
//   free function   -> "name"
//   class method    -> "Class.method"
//   constructor     -> "Class.__init__"
void Gen::collectCallables() {
    for (const auto& s : prog) {
        if (s->kind != SK::FuncDef) continue;
        Callable c;
        c.fn = s.get();
        c.key = s->s;
        callables[c.key] = c;
        callableOrder.push_back(c.key);
    }
    for (const auto& cn : classOrder) {
        auto ki = klasses.find(cn);
        if (ki == klasses.end()) continue;
        for (const auto& m : ki->second->body) {
            if (m->kind != SK::FuncDef) continue;
            Callable c;
            c.fn = m.get();
            c.key = cn + "." + m->s;
            c.cls = cn;
            c.isCtor = (m->s == "__init__");
            callables[c.key] = c;
            callableOrder.push_back(c.key);
        }
    }
}

// ===========================================================================
// inference
// ===========================================================================
std::string Gen::infer(const Expr* e) { return inferWith(e, F->env); }

std::string Gen::inferReturn(const std::string& key) {
    auto cached = returnTypes.find(key);
    if (cached != returnTypes.end()) return cached->second;
    if (returnInProgress.count(key)) return "";
    returnInProgress.insert(key);

    std::string result;
    const Stmt* fn = nullptr;
    const std::map<std::string, std::string>* envp = nullptr;
    auto ci = callables.find(key);
    if (ci != callables.end() && ci->second.fn) {
        fn = ci->second.fn;
        auto fi = frames.find(key);
        if (fi != frames.end()) envp = &fi->second.env;
    }
    if (fn && envp) {
        std::vector<const Expr*> rets;
        std::function<void(const std::vector<StmtP>&)> walk = [&](const std::vector<StmtP>& body) {
            for (const auto& s : body) {
                if (s->kind == SK::FuncDef || s->kind == SK::ClassDef) continue;
                if (s->kind == SK::Return && s->a) rets.push_back(s->a.get());
                walk(s->body);
                walk(s->orelse);
                walk(s->finalbody);
                for (const auto& h : s->handlers) walk(h.body);
            }
        };
        walk(fn->body);
        std::vector<std::string> ts;
        for (auto r : rets) ts.push_back(inferWith(r, *envp));
        result = unify(ts);
        if (isNoneT(result)) result = "void";
    }
    returnInProgress.erase(key);
    returnTypes[key] = result;
    return result;
}

std::string Gen::builtinReturn(const std::string& name, const Expr* call) {
    auto argType = [&](size_t i) -> std::string {
        if (i < call->items.size()) return infer(call->items[i].get());
        return "";
    };
    if (name == "len") return "long long";
    if (name == "int") return "long long";
    if (name == "float") return "double";
    if (name == "str" || name == "repr" || name == "input" || name == "format")
        return "std::string";
    if (name == "bool") return "bool";
    if (name == "round") return call->items.size() > 1 ? "double" : "long long";
    if (name == "abs") return argType(0);
    // sum(iterable) yields an *element* of the iterable, not the container.
    if (name == "sum") return elemOf(argType(0));
    if (name == "min" || name == "max") {
        if (call->items.size() == 1) return elemOf(argType(0));
        std::vector<std::string> ts;
        for (size_t i = 0; i < call->items.size(); ++i) ts.push_back(argType(i));
        return unify(ts);
    }
    if (name == "sorted" || name == "reversed" || name == "list") {
        if (!call->items.empty() && call->items[0]->kind == EK::Call && call->items[0]->a &&
            call->items[0]->a->kind == EK::Name && call->items[0]->a->s == "range")
            return "std::vector<long long>";
        return argType(0);
    }
    if (name == "chr") return "std::string";
    if (name == "ord") return "long long";
    if (name == "isinstance" || name == "any" || name == "all") return "bool";
    return "";
}

bool Gen::isStarParam(const std::string& n) const {
    if (!F || F->key.empty()) return false;
    auto ci = callables.find(F->key);
    if (ci == callables.end() || !ci->second.fn) return false;
    for (const auto& p : ci->second.fn->params)
        if (p.name == n && p.isStar) return true;  // **kwargs is not a pack here
    return false;
}

std::string Gen::methodReturn(const std::string& bt, const std::string& m, const Expr* call) {
    (void)call;
    if (isStrT(bt)) {
        if (m == "upper" || m == "lower" || m == "strip" || m == "lstrip" || m == "rstrip" ||
            m == "replace" || m == "title" || m == "capitalize" || m == "zfill" ||
            m == "join" || m == "format")
            return "std::string";
        if (m == "split") return "std::vector<std::string>";
        if (m == "find" || m == "rfind" || m == "count" || m == "index") return "long long";
        if (m == "startswith" || m == "endswith" || m == "isdigit" || m == "isalpha" ||
            m == "isspace" || m == "isalnum")
            return "bool";
    }
    if (isVectorT(bt)) {
        std::string el = elemOf(bt);
        if (m == "pop") return el;
        if (m == "index" || m == "count") return "long long";
        if (m == "copy" || m == "sorted" || m == "reversed") return bt;
    }
    if (isMapT(bt)) {
        if (m == "items") return "std::vector<std::pair<" + keyOf(bt) + ", " + valOf(bt) + ">>";
        if (m == "keys") return "std::vector<" + keyOf(bt) + ">";
        if (m == "values") return "std::vector<" + valOf(bt) + ">";
        if (m == "get" || m == "pop") return valOf(bt);
        if (m == "copy") return bt;
    }
    return "";
}

std::string Gen::inferIterElem(const Expr* iter) {
    if (!iter) return "";
    if (isRangeCall(iter)) return "long long";
    if (iter->kind == EK::Call && iter->a && iter->a->kind == EK::Attr) {
        std::string bt = infer(iter->a->a.get());
        if (isMapT(bt) && iter->a->s == "items")
            return "std::pair<" + keyOf(bt) + ", " + valOf(bt) + ">";
        if (isMapT(bt) && iter->a->s == "values") return valOf(bt);
    }
    return elemOf(infer(iter));
}

std::string Gen::inferWith(const Expr* e, const std::map<std::string, std::string>& E) {
    if (!e) return "";
    auto sub = [&](const Expr* x) { return inferWith(x, E); };
    switch (e->kind) {
        case EK::IntLit:
            return "long long";
        case EK::FloatLit:
            return "double";
        case EK::StrLit:
        case EK::FStr:
            return "std::string";
        case EK::BoolLit:
            return "bool";
        case EK::NoneLit:
            return "py::NoneType";
        case EK::Name: {
            auto it = E.find(e->s);
            if (it != E.end()) return it->second;
            if (classSet.count(e->s)) return e->s;
            return "";
        }
        case EK::Starred:
            return sub(e->a.get());
        case EK::ListLit: {
            if (e->items.empty()) return "";
            std::vector<std::string> ts;
            for (const auto& x : e->items) {
                if (x->kind == EK::Starred) return "";
                ts.push_back(sub(x.get()));
            }
            std::string el = unify(ts);
            return el.empty() ? "" : "std::vector<" + el + ">";
        }
        case EK::TupleLit: {
            if (e->items.empty()) return "";
            std::vector<std::string> ts;
            for (const auto& x : e->items) {
                std::string t = sub(x.get());
                if (t.empty()) return "";
                ts.push_back(t);
            }
            return "std::tuple<" + joinStr(ts, ", ") + ">";
        }
        case EK::SetLit: {
            if (e->items.empty()) return "";
            std::vector<std::string> ts;
            for (const auto& x : e->items) ts.push_back(sub(x.get()));
            std::string el = unify(ts);
            return el.empty() ? "" : "std::set<" + el + ">";
        }
        case EK::DictLit: {
            if (e->items.size() < 2) return "";
            std::vector<std::string> ks, vs;
            for (size_t i = 0; i + 1 < e->items.size(); i += 2) {
                ks.push_back(sub(e->items[i].get()));
                vs.push_back(sub(e->items[i + 1].get()));
            }
            std::string k = unify(ks), v = unify(vs);
            if (k.empty() || v.empty()) return "";
            return "py::dict<" + k + ", " + v + ">";
        }
        case EK::BinOp: {
            std::string a = sub(e->a.get()), b = sub(e->b.get());
            const std::string& op = e->s;
            if (op == "+") {
                if (isStrT(a) || isStrT(b)) return "std::string";
                if (isVectorT(a) && isVectorT(b) && a == b) return a;
                return promote(a, b);
            }
            if (op == "*") {
                if (isStrT(a) && isIntT(b)) return "std::string";
                if (isIntT(a) && isStrT(b)) return "std::string";
                if (isVectorT(a) && isIntT(b)) return a;
                return promote(a, b);
            }
            if (op == "/") return "double";
            if (op == "//")
                return (isIntT(a) && isIntT(b)) ? "long long"
                       : (isNumT(a) && isNumT(b)) ? "double"
                                                  : "";
            if (op == "%") return promote(a, b);
            if (op == "**") {
                if (isIntT(a) && isIntT(b)) return "long long";
                if (isNumT(a) && isNumT(b)) return "double";
                return "";
            }
            if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
                std::string p = unify({a, b});
                if (isIntT(p)) return "long long";
                if (p == "bool") return "bool";
                return "";
            }
            return "";
        }
        case EK::UnaryOp:
            if (e->s == "not") return "bool";
            if (e->s == "~") return "long long";
            {
                std::string a = sub(e->a.get());
                return a == "bool" ? "long long" : a;
            }
        case EK::BoolOp:
            return "bool";
        case EK::Compare:
            return "bool";
        case EK::IfExp:
            return unify({sub(e->a.get()), sub(e->c.get())});
        case EK::Slice: {
            std::string bt = sub(e->a.get());
            return (isVectorT(bt) || isStrT(bt)) ? bt : "";
        }
        case EK::Subscript: {
            std::string bt = sub(e->a.get());
            if (isMapT(bt)) return valOf(bt);
            return elemOf(bt);
        }
        case EK::Attr: {
            if (e->a && e->a->kind == EK::Name && e->a->s == "self") {
                // F is not available while inferring class members: fall back to
                // the "self" binding carried by the environment.
                std::string cls = F ? F->cls : std::string();
                if (cls.empty()) {
                    auto si = E.find("self");
                    if (si != E.end()) cls = si->second;
                }
                auto ci = members.find(cls);
                if (ci != members.end()) {
                    auto mi = ci->second.find(e->s);
                    if (mi != ci->second.end()) return mi->second;
                }
                return "";
            }
            std::string bt = sub(e->a.get());
            if (classSet.count(bt)) {
                auto ci = members.find(bt);
                if (ci != members.end()) {
                    auto mi = ci->second.find(e->s);
                    if (mi != ci->second.end()) return mi->second;
                }
            }
            if (e->a && e->a->kind == EK::Name) {
                auto mi = moduleAlias.find(e->a->s);
                if (mi != moduleAlias.end()) {
                    if (mi->second == "math" &&
                        (e->s == "pi" || e->s == "e" || e->s == "tau" || e->s == "inf" ||
                         e->s == "nan"))
                        return "double";
                    if (mi->second == "sys" && e->s == "argv") return "std::vector<std::string>";
                }
            }
            return "";
        }
        case EK::Call: {
            const Expr* callee = e->a.get();
            if (!callee) return "";
            if (callee->kind == EK::Name) {
                const std::string& nm = callee->s;
                if (nm == "range") return "std::vector<long long>";
                std::string b = builtinReturn(nm, e);
                if (!b.empty()) return b;
                if (funcs.count(nm)) return inferReturn(nm);
                if (classSet.count(nm)) return nm;
                return "";
            }
            if (callee->kind == EK::Attr) {
                if (callee->a && callee->a->kind == EK::Name) {
                    auto mi = moduleAlias.find(callee->a->s);
                    if (mi != moduleAlias.end()) {
                        const std::string& mod = mi->second;
                        const std::string& fn = callee->s;
                        if (mod == "math") {
                            if (fn == "factorial" || fn == "gcd") return "long long";
                            // Python's floor/ceil/trunc return int, not float.
                            if (fn == "floor" || fn == "ceil" || fn == "trunc") return "long long";
                            if (fn == "isnan" || fn == "isinf" || fn == "isfinite") return "bool";
                            return "double";
                        }
                        if (mod == "random") {
                            if (fn == "randint" || fn == "randrange") return "long long";
                            if (fn == "choice" && !e->items.empty())
                                return elemOf(inferWith(e->items[0].get(), E));
                            return "double";
                        }
                        if (mod == "time" && fn == "time") return "double";
                    }
                }
                std::string bt = sub(callee->a.get());
                std::string mr = methodReturn(bt, callee->s, e);
                if (!mr.empty()) return mr;
                return "";
            }
            return "";
        }
        case EK::ListComp:
        case EK::SetComp: {
            std::map<std::string, std::string> e2 = E;
            for (size_t i = 0; i < e->compTargets.size(); ++i) {
                // Later `for` clauses may iterate over the previous clause's
                // targets, so resolve against e2 (which accumulates them).
                std::string el = elemOf(inferWith(e->compIters[i].get(), e2));
                std::vector<const Expr*> names;
                collectTargetNames(e->compTargets[i].get(), names);
                for (auto n : names)
                    if (!el.empty()) e2[n->s] = el;
            }
            std::string el = inferWith(e->a.get(), e2);
            if (el.empty()) return "";
            return (e->kind == EK::SetComp ? "std::set<" : "std::vector<") + el + ">";
        }
        case EK::DictComp: {
            std::map<std::string, std::string> e2 = E;
            for (size_t i = 0; i < e->compTargets.size(); ++i) {
                std::string el = elemOf(inferWith(e->compIters[i].get(), e2));
                std::vector<const Expr*> names;
                collectTargetNames(e->compTargets[i].get(), names);
                for (auto n : names)
                    if (!el.empty()) e2[n->s] = el;
            }
            std::string k = inferWith(e->b.get(), e2), v = inferWith(e->a.get(), e2);
            if (k.empty() || v.empty()) return "";
            return "py::dict<" + k + ", " + v + ">";
        }
        case EK::Lambda:
            return "";
    }
    return "";
}

// ===========================================================================
// analyze
// ===========================================================================
std::map<std::string, std::string> Gen::inferLocalTypes(
    const NameInfo& info, const std::map<std::string, std::vector<const Expr*>>& appends,
    std::map<std::string, std::string> env, const std::set<std::string>& skip,
    std::set<std::string>& declaredOut, std::vector<std::string>& hoistOut) {
    std::set<std::string> hoistSet = info.assigned;
    for (const auto& n : info.loopBound) {
        auto rit = info.reads.find(n);
        int total = rit == info.reads.end() ? 0 : rit->second;
        auto iit = info.readsInsideOwnLoop.find(n);
        int inside = iit == info.readsInsideOwnLoop.end() ? 0 : iit->second;
        if (total > inside) hoistSet.insert(n);
    }
    std::vector<std::string> order(hoistSet.begin(), hoistSet.end());
    auto lineOf = [&](const std::string& n) {
        auto it = info.firstAssign.find(n);
        return (it != info.firstAssign.end() && it->second) ? it->second->line : INT_MAX;
    };
    std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        int la = lineOf(a), lb = lineOf(b);
        if (la != lb) return la < lb;
        return a < b;
    });

    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& n : order) {
            if (skip.count(n)) continue;
            auto ex = env.find(n);
            if (ex != env.end() && !ex->second.empty()) continue;

            std::vector<std::string> ts;
            bool emptyContainer = false;
            bool emptyDict = false;
            auto ri = info.rhs.find(n);
            if (ri != info.rhs.end()) {
                for (auto r : ri->second) {
                    if (!r) continue;
                    ts.push_back(inferWith(r, env));
                    if ((r->kind == EK::ListLit || r->kind == EK::SetLit) && r->items.empty())
                        emptyContainer = true;
                    if (r->kind == EK::DictLit && r->items.empty()) {
                        emptyContainer = true;
                        emptyDict = true;
                    }
                }
            }
            std::string t = unify(ts);
            if ((t.empty() || emptyContainer) && appends.count(n)) {
                std::vector<std::string> et;
                for (auto a : appends.at(n)) et.push_back(inferWith(a, env));
                std::string el = unify(et);
                if (!el.empty()) t = "std::vector<" + el + ">";
            }
            // `d = {}` / `xs = []` followed by subscript stores: recover the
            // element (and key) types from  d[k] = v  /  xs[i] = v.
            if (t.empty() || emptyContainer) {
                auto sw = info.subWrites.find(n);
                if (sw != info.subWrites.end() && !sw->second.empty()) {
                    std::vector<std::string> vt;
                    std::vector<std::string> kt;
                    for (const auto& w : sw->second) {
                        std::string vt1 = w.value ? inferWith(w.value, env) : "";
                        // `d[k] = []` followed by `d[k].append(x)` -> vector<X>
                        if (vt1.empty() && w.value && w.value->items.empty() &&
                            (w.value->kind == EK::ListLit || w.value->kind == EK::SetLit)) {
                            auto sa = info.subAppends.find(n);
                            if (sa != info.subAppends.end() && !sa->second.empty()) {
                                std::vector<std::string> et;
                                for (auto a : sa->second) et.push_back(inferWith(a, env));
                                std::string el = unify(et);
                                if (!el.empty()) vt1 = "std::vector<" + el + ">";
                            }
                        }
                        vt.push_back(vt1);
                        if (w.index) kt.push_back(inferWith(w.index, env));
                    }
                    std::string v = unify(vt);
                    std::string k = unify(kt);
                    if (!v.empty()) {
                        if (emptyDict || !k.empty()) {
                            if (k.empty()) k = "std::string";
                            t = "py::dict<" + k + ", " + v + ">";
                        } else {
                            t = "std::vector<" + v + ">";
                        }
                    }
                }
            }
            if (t.empty()) continue;
            env[n] = t;
            if (!declaredOut.count(n)) {
                hoistOut.push_back(n);
                declaredOut.insert(n);
            }
        }

        // Loop targets (`for x in it:`) are declared by the for statement, so
        // they are never hoisted -- but an expression such as `k = x[0]` needs
        // their element type, so record it in the environment too.
        for (const auto& kv : info.loopIter) {
            const std::string& n = kv.first;
            if (skip.count(n)) continue;
            auto ex = env.find(n);
            if (ex != env.end() && !ex->second.empty()) continue;
            std::string el = elemOf(inferWith(kv.second.iter, env));
            if (el.empty()) continue;
            if (kv.second.total <= 1) {
                env[n] = el;
                continue;
            }
            auto parts = tupleParts(el);
            if (static_cast<int>(parts.size()) > kv.second.index)
                env[n] = parts[static_cast<size_t>(kv.second.index)];
        }
    }

    std::stable_sort(hoistOut.begin(), hoistOut.end(), [&](const std::string& a, const std::string& b) {
        int la = lineOf(a), lb = lineOf(b);
        if (la != lb) return la < lb;
        return a < b;
    });
    std::vector<std::string> uniq;
    std::set<std::string> seen;
    for (const auto& n : hoistOut)
        if (!seen.count(n)) {
            seen.insert(n);
            uniq.push_back(n);
        }
    hoistOut = uniq;
    return env;
}

void Gen::inferClassMembers(const std::string& cls) {
    auto ki = klasses.find(cls);
    if (ki == klasses.end()) return;
    const Stmt* k = ki->second;

    // Environment carrying `self` plus every constructor parameter.
    std::map<std::string, std::string> env;
    env["self"] = cls;
    auto ci = ctors.find(cls);
    if (ci != ctors.end()) {
        auto pt = paramTypes.find(cls + ".__init__");
        size_t pi = 0;
        for (const auto& p : ci->second->params) {
            if (p.name == "self" || p.isStar || p.isKwStar) continue;
            std::string t = (pt != paramTypes.end() && pi < pt->second.size()) ? pt->second[pi] : "";
            if (t.empty() && p.def) t = inferWith(p.def.get(), env);
            if (t.empty()) t = "long long";
            env[p.name] = t;
            pi++;
        }
    }

    std::map<std::string, std::string> mem;
    std::vector<std::string> order;
    // Members initialised to an empty container: refined from later usage.
    std::set<std::string> pendingVec, pendingMap;
    std::map<std::string, std::vector<const Expr*>> appendEl;
    std::map<std::string, std::vector<const Expr*>> mapKey, mapVal;

    std::vector<const Stmt*> methods;
    for (const auto& m : k->body)
        if (m->kind == SK::FuncDef) methods.push_back(m.get());

    std::set<std::string> ordered;
    auto touch = [&](const std::string& n) {
        if (ordered.insert(n).second) order.push_back(n);
    };

    for (int pass = 0; pass < 3; ++pass) {
        for (auto m : methods) {
            std::map<std::string, std::string> menv = env;
            auto pt = paramTypes.find(cls + "." + m->s);
            size_t pi = 0;
            for (const auto& p : m->params) {
                if (p.name == "self" || p.isStar || p.isKwStar) continue;
                std::string t = (pt != paramTypes.end() && pi < pt->second.size()) ? pt->second[pi] : "";
                if (t.empty() && p.def) t = inferWith(p.def.get(), menv);
                if (t.empty()) t = "long long";
                menv[p.name] = t;
                pi++;
            }
            std::function<void(const std::vector<StmtP>&)> w = [&](const std::vector<StmtP>& body) {
                for (const auto& s : body) {
                    // self.member = value
                    if (s->kind == SK::Assign) {
                        for (size_t i = 0; i < s->targets.size(); ++i) {
                            const Expr* t = s->targets[i].get();
                            const Expr* v = i < s->values.size() ? s->values[i].get() : nullptr;
                            if (!v) continue;
                            if (t && t->kind == EK::Attr && t->a && t->a->kind == EK::Name &&
                                t->a->s == "self") {
                                std::string tt = inferWith(v, menv);
                                touch(t->s);
                                if (tt.empty() || isNoneT(tt)) {
                                    if (v->kind == EK::ListLit) pendingVec.insert(t->s);
                                    else if (v->kind == EK::DictLit) pendingMap.insert(t->s);
                                } else if (!mem.count(t->s) || mem[t->s].empty()) {
                                    mem[t->s] = tt;
                                } else {
                                    std::string p2 = promote(mem[t->s], tt);
                                    if (!p2.empty()) mem[t->s] = p2;
                                }
                            }
                            // self.member[key] = value
                            if (t && t->kind == EK::Subscript && t->a && t->a->kind == EK::Attr &&
                                t->a->a && t->a->a->kind == EK::Name && t->a->a->s == "self") {
                                const std::string& n = t->a->s;
                                touch(n);
                                pendingMap.insert(n);
                                if (t->b) mapKey[n].push_back(t->b.get());
                                mapVal[n].push_back(v);
                            }
                        }
                    }
                    // self.member.append(x) / .add(x)  -> element type
                    if (s->kind == SK::ExprStmt || s->kind == SK::Assign ||
                        s->kind == SK::Return) {
                        std::vector<const Expr*> es;
                        if (s->a) es.push_back(s->a.get());
                        for (const auto& x : s->values) es.push_back(x.get());
                        for (auto e : es) {
                            if (!(e && e->kind == EK::Call && e->a && e->a->kind == EK::Attr &&
                                  e->a->a && e->a->a->kind == EK::Attr && e->a->a->a &&
                                  e->a->a->a->kind == EK::Name && e->a->a->a->s == "self"))
                                continue;
                            const std::string& n = e->a->a->s;
                            if (e->a->s != "append" && e->a->s != "add") continue;
                            touch(n);
                            pendingVec.insert(n);
                            for (const auto& x : e->items) appendEl[n].push_back(x.get());
                        }
                    }
                    w(s->body);
                    w(s->orelse);
                    w(s->finalbody);
                    for (const auto& h : s->handlers) w(h.body);
                }
            };
            w(m->body);
        }
    }

    // Refine members that were only ever initialised to an empty container.
    for (const auto& n : pendingVec) {
        if (mem.count(n) && !mem[n].empty()) continue;
        std::vector<std::string> ts;
        for (auto e : appendEl[n])
            if (e) ts.push_back(inferWith(e, env));
        std::string el = unify(ts);
        if (el.empty()) el = "long long";
        mem[n] = "std::vector<" + el + ">";
    }
    for (const auto& n : pendingMap) {
        if (mem.count(n) && !mem[n].empty() && !pendingVec.count(n)) {
            // already resolved as something concrete; leave it
        }
        std::vector<std::string> ks, vs;
        for (auto e : mapKey[n])
            if (e) ks.push_back(inferWith(e, env));
        for (auto e : mapVal[n])
            if (e) vs.push_back(inferWith(e, env));
        std::string kt = unify(ks), vt = unify(vs);
        if (kt.empty()) kt = "std::string";
        if (vt.empty()) vt = "long long";
        mem[n] = "py::dict<" + kt + ", " + vt + ">";
    }

    members[cls] = mem;
    memberOrder[cls] = order;
}

void Gen::computeParamTypes() {
    std::map<std::string, std::vector<std::string>> acc;
    std::function<void(const Expr*, const Frame&, const std::string&)> walkExpr;
    std::function<void(const std::vector<StmtP>&, const std::string&, const Frame&)> walkBody;

    walkExpr = [&](const Expr* e, const Frame& caller, const std::string& callerCls) {
        if (!e) return;
        if (e->kind == EK::Call && e->a) {
            std::string key;
            if (e->a->kind == EK::Name) {
                if (callables.count(e->a->s)) key = e->a->s;
                else if (callables.count(e->a->s + ".__init__")) key = e->a->s + ".__init__";
            } else if (e->a->kind == EK::Attr && e->a->a && e->a->a->kind == EK::Name &&
                       e->a->a->s == "self" && callables.count(callerCls + "." + e->a->s)) {
                key = callerCls + "." + e->a->s;
            }
            if (!key.empty()) {
                const Stmt* fn = callables[key].fn;
                size_t pi = 0;
                for (const auto& p : fn->params) {
                    if (p.name == "self" || p.isStar || p.isKwStar) continue;
                    if (pi < e->items.size() && e->items[pi]->kind != EK::Starred) {
                        std::string t = inferWith(e->items[pi].get(), caller.env);
                        if (!t.empty() && !isNoneT(t))
                            acc[key + "#" + std::to_string(pi)].push_back(t);
                    }
                    pi++;
                }
            }
        }
        walkExpr(e->a.get(), caller, callerCls);
        walkExpr(e->b.get(), caller, callerCls);
        walkExpr(e->c.get(), caller, callerCls);
        walkExpr(e->d.get(), caller, callerCls);
        for (const auto& x : e->items) walkExpr(x.get(), caller, callerCls);
        for (const auto& kv : e->kwargs) walkExpr(kv.second.get(), caller, callerCls);
        for (const auto& p : e->parts) walkExpr(p.expr.get(), caller, callerCls);
        for (const auto& it : e->compIters) walkExpr(it.get(), caller, callerCls);
        for (const auto& cl : e->compIfsNested)
            for (const auto& c : cl) walkExpr(c.get(), caller, callerCls);
    };

    walkBody = [&](const std::vector<StmtP>& body, const std::string& cls, const Frame& caller) {
        for (const auto& s : body) {
            walkExpr(s->a.get(), caller, cls);
            walkExpr(s->b.get(), caller, cls);
            for (const auto& t : s->targets) walkExpr(t.get(), caller, cls);
            for (const auto& v : s->values) walkExpr(v.get(), caller, cls);
            walkBody(s->body, cls, caller);
            walkBody(s->orelse, cls, caller);
            walkBody(s->finalbody, cls, caller);
            for (const auto& h : s->handlers) walkBody(h.body, cls, caller);
        }
    };

    {
        static Frame empty;
        auto mi = frames.find("");
        walkBody(prog, "", mi == frames.end() ? empty : mi->second);
    }
    for (const auto& key : callableOrder) {
        auto ci = callables.find(key);
        if (ci == callables.end() || !ci->second.fn) continue;
        Frame f;
        f.key = key;
        f.cls = ci->second.cls;
        f.fn = ci->second.fn;
        auto fi = frames.find(key);
        if (fi != frames.end()) f.env = fi->second.env;
        for (const auto& p : f.fn->params)
            if (!f.env.count(p.name)) f.env[p.name] = "";
        walkBody(f.fn->body, f.cls, f);
    }

    for (const auto& key : callableOrder) {
        auto ci = callables.find(key);
        if (ci == callables.end() || !ci->second.fn) continue;
        std::vector<std::string> types;
        size_t pi = 0;
        for (const auto& p : ci->second.fn->params) {
            if (p.name == "self" && !ci->second.cls.empty()) continue;
            if (p.isStar || p.isKwStar) {
                types.push_back("");
                continue;
            }
            std::vector<std::string> ts;
            std::string k = key + "#" + std::to_string(pi);
            if (acc.count(k)) ts = acc[k];
            if (p.def) {
                auto fi = frames.find(key);
                std::map<std::string, std::string> e2 =
                    fi == frames.end() ? std::map<std::string, std::string>{} : fi->second.env;
                ts.push_back(inferWith(p.def.get(), e2));
            }
            types.push_back(unify(ts));
            pi++;
        }
        paramTypes[key] = types;
    }
}

void Gen::analyzeFrame(const std::string& key) {
    auto ci = callables.find(key);
    if (ci == callables.end() || !ci->second.fn) return;
    const Stmt* fn = ci->second.fn;

    Frame f;
    f.key = key;
    f.cls = ci->second.cls;
    f.fn = fn;

    std::set<std::string> skip;
    auto pt = paramTypes.find(key);
    size_t pi = 0;
    for (const auto& p : fn->params) {
        f.params.insert(p.name);
        f.declared.insert(p.name);
        skip.insert(p.name);
        if (p.isStar || p.isKwStar) {
            // *args stays a C++ parameter pack; **kwargs is declared in the body
            // as an (always empty) mapping -- see emitCallable.
            f.env[p.name] = p.isStar ? "std::vector<long long>"
                                     : "py::dict<std::string, std::string>";
            pi++;
            continue;
        }
        if (p.name == "self") {
            f.env["self"] = ci->second.cls;
        } else {
            std::string t = (pt != paramTypes.end() && pi < pt->second.size()) ? pt->second[pi] : "";
            if (t.empty() && p.def) t = inferWith(p.def.get(), f.env);
            f.env[p.name] = t;
        }
        pi++;
    }

    collectInfoBody(fn->body, f.info);
    collectAppendBody(fn->body, f.appends);

    std::function<void(const std::vector<StmtP>&)> findGlobals = [&](const std::vector<StmtP>& body) {
        for (const auto& s : body) {
            if (s->kind == SK::Global) {
                std::string cur2;
                for (char c : s->s) {
                    if (c == ',') {
                        if (!cur2.empty()) f.globalNames.insert(cur2);
                        cur2.clear();
                    } else if (!std::isspace(static_cast<unsigned char>(c))) {
                        cur2 += c;
                    }
                }
                if (!cur2.empty()) f.globalNames.insert(cur2);
            } else if (s->kind != SK::FuncDef && s->kind != SK::ClassDef) {
                findGlobals(s->body);
                findGlobals(s->orelse);
                findGlobals(s->finalbody);
                for (const auto& h : s->handlers) findGlobals(h.body);
            }
        }
    };
    findGlobals(fn->body);
    for (const auto& g : f.globalNames) skip.insert(g);

    std::set<std::string> decl = f.declared;
    auto env2 = inferLocalTypes(f.info, f.appends, f.env, skip, decl, f.hoist);
    f.env = env2;
    f.declared = decl;

    frames[key] = f;
}

void Gen::orderCallables() {
    for (const auto& key : callableOrder) {
        auto& c = callables[key];
        std::vector<const Expr*> calls;
        collectCallsBody(c.fn->body, calls);
        for (auto call : calls) {
            if (!call->a) continue;
            if (call->a->kind == EK::Name) {
                if (callables.count(call->a->s)) c.calls.insert(call->a->s);
            } else if (call->a->kind == EK::Attr && call->a->a && call->a->a->kind == EK::Name &&
                       call->a->a->s == "self" && callables.count(c.cls + "." + call->a->s)) {
                c.calls.insert(c.cls + "." + call->a->s);
            }
        }
    }

    std::set<std::string> visited, onStack;
    std::function<void(const std::string&)> dfs = [&](const std::string& n) {
        if (visited.count(n)) return;
        if (onStack.count(n)) return;
        onStack.insert(n);
        for (const auto& d : callables[n].calls) {
            if (!callables.count(d)) continue;
            if (onStack.count(d)) {
                callables[d].inCycle = true;
                callables[n].inCycle = true;
            } else {
                dfs(d);
            }
        }
        onStack.erase(n);
        visited.insert(n);
        emitOrder.push_back(n);
    };
    for (const auto& key : callableOrder) dfs(key);
}

void Gen::analyze() {
    for (int round = 0; round < 3; ++round) {
        returnTypes.clear();
        computeParamTypes();
        for (const auto& cn : classOrder) inferClassMembers(cn);

        {
            Frame f;
            f.key = "";
            f.info = modInfo;
            f.appends = modAppends;
            std::set<std::string> decl;
            auto env2 = inferLocalTypes(modInfo, modAppends, {}, {}, decl, f.hoist);
            f.env = env2;
            f.declared = decl;
            frames[""] = f;

            std::set<std::string> all = modInfo.assigned;
            for (const auto& n : modInfo.loopBound) {
                int total = modInfo.reads.count(n) ? modInfo.reads[n] : 0;
                int inside =
                    modInfo.readsInsideOwnLoop.count(n) ? modInfo.readsInsideOwnLoop[n] : 0;
                if (total > inside) all.insert(n);
            }
            std::vector<std::string> ord(all.begin(), all.end());
            std::stable_sort(ord.begin(), ord.end(), [&](const std::string& a, const std::string& b) {
                int la = INT_MAX, lb = INT_MAX;
                auto ia = modInfo.firstAssign.find(a);
                if (ia != modInfo.firstAssign.end() && ia->second) la = ia->second->line;
                auto ib = modInfo.firstAssign.find(b);
                if (ib != modInfo.firstAssign.end() && ib->second) lb = ib->second->line;
                if (la != lb) return la < lb;
                return a < b;
            });
            moduleDeclOrder = ord;
        }

        for (const auto& key : callableOrder) analyzeFrame(key);
    }
    orderCallables();
}

// ===========================================================================
// expression emission
// ===========================================================================
std::string Gen::gen(const Expr* e, int parent) {
    if (!e) return "";
    switch (e->kind) {
        case EK::IntLit:
            return std::to_string(e->i);
        case EK::FloatLit: {
            std::ostringstream os;
            os << std::setprecision(17) << e->numLit;
            std::string r = os.str();
            if (r.find('.') == std::string::npos && r.find('e') == std::string::npos &&
                r.find('n') == std::string::npos && r.find('i') == std::string::npos)
                r += ".0";
            return r;
        }
        case EK::StrLit:
            return esc(e->s);
        case EK::FStr:
            return genFString(e);
        case EK::BoolLit:
            return e->boolLit ? "true" : "false";
        case EK::NoneLit:
            return "py::None";
        case EK::Name:
            if (e->s == "self" && !F->cls.empty()) return "(*this)";
            if (e->s == "__name__") return "std::string(\"__main__\")";
            // Globals stored in a std::optional must be dereferenced.
            if (!F->params.count(e->s) && !F->declared.count(e->s) &&
                lazyGlobals.count(e->s))
                return "(*" + e->s + ")";
            // A user function used as a first-class value (map(f, xs), key=f, ...)
            // becomes a forwarding lambda.
            if (F && callables.count(e->s) && !F->params.count(e->s) &&
                !F->declared.count(e->s) && !F->globalNames.count(e->s)) {
                std::string fn = e->s;
                return "[&](auto&&... __a) { return " + fn +
                       "(std::forward<decltype(__a)>(__a)...); }";
            }
            // Same for a builtin used as a value, e.g. sorted(xs, key=len).
            if (F && !F->params.count(e->s) && !F->declared.count(e->s)) {
                static const std::map<std::string, std::string> bfn = {
                    {"len", "py::len"},       {"abs", "py::py_abs"},     {"str", "py::str"},
                    {"int", "py::to_int"},    {"float", "py::to_float"}, {"bool", "py::to_bool"},
                    {"repr", "py::repr"},     {"sum", "py::py_sum"},     {"min", "py::py_min"},
                    {"max", "py::py_max"},    {"sorted", "py::py_sorted"}, {"round", "py::py_round"},
                    {"ord", "py::ord_of"},    {"chr", "py::chr_of"},     {"list", "py::to_list"},
                };
                auto bi = bfn.find(e->s);
                if (bi != bfn.end())
                    return "[&](auto&&... __a) { return " + bi->second +
                           "(std::forward<decltype(__a)>(__a)...); }";
            }
            return e->s;
        case EK::Starred:
            return gen(e->a.get(), parent);
        case EK::TupleLit: {
            std::string s = "std::make_tuple(";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) s += ", ";
                s += gen(e->items[i].get(), 0);
            }
            return s + ")";
        }
        case EK::ListLit: {
            std::vector<std::string> ts;
            for (const auto& x : e->items)
                if (x->kind != EK::Starred) ts.push_back(infer(x.get()));
            std::string et = unify(ts);
            if (et.empty()) et = "long long";
            std::string s = "std::vector<" + et + ">{";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) s += ", ";
                s += gen(e->items[i].get(), 0);
            }
            return s + "}";
        }
        case EK::SetLit: {
            std::vector<std::string> ts;
            for (const auto& x : e->items) ts.push_back(infer(x.get()));
            std::string et = unify(ts);
            if (et.empty()) et = "long long";
            std::string s = "std::set<" + et + ">{";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) s += ", ";
                s += gen(e->items[i].get(), 0);
            }
            return s + "}";
        }
        case EK::DictLit: {
            std::vector<std::string> ks, vs;
            for (size_t i = 0; i + 1 < e->items.size(); i += 2) {
                ks.push_back(infer(e->items[i].get()));
                vs.push_back(infer(e->items[i + 1].get()));
            }
            std::string k = unify(ks), v = unify(vs);
            if (k.empty()) k = "std::string";
            if (v.empty()) v = "long long";
            std::string s = "py::dict<" + k + ", " + v + ">{";
            for (size_t i = 0; i + 1 < e->items.size(); i += 2) {
                if (i) s += ", ";
                s += "{" + gen(e->items[i].get(), 0) + ", " + gen(e->items[i + 1].get(), 0) + "}";
            }
            return s + "}";
        }
        case EK::BinOp:
            return genBinOp(e, parent);
        case EK::UnaryOp: {
            std::string a = gen(e->a.get(), 12);
            std::string r;
            if (e->s == "not")
                r = "!" + a;
            else if (e->s == "~")
                r = "~" + a;
            else
                r = e->s + a;
            if (parent > 12) r = "(" + r + ")";
            return r;
        }
        case EK::BoolOp: {
            std::string s;
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) s += e->s == "and" ? " && " : " || ";
                s += gen(e->items[i].get(), opPrec(e->s) + 1);
            }
            if (parent > opPrec(e->s)) s = "(" + s + ")";
            return s;
        }
        case EK::Compare: {
            std::string r = genCompare(e);
            if (parent > 5) r = "(" + r + ")";
            return r;
        }
        case EK::IfExp:
            return "(" + gen(e->b.get(), 0) + " ? " + gen(e->a.get(), 0) + " : " +
                   gen(e->c.get(), 0) + ")";
        case EK::Call:
            return genCall(e);
        case EK::Attr:
            return genAttr(e);
        case EK::Subscript:
            return genSubscript(e);
        case EK::Slice:
            return genSlice(e);
        case EK::Lambda:
            return genLambda(e);
        case EK::ListComp:
        case EK::SetComp:
        case EK::DictComp: {
            bool isDict = e->kind == EK::DictComp;
            bool isSetComp = e->kind == EK::SetComp;
            // Take the element types from the whole expression's inferred type:
            // that is the same source the container's declaration used, so the
            // two can never disagree.
            std::string whole = infer(e);
            std::string keyT = isDict ? keyOf(whole) : "";
            std::string valT = isSetComp ? tplInner(whole, "std::set<")
                                         : (isDict ? valOf(whole) : tplInner(whole, "std::vector<"));
            if (isDict) {
                if (keyT.empty()) keyT = "std::string";
                if (valT.empty()) valT = "long long";
            } else if (valT.empty()) {
                valT = "long long";
            }

            std::string body;
            body += "\n    ";
            body += isDict ? ("py::dict<" + keyT + ", " + valT + "> __r;")
                           : (isSetComp ? ("std::set<" + valT + "> __r;")
                                        : ("std::vector<" + valT + "> __r;"));
            int depth = 1;
            for (size_t i = 0; i < e->compTargets.size(); ++i) {
                std::string ci = gen(e->compIters[i].get(), 0);
                // Python iterates a str as 1-char strings; C++ would yield chars.
                if (isStrT(infer(e->compIters[i].get()))) ci = "py::py_chars(" + ci + ")";
                std::vector<const Expr*> names;
                collectTargetNames(e->compTargets[i].get(), names);
                std::string tgt;
                if (names.size() == 1) {
                    tgt = names[0]->s;
                } else {
                    std::string inner;
                    for (size_t k = 0; k < names.size(); ++k) {
                        if (k) inner += ", ";
                        inner += names[k]->s;
                    }
                    tgt = "[" + inner + "]";
                }
                body += "\n" + std::string(static_cast<size_t>(4 * depth), ' ') +
                        "for (auto " + tgt + " : " + ci + ") {";
                depth++;
            }
            std::string pad(static_cast<size_t>(4 * depth), ' ');
            int extra = 0;
            for (const auto& cl : e->compIfsNested)
                for (const auto& c : cl) {
                    body += "\n" + pad + "if (" + gen(c.get(), 0) + ") {";
                    extra++;
                }
            body += "\n" + pad +
                    (isDict ? ("__r[" + gen(e->b.get(), 0) + "] = " + gen(e->a.get(), 0) + ";")
                            : (isSetComp ? ("__r.insert(" + gen(e->a.get(), 0) + ");")
                                         : ("__r.push_back(" + gen(e->a.get(), 0) + ");")));
            for (int i = 0; i < extra; ++i) body += "\n" + pad + "}";
            for (int i = static_cast<int>(e->compTargets.size()) - 1; i >= 0; --i)
                body += "\n" + std::string(static_cast<size_t>(4 * (i + 1)), ' ') + "}";
            body += "\n    return __r;\n";
            return std::string(inGlobalDecltype ? "([]() {" : "([&]() {") + body + "}())";
        }
    }
    return "";
}

std::string Gen::genFString(const Expr* e) {
    std::vector<std::string> parts;
    std::vector<bool> isLit;
    for (const auto& p : e->parts) {
        if (!p.isExpr) {
            if (!p.literal.empty()) {
                parts.push_back(esc(p.literal));
                isLit.push_back(true);
            }
            continue;
        }
        std::string inner = gen(p.expr.get(), 0);
        if (!p.spec.empty())
            inner = "py::fmt(" + gen(p.expr.get(), 0) + ", " + esc(p.spec) + ")";
        else if (p.conv == "r" || p.conv == "a")
            inner = "py::repr(" + inner + ")";
        else
            inner = "py::str(" + inner + ")";
        parts.push_back(inner);
        isLit.push_back(false);
    }
    if (parts.empty()) return "std::string(\"\")";
    if (parts.size() == 1) return isLit[0] ? ("std::string(" + parts[0] + ")") : parts[0];
    std::string r = parts[0];
    if (isLit[0]) r = "std::string(" + parts[0] + ")";
    for (size_t i = 1; i < parts.size(); ++i) r += " + " + parts[i];
    return r;
}

std::string Gen::genLambda(const Expr* e) {
    // A lambda that was hoisted to a namespace-scope variable is referred to by
    // name so that every use denotes the same type.
    auto la = lambdaAlias.find(e);
    if (la != lambdaAlias.end()) return la->second;
    // Capture by value: a Python lambda may escape the enclosing scope, and a
    // by-reference capture would dangle (and makes the closure unabassignable).
    // In a namespace-scope decltype no capture-default is allowed.
    std::string s = inGlobalDecltype ? "[](" : "[=](";
    for (size_t i = 0; i < e->argNames.size(); ++i) {
        if (i) s += ", ";
        if (!e->argNames[i].empty() && e->argNames[i][0] == '*') {
            s += "auto... " + e->argNames[i].substr(1);
        } else {
            // A defaulted `auto` parameter cannot be deduced from its default.
            const Expr* def = (i < e->defaults.size()) ? e->defaults[i].get() : nullptr;
            std::string dt = def ? infer(def) : "";
            if (!dt.empty())
                s += dt + " " + e->argNames[i] + " = " + gen(def, 0);
            else {
                s += "auto " + e->argNames[i];
                if (def) s += " = " + gen(def, 0);
            }
        }
    }
    s += ") { return " + gen(e->a.get(), 0) + "; }";
    return s;
}

std::string Gen::genBinOp(const Expr* e, int parent) {
    const std::string& op = e->s;
    std::string a = gen(e->a.get(), opPrec(op));
    std::string b = gen(e->b.get(), opPrec(op) + 1);
    std::string ta = infer(e->a.get()), tb = infer(e->b.get());
    std::string r;

    if (op == "/")
        r = "py::truediv(" + a + ", " + b + ")";
    else if (op == "//")
        r = "py::floordiv(" + a + ", " + b + ")";
    else if (op == "%")
        r = "py::pymod(" + a + ", " + b + ")";
    else if (op == "**")
        r = "py::power(" + a + ", " + b + ")";
    else if (op == "+" && isStrT(ta) && isStrT(tb))
        r = a + " + " + b;
    else if (op == "+" && isStrT(ta))
        r = a + " + py::str(" + b + ")";
    else if (op == "+" && isStrT(tb))
        r = "py::str(" + a + ") + " + b;
    else if (op == "+" && isVectorT(ta) && isVectorT(tb))
        r = "py::concat(" + a + ", " + b + ")";
    else if (op == "*" && isStrT(ta) && isIntT(tb))
        r = "py::repeat(" + a + ", " + b + ")";
    else if (op == "*" && isIntT(ta) && isStrT(tb))
        r = "py::repeat(" + b + ", " + a + ")";
    else if (op == "*" && isVectorT(ta) && isIntT(tb))
        r = "py::repeat(" + a + ", " + b + ")";
    else if (op == "*" && isIntT(ta) && isVectorT(tb))
        r = "py::repeat(" + b + ", " + a + ")";
    else if (op == "|" && isSetT(ta) && isSetT(tb))
        r = "py::set_union(" + a + ", " + b + ")";
    else if (op == "&" && isSetT(ta) && isSetT(tb))
        r = "py::set_inter(" + a + ", " + b + ")";
    else if (op == "-" && isSetT(ta) && isSetT(tb))
        r = "py::set_diff(" + a + ", " + b + ")";
    else
        r = a + " " + op + " " + b;

    if (parent > opPrec(op)) r = "(" + r + ")";
    return r;
}

std::string Gen::genCompare(const Expr* e) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < e->ops.size(); ++i) {
        const std::string& op = e->ops[i];
        std::string l = gen(e->items[i].get(), 6);
        std::string r = gen(e->items[i + 1].get(), 6);
        if (op == "in") {
            parts.push_back("py::contains(" + gen(e->items[i + 1].get(), 0) + ", " +
                            gen(e->items[i].get(), 0) + ")");
        } else if (op == "not in") {
            parts.push_back("!py::contains(" + gen(e->items[i + 1].get(), 0) + ", " +
                            gen(e->items[i].get(), 0) + ")");
        } else if (op == "is") {
            parts.push_back("py::same(" + l + ", " + r + ")");
        } else if (op == "is not") {
            parts.push_back("!py::same(" + l + ", " + r + ")");
        } else {
            parts.push_back(l + " " + op + " " + r);
        }
    }
    return joinStr(parts, " && ");
}

std::string Gen::genAttr(const Expr* e) {
    if (e->a && e->a->kind == EK::Name && e->a->s == "self" && !F->cls.empty())
        return "this->" + e->s;
    if (e->a && e->a->kind == EK::Name) {
        auto mi = moduleAlias.find(e->a->s);
        if (mi != moduleAlias.end()) {
            const std::string& mod = mi->second;
            if (mod == "math") {
                if (e->s == "pi") return "py::pi()";
                if (e->s == "e") return "py::e()";
                if (e->s == "tau") return "(2.0 * py::pi())";
                if (e->s == "inf") return "py::inf()";
            }
            if (mod == "sys" && e->s == "argv") return "py::argv_store";
        }
    }
    std::string bt = infer(e->a.get());
    std::string base = gen(e->a.get(), 15);
    if (isMapT(bt) && e->s == "items") return base;
    return base + "." + e->s;
}

std::string Gen::genSubscript(const Expr* e) {
    std::string bt = infer(e->a.get());
    std::string b = gen(e->a.get(), 15);
    std::string idx = gen(e->b.get(), 0);
    if (isVectorT(bt) || isStrT(bt)) return "py::item(" + b + ", " + idx + ")";
    if (isMapT(bt)) return "py::dget(" + b + ", " + idx + ")";
    return b + "[" + idx + "]";
}

std::string Gen::genSlice(const Expr* e) {
    std::string b = gen(e->a.get(), 15);
    std::string lo = e->b ? gen(e->b.get(), 0) : "py::NONE_LL";
    std::string hi = e->c ? gen(e->c.get(), 0) : "py::NONE_LL";
    if (e->d)
        return "py::slice_step(" + b + ", " + lo + ", " + hi + ", " + gen(e->d.get(), 0) + ")";
    return "py::slice(" + b + ", " + lo + ", " + hi + ")";
}

bool Gen::isRangeCall(const Expr* e) const {
    return e && e->kind == EK::Call && e->a && e->a->kind == EK::Name && e->a->s == "range" &&
           !e->items.empty();
}

std::string Gen::genCall(const Expr* e) {
    const Expr* callee = e->a.get();
    if (!callee) return "";

    auto rawArgs = [&](size_t from) {
        std::string s;
        for (size_t i = from; i < e->items.size(); ++i) {
            if (i > from) s += ", ";
            s += gen(e->items[i].get(), 0);
        }
        return s;
    };
    // Positional arguments followed by keyword values (keyword names cannot be
    // expressed in C++, so they are appended in written order).
    auto callArgs = [&]() {
        std::string s = rawArgs(0);
        for (const auto& kv : e->kwargs) {
            if (!s.empty()) s += ", ";
            s += gen(kv.second.get(), 0);
        }
        return s;
    };

    if (callee->kind == EK::Name) {
        const std::string& n = callee->s;
        std::string fromMod;
        auto fi = fromImport.find(n);
        if (fi != fromImport.end()) fromMod = fi->second;

        // A global holding a closure is stored in a std::optional.
        if (fromMod.empty() && lazyGlobals.count(n) && !F->params.count(n) &&
            !F->declared.count(n))
            return "(*" + n + ")(" + callArgs() + ")";

        // A user-defined function wins over any builtin/module helper with the
        // same name (e.g. a user `factorial` must not become py::factorial).
        if (fromMod.empty()) {
            auto uc = callables.find(n);
            if (uc != callables.end() && uc->second.cls.empty()) {
                // Wrap bare string literals in std::string so that template
                // argument deduction picks std::string instead of const char*.
                std::string s;
                for (size_t i = 0; i < e->items.size(); ++i) {
                    if (i) s += ", ";
                    const Expr* x = e->items[i].get();
                    if (x->kind == EK::StrLit)
                        s += "std::string(" + esc(x->s) + ")";
                    else
                        s += gen(x, 0);
                }
                for (const auto& kv : e->kwargs) {
                    if (!s.empty()) s += ", ";
                    s += gen(kv.second.get(), 0);
                }
                return n + "(" + s + ")";
            }
        }

        if (n == "print") return "py::str(" + rawArgs(0) + ")";
        if (n == "len") {
            // len(*args) on a parameter pack: the pack has no .size().
            if (!e->items.empty() && e->items[0]->kind == EK::Name &&
                isStarParam(e->items[0]->s))
                return "static_cast<long long>(sizeof...(" + e->items[0]->s + "))";
            return "py::len(" + rawArgs(0) + ")";
        }
        if (n == "range") return "py::xrange(" + rawArgs(0) + ")";
        if (n == "int") return e->items.empty() ? "0LL" : ("py::to_int(" + rawArgs(0) + ")");
        if (n == "float") return e->items.empty() ? "0.0" : ("py::to_float(" + rawArgs(0) + ")");
        if (n == "str")
            return e->items.empty() ? "std::string(\"\")" : ("py::str(" + rawArgs(0) + ")");
        if (n == "repr") return "py::repr(" + rawArgs(0) + ")";
        if (n == "bool") return e->items.empty() ? "false" : ("py::to_bool(" + rawArgs(0) + ")");
        if (n == "abs") return "py::py_abs(" + rawArgs(0) + ")";
        if (n == "round") return "py::py_round(" + rawArgs(0) + ")";
        if (n == "min" || n == "max") {
            const std::string fn = (n == "min") ? "py::py_min" : "py::py_max";
            const std::string sf = (n == "min") ? "py::py_min_of" : "py::py_max_of";
            if (e->items.size() == 1) return fn + "(" + rawArgs(0) + ")";
            return sf + "(" + rawArgs(0) + ")";
        }
        if (n == "sum") return "py::py_sum(" + rawArgs(0) + ")";
        if (n == "sorted") {
            bool rev = false;
            std::string key;
            for (const auto& kv : e->kwargs) {
                if (kv.first == "reverse" && kv.second->kind == EK::BoolLit) rev = kv.second->boolLit;
                else if (kv.first == "key") key = gen(kv.second.get(), 0);
            }
            std::string a = e->items.empty() ? std::string() : gen(e->items[0].get(), 0);
            if (!key.empty()) {
                std::string s = "py::py_sorted(" + a + ", " + key;
                if (rev) s += ", true";
                return s + ")";
            }
            return rev ? ("py::py_sorted_rev(" + a + ")") : ("py::py_sorted(" + a + ")");
        }
        if (n == "reversed") return "py::reversed(" + rawArgs(0) + ")";
        if (n == "enumerate") {
            if (e->items.size() > 1) return "py::enumerate_from(" + rawArgs(0) + ")";
            return "py::enumerate(" + rawArgs(0) + ")";
        }
        if (n == "zip") return "py::zip2(" + rawArgs(0) + ")";
        if (n == "tuple") {
            if (e->items.empty()) return "py::tup<long long>{}";
            return "py::to_tuple(" + rawArgs(0) + ")";
        }
        if (n == "list") {
            // list(range(...)) is just the range itself -- forward the *inner*
            // call's arguments, not the (empty) ones left of it.
            if (!e->items.empty() && isRangeCall(e->items[0].get())) {
                const Expr* inner = e->items[0].get();
                std::string s;
                for (size_t i = 0; i < inner->items.size(); ++i) {
                    if (i) s += ", ";
                    s += gen(inner->items[i].get(), 0);
                }
                return "py::xrange(" + s + ")";
            }
            std::string it = e->items.empty() ? std::string() : infer(e->items[0].get());
            if (isMapT(it)) return "py::to_list_keys(" + rawArgs(0) + ")";
            if (isSetT(it)) return "py::to_list_set(" + rawArgs(0) + ")";
            return "py::to_list(" + rawArgs(0) + ")";
        }
        if (n == "input") return "py::input(" + rawArgs(0) + ")";
        if (n == "map") return "py::py_map(" + rawArgs(0) + ")";
        if (n == "filter") return "py::py_filter(" + rawArgs(0) + ")";
        if (n == "reversed") return "py::py_reversed(" + rawArgs(0) + ")";
        if (n == "chr") return "py::chr_of(" + rawArgs(0) + ")";
        if (n == "ord") return "py::ord_of(" + rawArgs(0) + ")";
        if (n == "isinstance") return "true";
        if (n == "any") return "py::any_of(" + rawArgs(0) + ")";
        if (n == "all") return "py::all_of(" + rawArgs(0) + ")";
        if (n == "format") {
            if (e->items.size() >= 2)
                return "py::fmt(" + gen(e->items[0].get(), 0) + ", " + gen(e->items[1].get(), 0) +
                       ")";
            return "py::str(" + rawArgs(0) + ")";
        }
        if (n == "factorial") return "py::factorial(" + rawArgs(0) + ")";
        if (n == "gcd") return "py::gcd_ll(" + rawArgs(0) + ")";
        if (n == "exit") return "py::exit_(" + (e->items.empty() ? "0" : rawArgs(0)) + ")";
        if (n == "id" || n == "hash") return "0LL";

        if (fromMod == "math" && mathFuncs().count(n)) {            // Python's floor/ceil/trunc return int.
            if (n == "floor" || n == "ceil" || n == "trunc")
                return "static_cast<long long>(" + mathFuncs().at(n) + "(" + rawArgs(0) + "))";
            std::string s = mathFuncs().at(n) + "(";
            for (size_t i = 0; i < e->items.size(); ++i) {
                if (i) s += ", ";
                s += "static_cast<double>(" + gen(e->items[i].get(), 0) + ")";
            }
            return s + ")";
        }
        return n + "(" + rawArgs(0) + ")";
    }

    if (callee->kind == EK::Attr) {
        const Expr* base = callee->a.get();
        const std::string& m = callee->s;
        std::string bt = infer(base);
        std::string b = gen(base, 15);

        if (base && base->kind == EK::Name) {
            auto mi = moduleAlias.find(base->s);
            if (mi != moduleAlias.end()) {
                const std::string& mod = mi->second;
                if (mod == "math") {
                    if (m == "factorial") return "py::factorial(" + rawArgs(0) + ")";
                    if (m == "gcd") return "py::gcd_ll(" + rawArgs(0) + ")";
                    if (m == "degrees") return "((" + rawArgs(0) + ") * 180.0 / py::pi())";
                    if (m == "radians") return "((" + rawArgs(0) + ") * py::pi() / 180.0)";
                    if (m == "floor" || m == "ceil" || m == "trunc")
                        return "static_cast<long long>(" + mathFuncs().at(m) + "(" + rawArgs(0) + "))";
                    if (mathFuncs().count(m)) {
                        std::string s = mathFuncs().at(m) + "(";
                        for (size_t i = 0; i < e->items.size(); ++i) {
                            if (i) s += ", ";
                            s += "static_cast<double>(" + gen(e->items[i].get(), 0) + ")";
                        }
                        return s + ")";
                    }
                }
                if (mod == "random") {
                    if (m == "random") return "py::random()";
                    if (m == "randint") return "py::randint(" + rawArgs(0) + ")";
                    if (m == "randrange") return "py::randint(0, (" + rawArgs(0) + ") - 1)";
                    if (m == "choice") return "py::choice(" + rawArgs(0) + ")";
                    if (m == "shuffle") return "(py::shuffle(" + rawArgs(0) + "), void())";
                    if (m == "uniform") return "py::uniform(" + rawArgs(0) + ")";
                }
                if (mod == "sys" && m == "exit") return "py::exit_(" + rawArgs(0) + ")";
                if (mod == "time" && m == "time") return "py::time_now()";
            }
        }

        if (isStrT(bt)) {
            if (m == "upper") return "py::upper(" + b + ")";
            if (m == "lower") return "py::lower(" + b + ")";
            if (m == "strip") return "py::strip(" + b + ")";
            if (m == "lstrip") return "py::lstrip(" + b + ")";
            if (m == "rstrip") return "py::rstrip(" + b + ")";
            if (m == "split")
                return e->items.empty() ? ("py::split(" + b + ")")
                                        : ("py::split(" + b + ", " + rawArgs(0) + ")");
            if (m == "join") return "py::join(" + b + ", " + rawArgs(0) + ")";
            if (m == "replace") return "py::replace(" + b + ", " + rawArgs(0) + ")";
            if (m == "startswith") return "py::startswith(" + b + ", " + rawArgs(0) + ")";
            if (m == "endswith") return "py::endswith(" + b + ", " + rawArgs(0) + ")";
            if (m == "find") return "py::find(" + b + ", " + rawArgs(0) + ")";
            if (m == "rfind") return "py::rfind(" + b + ", " + rawArgs(0) + ")";
            if (m == "count") return "py::count_sub(" + b + ", " + rawArgs(0) + ")";
            if (m == "index") return "py::find(" + b + ", " + rawArgs(0) + ")";
            if (m == "title") return "py::title(" + b + ")";
            if (m == "capitalize") return "py::capitalize(" + b + ")";
            if (m == "zfill") return "py::zfill(" + b + ", " + rawArgs(0) + ")";
            if (m == "isdigit") return "py::isdigit_(" + b + ")";
            if (m == "isalpha") return "py::isalpha_(" + b + ")";
            if (m == "isspace") return "py::isspace_(" + b + ")";
            if (m == "isalnum") return "py::isalnum_(" + b + ")";
            if (m == "format")
                return e->items.empty() ? b : ("py::fmt(" + b + ", " + rawArgs(0) + ")");
        }
        if (isVectorT(bt)) {
            if (m == "append") return "py::append(" + b + ", " + rawArgs(0) + ")";
            if (m == "extend") return "py::extend(" + b + ", " + rawArgs(0) + ")";
            if (m == "pop")
                return e->items.empty() ? ("py::pop(" + b + ")")
                                        : ("py::pop(" + b + ", " + rawArgs(0) + ")");
            if (m == "remove") return "py::remove(" + b + ", " + rawArgs(0) + ")";
            if (m == "index") return "py::index_of(" + b + ", " + rawArgs(0) + ")";
            if (m == "count") return "py::count_in(" + b + ", " + rawArgs(0) + ")";
            if (m == "sort") {
                bool rev = false;
                std::string key;
                for (const auto& kv : e->kwargs) {
                    if (kv.first == "reverse" && kv.second->kind == EK::BoolLit) rev = kv.second->boolLit;
                    else if (kv.first == "key") key = gen(kv.second.get(), 0);
                }
                if (!key.empty())
                    return "(py::py_sort_by(" + b + ", " + key + (rev ? ", true" : "") + "), void())";
                return "(py::sort_inplace(" + b + (rev ? ", true" : "") + "), void())";
            }
            if (m == "reverse") return "(py::reverse_inplace(" + b + "), void())";
            if (m == "clear") return "(" + b + ".clear(), void())";
            if (m == "insert")
                return "(" + b + ".insert(" + b + ".begin() + (" + rawArgs(0) + ")), void())";
            if (m == "copy") return b;
        }
        if (isSetT(bt)) {
            if (m == "add") return "(" + b + ".insert(" + rawArgs(0) + "), void())";
            if (m == "discard") return "(" + b + ".erase(" + rawArgs(0) + "), void())";
            if (m == "remove") return "py::set_remove(" + b + ", " + rawArgs(0) + ")";
            if (m == "clear") return "(" + b + ".clear(), void())";
            if (m == "copy") return b;
            if (m == "union") return "py::set_union(" + b + ", " + rawArgs(0) + ")";
            if (m == "intersection") return "py::set_inter(" + b + ", " + rawArgs(0) + ")";
            if (m == "difference") return "py::set_diff(" + b + ", " + rawArgs(0) + ")";
            if (m == "issubset") return "py::set_subset(" + b + ", " + rawArgs(0) + ")";
            if (m == "issuperset") return "py::set_subset(" + rawArgs(0) + ", " + b + ")";
        }
        if (isMapT(bt)) {
            if (m == "items") return b;
            if (m == "keys") return "py::to_list_keys(" + b + ")";
            if (m == "values") return "py::map_values(" + b + ")";
            if (m == "get") return "py::dget_or(" + b + ", " + rawArgs(0) + ")";
            if (m == "clear") return "(" + b + ".clear(), void())";
            if (m == "copy") return b;
        }

        if (m == "append") return "py::append(" + b + ", " + rawArgs(0) + ")";
        if (m == "upper") return "py::upper(" + b + ")";
        if (m == "lower") return "py::lower(" + b + ")";
        if (m == "strip") return "py::strip(" + b + ")";
        if (m == "split")
            return e->items.empty() ? ("py::split(" + b + ")")
                                    : ("py::split(" + b + ", " + rawArgs(0) + ")");
        if (m == "join") return "py::join(" + b + ", " + rawArgs(0) + ")";

        return b + "." + m + "(" + rawArgs(0) + ")";
    }
    // Calling the result of an expression, e.g. compose(f, g)(5).
    std::string calleeText = gen(callee, 15);
    if (!calleeText.empty()) return "(" + calleeText + ")(" + callArgs() + ")";
    return "";
}

// ===========================================================================
// statement emission
// ===========================================================================
void Gen::emitBody(const std::vector<StmtP>& body) {
    for (const auto& s : body) {
        switch (s->kind) {
            case SK::FuncDef:
            case SK::ClassDef:
                if (F->key.empty()) continue;
                emitNestedFunc(s.get());
                continue;
            case SK::Import:
            case SK::FromImport:
            case SK::Global:
            case SK::Nonlocal:
                continue;
            default:
                emitStmt(s.get());
        }
    }
}

void Gen::emitNestedFunc(const Stmt* s) {
    if (s->kind == SK::ClassDef) {
        line("// py2cpp: nested class '" + s->s + "' was not converted");
        warn("nested class '" + s->s + "' was skipped");
        return;
    }
    // Capture by value: the closure may be returned from the enclosing
    // function (a by-reference capture would dangle and is not assignable).
    std::string sig = "[=](";
    bool first = true;
    std::vector<std::string> added;
    std::map<std::string, std::string> types;  // params given a concrete type
    for (const auto& p : s->params) {
        if (p.name == "self") continue;
        if (!first) sig += ", ";
        first = false;
        // A defaulted `auto` parameter cannot be deduced from its default, so
        // parameters that carry a Python default get the concrete inferred type.
        std::string pt = p.def ? infer(p.def.get()) : "";
        if (!pt.empty()) {
            sig += pt + " " + p.name + " = " + gen(p.def.get(), 0);
            types[p.name] = pt;
        } else {
            sig += "auto " + p.name;
            if (p.def) sig += " = " + gen(p.def.get(), 0);
        }
        added.push_back(p.name);
    }
    sig += ") {";
    line("auto " + s->s + " = " + sig);
    ind++;

    std::map<std::string, std::string> savedEnv;
    std::set<std::string> savedDecl;
    for (const auto& n : added) {
        if (F->env.count(n)) savedEnv[n] = F->env[n];
        if (F->declared.count(n)) savedDecl.insert(n);
        F->env[n] = types.count(n) ? types[n] : "";
        F->declared.insert(n);
    }
    NameInfo info;
    collectInfoBody(s->body, info);
    std::map<std::string, std::vector<const Expr*>> ap;
    collectAppendBody(s->body, ap);
    std::set<std::string> decl = F->declared;
    std::vector<std::string> hoist;
    std::set<std::string> skip;
    for (const auto& p : s->params) skip.insert(p.name);
    auto env2 = inferLocalTypes(info, ap, F->env, skip, decl, hoist);
    std::set<std::string> fallback(hoist.begin(), hoist.end());
    for (const auto& n : hoist) {
        std::string t = env2.count(n) ? env2[n] : "";
        if (!t.empty()) line(t + " " + n + "{};");
    }
    for (const auto& n : hoist) {
        F->env[n] = env2.count(n) ? env2[n] : "";
        F->declared.insert(n);
    }
    F->hoistFallback = fallback;
    emitBody(s->body);
    F->hoistFallback.clear();

    for (const auto& n : added) {
        if (savedEnv.count(n))
            F->env[n] = savedEnv[n];
        else
            F->env.erase(n);
        if (!savedDecl.count(n)) F->declared.erase(n);
    }
    ind--;
    line("};");
}

void Gen::emitWith(const Stmt* s) {
    const Expr* saved = s->a.get();
    if (!s->s.empty()) {
        line("auto " + s->s + " = " + gen(saved, 0) + ";");
        F->declared.insert(s->s);
        F->hoistFallback.insert(s->s);
        F->env[s->s] = infer(saved);
    } else {
        line(gen(saved, 0) + ";");
    }
    emitBody(s->body);
}

void Gen::emitStmt(const Stmt* s) {
    switch (s->kind) {
        case SK::Pass:
            return;
        case SK::Break:
            line("break;");
            return;
        case SK::Continue:
            line("continue;");
            return;
        case SK::ExprStmt:
            // A Python generator cannot be mapped onto C++ without coroutines;
            // say so instead of silently dropping the value.
            if (s->s == "yield") {
                line("// py2cpp: `yield` is not supported -- generators are skipped");
                warn("`yield` is not supported (generators are skipped) at line " +
                     std::to_string(s->line));
                return;
            }
            emitExprStmt(s->a.get());
            return;
        case SK::Assign:
            emitAssign(s);
            return;
        case SK::AnnAssign:
            emitOneAssign(s->a.get(), s->c ? s->c.get() : nullptr);
            return;
        case SK::AugAssign: {
            const Expr* t = s->a.get();
            std::string lhs = gen(t, 0);
            std::string rhs = gen(s->b.get(), 0);
            std::string tt = infer(t);
            const std::string& op = s->s;
            if (op == "+=" && isVectorT(tt))
                line(lhs + " = py::concat(" + lhs + ", " + rhs + ");");
            else if (op == "+=" && isStrT(tt))
                line(lhs + " += py::str(" + rhs + ");");
            else if (op == "/=")
                line(lhs + " = py::truediv(" + lhs + ", " + rhs + ");");
            else if (op == "//=")
                line(lhs + " = py::floordiv(" + lhs + ", " + rhs + ");");
            else if (op == "%=")
                line(lhs + " = py::pymod(" + lhs + ", " + rhs + ");");
            else if (op == "**=")
                line(lhs + " = py::power(" + lhs + ", " + rhs + ");");
            else
                line(lhs + " " + op + " " + rhs + ";");
            return;
        }
        case SK::If: {
            line("if (" + gen(s->a.get(), 0) + ") {");
            ind++;
            emitBody(s->body);
            ind--;
            if (!s->orelse.empty()) {
                line("} else {");
                ind++;
                emitBody(s->orelse);
                ind--;
            }
            line("}");
            return;
        }
        case SK::While: {
            line("while (" + gen(s->a.get(), 0) + ") {");
            ind++;
            emitBody(s->body);
            ind--;
            line("}");
            return;
        }
        case SK::For:
            emitFor(s);
            return;
        case SK::Return:
            if (!s->a)
                line("return;");
            else
                line("return " + gen(s->a.get(), 0) + ";");
            return;
        case SK::Try:
            emitTry(s);
            return;
        case SK::Raise:
            emitRaise(s);
            return;
        case SK::Assert:
            if (s->b)
                line("py::assert_(static_cast<bool>(" + gen(s->a.get(), 0) + "), py::str(" +
                     gen(s->b.get(), 0) + "));");
            else
                line("py::assert_(static_cast<bool>(" + gen(s->a.get(), 0) + "));");
            return;
        case SK::Del:
            return;
        case SK::With:
            emitWith(s);
            return;
        default:
            return;
    }
}

void Gen::emitOneAssign(const Expr* target, const Expr* value) {
    if (!target) return;
    if (target->kind == EK::Name) {
        const std::string& n = target->s;
        // A std::optional global (closure, ...) is filled in place: the stored
        // type may not be copy-assignable at all.
        if (F->key.empty() && lazyGlobals.count(n)) {
            line(n + ".emplace(" + (value ? gen(value, 0) : std::string("{}")) + ");");
            return;
        }
        std::string v;
        if (!value)
            v = "{}";
        else if (value->kind == EK::NoneLit && envOf(n) != "py::NoneType" && !F->params.count(n))
            v = "{}";
        else
            v = gen(value, 0);

        bool known = F->key.empty() || F->declared.count(n) || F->params.count(n) ||
                     F->globalNames.count(n);
        // An empty literal must adopt the target's declared type: the literal
        // alone cannot express the element type (`by_first = {}` where the
        // inference found dict<str, vector<str>>).
        if (known && value && value->items.empty() &&
            (value->kind == EK::ListLit || value->kind == EK::SetLit || value->kind == EK::DictLit))
            v = "{}";
        if (known) {
            line(n + " = " + v + ";");
        } else {
            line("auto " + n + " = " + v + ";");
            F->declared.insert(n);
            if (value) {
                std::string t = infer(value);
                if (!t.empty()) F->env[n] = t;
            }
        }
        return;
    }
    if (target->kind == EK::TupleLit || target->kind == EK::ListLit) {
        std::vector<const Expr*> names;
        collectTargetNames(target, names);
        std::vector<std::string> nm;
        bool allDeclared = true;
        for (auto x : names) {
            nm.push_back(x->s);
            if (!F->declared.count(x->s) && !F->params.count(x->s)) allDeclared = false;
        }
        std::string v = value ? gen(value, 0) : std::string("std::make_tuple()");
        if (allDeclared) {
            line("std::tie(" + joinStr(nm, ", ") + ") = " + v + ";");
        } else {
            line("auto [" + joinStr(nm, ", ") + "] = " + v + ";");
            for (auto x : names) {
                F->declared.insert(x->s);
                F->hoistFallback.insert(x->s);
            }
        }
        return;
    }
    if (target->kind == EK::Subscript) {
        std::string b = gen(target->a.get(), 15);
        std::string i = gen(target->b.get(), 0);
        std::string v;
        // An empty literal store must adopt the container's element type, which
        // the literal alone cannot express (decltype(m[k]) is exactly it).
        if (value && value->items.empty() &&
            (value->kind == EK::ListLit || value->kind == EK::SetLit || value->kind == EK::DictLit))
            v = "std::remove_reference_t<decltype(" + b + "[" + i + "])>{}";
        else
            v = value ? gen(value, 0) : std::string("{}");
        line(b + "[" + i + "] = " + v + ";");
        return;
    }
    line(gen(target, 0) + " = " + (value ? gen(value, 0) : std::string("{}")) + ";");
}

void Gen::emitAssign(const Stmt* s) {
    if (s->values.empty()) {
        for (const auto& t : s->targets) emitOneAssign(t.get(), nullptr);
        return;
    }
    for (const auto& t : s->targets)
        for (const auto& v : s->values) emitOneAssign(t.get(), v.get());
}

void Gen::emitFor(const Stmt* s) {
    const Expr* iter = s->iter.get();
    std::vector<const Expr*> names;
    collectTargetNames(s->a.get(), names);

    if (isRangeCall(iter) && names.size() == 1 && iter->items.size() <= 3) {
        const std::string& v = names[0]->s;
        size_t n = iter->items.size();
        std::string start = (n == 1) ? "0" : gen(iter->items[0].get(), 0);
        std::string stop = (n == 1) ? gen(iter->items[0].get(), 0) : gen(iter->items[1].get(), 0);
        bool declared = F->declared.count(v) != 0;
        std::string head = declared ? "" : "long long ";
        if (!declared) F->declared.insert(v);
        if (n < 3) {
            line("for (" + head + v + " = " + start + "; " + v + " < " + stop + "; ++" + v + ") {");
        } else {
            const Expr* step = iter->items[2].get();
            if (step->kind == EK::IntLit && step->i > 0) {
                line("for (" + head + v + " = " + start + "; " + v + " < " + stop + "; " + v +
                     " += " + std::to_string(step->i) + ") {");
            } else if (step->kind == EK::IntLit && step->i < 0) {
                line("for (" + head + v + " = " + start + "; " + v + " > " + stop + "; " + v +
                     " += " + std::to_string(step->i) + ") {");
            } else {
                std::string st = gen(step, 0);
                line("for (long long __py_step = " + st + ", " + v + " = " + start +
                     "; __py_step > 0 ? " + v + " < " + stop + " : " + v + " > " + stop + "; " +
                     v + " += __py_step) {");
            }
        }
        ind++;
        F->env[v] = "long long";
        emitBody(s->body);
        ind--;
        line("}");
        return;
    }

    std::string it = gen(iter, 0);
    // A `*args` parameter is a C++ pack; materialise it into a list to iterate.
    if (iter->kind == EK::Name) {
        auto ci = callables.find(F->key);
        if (ci != callables.end() && ci->second.fn) {
            for (const auto& p : ci->second.fn->params)
                if (p.name == iter->s && p.isStar) it = "py::make_list(" + iter->s + "...)";
        }
    }
    // A bare string literal is `const char[N]`; its trailing '\0' must not be
    // iterated, so materialise it as a std::string.  (Only literals are wrapped:
    // wrapping an arbitrary temporary in py::seq() would dangle, because the
    // reference returned by seq() outlives the temporary argument.)
    if (iter->kind == EK::StrLit) it = "std::string(" + it + ")";

    if (names.size() == 1) {
        const std::string& v = names[0]->s;
        std::string t = inferIterElem(iter);
        // Python iterates a str as a sequence of 1-character strings; C++ would
        // hand out raw chars, so go through py_chars() to keep the element type
        // consistent with the inferred one.  (Only when the *iterable* is a
        // string -- a list of strings already yields std::string elements.)
        if (isStrT(infer(iter))) it = "py::py_chars(" + it + ")";
        if (F->declared.count(v) != 0) {
            line("for (auto&& __py_it : " + it + ") {");
            ind++;
            line(v + " = __py_it;");
        } else {
            line("for (" + (t.empty() ? std::string("auto ") : t + " ") + v + " : " + it + ") {");
            ind++;
            F->declared.insert(v);
            if (!t.empty()) F->env[v] = t;
        }
        emitBody(s->body);
        ind--;
        line("}");
        return;
    }

    std::string inner;
    for (size_t k = 0; k < names.size(); ++k) {
        if (k) inner += ", ";
        inner += names[k]->s;
    }
    line("for (auto& [" + inner + "] : " + it + ") {");
    ind++;
    for (auto x : names) {
        F->declared.insert(x->s);
        F->hoistFallback.insert(x->s);
    }
    emitBody(s->body);
    ind--;
    line("}");
}

void Gen::emitTry(const Stmt* s) {
    if (s->handlers.empty()) {
        if (s->finalbody.empty()) {
            emitBody(s->body);
        } else {
            emitBody(s->body);
            emitBody(s->finalbody);
        }
        return;
    }
    line("try {");
    ind++;
    emitBody(s->body);
    ind--;
    for (const auto& h : s->handlers) {
        std::string t = excCppType(h.type);
        if (t.empty())
            line("} catch (...) {");
        else if (h.name.empty())
            line("} catch (const " + t + "&) {");
        else
            line("} catch (const " + t + "& __py_exc) {");
        ind++;
        if (!h.name.empty()) {
            line("std::string " + h.name + " = __py_exc.what();");
            F->declared.insert(h.name);
            F->env[h.name] = "std::string";
        }
        emitBody(h.body);
        ind--;
    }
    line("}");
    if (!s->orelse.empty()) emitBody(s->orelse);
    if (!s->finalbody.empty()) emitBody(s->finalbody);
}

void Gen::emitRaise(const Stmt* s) {
    if (!s->a) {
        line("throw;");
        return;
    }
    if (s->a->kind == EK::Call && s->a->a && s->a->a->kind == EK::Name &&
        excNames().count(s->a->a->s)) {
        const std::string& tn = s->a->a->s;
        const Expr* call = s->a.get();
        std::string msg;
        if (call->items.empty())
            msg = esc(tn);
        else if (call->items.size() == 1 && call->items[0]->kind == EK::StrLit)
            msg = esc(call->items[0]->s);
        else {
            msg = "py::str(" + gen(call->items[0].get(), 0) + ")";
            for (size_t i = 1; i < call->items.size(); ++i)
                msg += " + \", \" + py::str(" + gen(call->items[i].get(), 0) + ")";
        }
        line("throw py::" + tn + "(" + msg + ");");
        return;
    }
    line("throw py::Exception(py::str(" + gen(s->a.get(), 0) + "));");
}

void Gen::emitPrint(const Expr* call) {
    std::string sep = "\" \"";
    std::string end = "\"\\n\"";
    std::vector<std::string> args;
    std::string starArg;
    bool hasStar = false;

    for (const auto& a : call->items) {
        if (a->kind == EK::Starred) {
            hasStar = true;
            starArg = gen(a->a.get(), 0);
            continue;
        }
        args.push_back(gen(a.get(), 0));
    }
    for (const auto& kv : call->kwargs) {
        if (kv.first == "sep")
            sep = gen(kv.second.get(), 0);
        else if (kv.first == "end")
            end = gen(kv.second.get(), 0);
    }

    if (hasStar) {
        line("for (auto&& __py_e : " + starArg +
             ") { std::cout << py::str(__py_e) << " + sep + "; }");
        line("std::cout << " + end + ";");
        return;
    }
    std::string code = "std::cout";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) code += " << " + sep;
        code += " << py::str(" + args[i] + ")";
    }
    code += " << " + end + ";";
    line(code);
}

void Gen::emitExprStmt(const Expr* e) {
    if (!e) return;
    if (e->kind == EK::StrLit) {
        if (opt.emitComments) line("// " + e->s);
        return;
    }
    if (e->kind == EK::Name) return;
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Name && e->a->s == "print") {
        emitPrint(e);
        return;
    }
    line(gen(e, 0) + ";");
}

// ===========================================================================
// signatures
// ===========================================================================
std::string Gen::templateHeadOf(const Callable& c, bool withDefault) {
    std::vector<std::string> ps;
    size_t i = 0;
    for (const auto& p : c.fn->params) {
        if (p.name == "self" && !c.cls.empty()) continue;
        if (p.isStar) {
            ps.push_back("class... __Rest");
            i++;
            continue;
        }
        if (p.isKwStar) {
            // `**kwargs` cannot be modelled as a second parameter pack (the
            // first pack would become a non-deduced context).  The name is
            // declared inside the body as an empty mapping instead.
            continue;
        }
        std::string def;
        if (withDefault && p.def) {
            std::string t;
            auto fi = frames.find(c.key);
            if (fi != frames.end()) t = inferWith(p.def.get(), fi->second.env);
            if (t.empty()) t = "long long";
            def = " = " + t;
        }
        ps.push_back("class T" + std::to_string(i) + def);
        i++;
    }
    if (ps.empty()) return withDefault ? "template <class __T = void>" : "template <class __T>";
    return "template <" + joinStr(ps, ", ") + ">";
}

std::string Gen::paramListOf(const Callable& c, bool withDefault) {
    std::vector<std::string> ps;
    size_t i = 0;
    for (const auto& p : c.fn->params) {
        if (p.name == "self" && !c.cls.empty()) continue;
        if (p.isStar) {
            ps.push_back("__Rest... " + p.name);
            i++;
            continue;
        }
        if (p.isKwStar) continue;  // see templateHeadOf
        std::string s = "T" + std::to_string(i) + " " + p.name;
        if (withDefault && p.def) s += " = " + gen(p.def.get(), 0);
        ps.push_back(s);
        i++;
    }
    return joinStr(ps, ", ");
}

// ===========================================================================
// top level emission
// ===========================================================================
void Gen::emitForwardDecls() {
    bool any = false;
    for (const auto& key : callableOrder) {
        const Callable& c = callables[key];
        if (!c.cls.empty()) continue;
        // The declaration must match the definition exactly: recursive (in-cycle)
        // functions are emitted with an explicit return type, so their forward
        // declaration must use it too — otherwise the two templates differ and
        // every call becomes ambiguous.
        std::string rt = "auto";
        if (c.inCycle) {
            rt = inferReturn(c.key);
            if (rt.empty() || rt == "void") rt = "long long";
        }
        line(templateHeadOf(c, true));
        line(rt + " " + c.fn->s + "(" + paramListOf(c, true) + ");");
        any = true;
    }
    if (any) blank();
}

void Gen::emitStructs() {
    if (classOrder.empty()) return;
    for (const auto& cn : classOrder) {
        const Stmt* k = klasses[cn];
        std::string bases;
        if (!k->bases.empty()) bases = " : public " + joinStr(k->bases, ", public ");
        line("struct " + cn + bases + " {");
        ind++;
        for (const auto& m : memberOrder[cn]) {
            std::string t = members[cn][m];
            if (t.empty()) t = "long long";
            line(t + " " + m + "{};");
        }
        const Stmt* init = ctors.count(cn) ? ctors[cn] : nullptr;
        if (!init) {
            line(cn + "() = default;");
        } else {
            Param probe;
            std::vector<Param> realParams;
            int nReal = 0;
            for (const auto& p : init->params) {
                if (p.name == "self" && !p.isStar && !p.isKwStar) continue;
                nReal++;
            }
            (void)probe;
            if (nReal == 0) {
                line(cn + "();");
            } else {
                line(cn + "() = default;");
                Callable tmp;
                tmp.fn = init;
                tmp.key = cn + ".__init__";
                tmp.cls = cn;
                line(templateHeadOf(tmp, true));
                line(cn + "(" + paramListOf(tmp, true) + ");");
            }
        }
        for (const auto& m : k->body) {
            if (m->kind != SK::FuncDef || m->s == "__init__") continue;
            auto ci = callables.find(cn + "." + m->s);
            if (ci == callables.end()) continue;
            line(templateHeadOf(ci->second, true));
            line("auto " + m->s + "(" + paramListOf(ci->second, true) + ");");
        }
        ind--;
        line("};");
        blank();
    }
}

void Gen::emitHoists() {
    std::set<std::string> done;
    for (const auto& n : F->hoist) {
        if (F->params.count(n) || F->globalNames.count(n)) continue;
        if (!done.insert(n).second) continue;
        // Tuple-unpacked names are declared at their unpacking statement.
        if (F->info.destructured.count(n)) continue;
        std::string t = F->env.count(n) ? F->env[n] : "";
        if (!t.empty() && t != "void") {
            line(t + " " + n + "{};");
            F->declared.insert(n);
            continue;
        }
        const Expr* e2 = nullptr;
        auto ia = F->info.firstAssign.find(n);
        if (ia != F->info.firstAssign.end()) e2 = ia->second;
        bool bad = !e2 || e2->kind == EK::NoneLit;
        if (e2 && (e2->kind == EK::ListLit || e2->kind == EK::SetLit || e2->kind == EK::DictLit) &&
            e2->items.empty())
            bad = true;
        if (!bad) {
            line("decltype(" + gen(e2, 0) + ") " + n + "{};");
            F->declared.insert(n);
            F->hoistFallback.insert(n);
        }
    }
}

void Gen::emitGlobals() {
    Frame* saved = F;
    F = &frames[""];

    // Lambdas that appear *as a value* in a module-level assignment (not nested
    // inside another lambda, and not the body of a comprehension, which is
    // invoked immediately) are hoisted to namespace-scope variables first.
    auto collectTopLambdas = [](const Expr* e, std::vector<const Expr*>& out,
                                auto&& self) -> void {
        if (!e) return;
        if (e->kind == EK::Lambda) {
            out.push_back(e);
            return;
        }
        if (e->kind == EK::ListComp || e->kind == EK::DictComp || e->kind == EK::SetComp) return;
        for (const auto& x : e->items) self(x.get(), out, self);
        for (const auto& kv : e->kwargs) self(kv.second.get(), out, self);
        self(e->a.get(), out, self);
        self(e->b.get(), out, self);
        self(e->c.get(), out, self);
        self(e->d.get(), out, self);
    };

    size_t lamIdx = 0;
    for (const auto& n : moduleDeclOrder) {
        auto la0 = modInfo.firstAssign.find(n);
        if (la0 != modInfo.firstAssign.end() && la0->second) {
            std::vector<const Expr*> lams;
            collectTopLambdas(la0->second, lams, collectTopLambdas);
            for (auto* L : lams) {
                if (lambdaAlias.count(L)) continue;
                bool saved = inGlobalDecltype;
                inGlobalDecltype = true;  // namespace scope: no capture-default
                std::string text = genLambda(L);
                inGlobalDecltype = saved;
                std::string nm = "__lam_" + std::to_string(lamIdx++);
                lambdaAlias[L] = nm;
                line("inline auto " + nm + " = " + text + ";");
            }
        }

        std::string t = F->env.count(n) ? F->env[n] : "";
        if (!t.empty() && t != "void") {
            line(t + " " + n + "{};");
            continue;
        }
        const Expr* e2 = nullptr;
        auto ia = modInfo.firstAssign.find(n);
        if (ia != modInfo.firstAssign.end()) e2 = ia->second;
        bool bad = !e2 || e2->kind == EK::NoneLit;
        if (e2 && (e2->kind == EK::ListLit || e2->kind == EK::SetLit || e2->kind == EK::DictLit) &&
            e2->items.empty())
            bad = true;
        if (bad) {
            line("long long " + n + "{};  // py2cpp: type could not be inferred");
            continue;
        }
        // The decltype form may describe a type that is not default
        // constructible (a closure, for instance). Declare it as an optional so
        // it can be assigned later from main; uses are dereferenced.
        // Inside a namespace-scope decltype a lambda may not carry a
        // capture-default, so the unevaluated copy drops it (there are no
        // locals to capture out here anyway).
        inGlobalDecltype = true;
        std::string dtext = gen(e2, 0);
        inGlobalDecltype = false;
        line("std::optional<decltype(" + dtext + ")> " + n + ";");
        lazyGlobals.insert(n);
    }
    F = saved;
    if (!moduleDeclOrder.empty()) blank();
}

void Gen::emitCallable(const Callable& c) {
    Frame& f = frames[c.key];
    F = &f;

    std::string head = templateHeadOf(c, false);
    std::string qname = c.cls.empty() ? c.fn->s : (c.cls + "::" + c.fn->s);
    if (c.isCtor) qname = c.cls + "::" + c.cls;

    bool noTemplate = c.isCtor && paramListOf(c, false).empty();

    if (c.isCtor) {
        if (noTemplate) {
            line(qname + "() {");
        } else {
            line(head);
            line(qname + "(" + paramListOf(c, false) + ") {");
        }
    } else if (c.inCycle) {
        std::string rt = inferReturn(c.key);
        if (rt.empty() || rt == "void") {
            rt = "long long";
            warn("mutually recursive function '" + qname +
                 "' has no inferable return type; 'long long' was assumed");
        }
        line("// py2cpp: an explicit return type breaks the mutual recursion cycle");
        line(head);
        line(rt + " " + qname + "(" + paramListOf(c, false) + ") {");
    } else {
        line(head);
        line("auto " + qname + "(" + paramListOf(c, false) + ") {");
    }
    ind++;
    // `**kwargs` has no C++ counterpart: declare the name so the body still
    // compiles, but it will always be empty.
    for (const auto& p : c.fn->params) {
        if (!p.isKwStar || p.name == "self") continue;
        line("py::dict<std::string, std::string> " + p.name + "{};  // py2cpp: **kwargs is not forwarded");
        warn("'" + qname + "': **kwargs is not forwarded to C++ (treated as empty)");
        f.declared.insert(p.name);
        f.env[p.name] = "py::dict<std::string, std::string>";
    }
    emitHoists();
    emitBody(c.fn->body);
    ind--;
    line("}");
    blank();
}

void Gen::emitMain() {
    Frame& f = frames[""];
    F = &f;
    line("int main(int argc, char** argv) {");
    ind++;
    line("py::init_argv(argc, argv);");
    emitBody(prog);
    ind--;
    line("}");
    blank();
}

// ===========================================================================
// driver
// ===========================================================================
GenResult Gen::run() {
    collect();
    collectCallables();
    analyze();

    if (opt.includeRuntime) {
        if (opt.splitRuntime) {
            // Keep the translated program readable: the runtime goes into its
            // own header, and the .cpp just references it.
            out << "// generated by p2cpp -- the py:: runtime lives in " << kRuntimeHeaderName
                << "\n";
            out << "#include \"" << kRuntimeHeaderName << "\"\n\n";
            runtimeHeader = std::string("#pragma once\n") +
                            "// " + kRuntimeHeaderName +
                            " -- runtime support for code generated by p2cpp.\n"
                            "// Generated automatically; do not edit. Compile alongside the "
                            "generated .cpp.\n" +
                            PY_PRELUDE;
        } else {
            out << PY_PRELUDE;
            out << "\n";
        }
    }

    // forward declarations first so that ordering issues are explicit
    emitForwardDecls();
    emitStructs();

    // free function definitions, topologically ordered (dependencies first)
    for (const auto& key : emitOrder) {
        const Callable& c = callables[key];
        if (!c.cls.empty()) continue;
        emitCallable(c);
    }

    // class member definitions (ctors + methods) come after free functions
    for (const auto& cn : classOrder) {
        auto ci = ctors.find(cn);
        if (ci != ctors.end()) {
            auto key = cn + ".__init__";
            if (callables.count(key)) emitCallable(callables[key]);
        }
    }
    for (const auto& cn : classOrder) {
        const Stmt* k = klasses[cn];
        for (const auto& m : k->body) {
            if (m->kind != SK::FuncDef || m->s == "__init__") continue;
            auto key = cn + "." + m->s;
            if (callables.count(key)) emitCallable(callables[key]);
        }
    }

    emitGlobals();
    emitMain();

    GenResult r;
    r.code = out.str();
    r.runtimeHeader = std::move(runtimeHeader);
    r.warnings = warnings;
    return r;
}

}  // namespace

const char* const kRuntimeHeaderName = "py_runtime.h";

GenResult generateCpp(const std::vector<StmtP>& program, const CodeGenOptions& opt) {
    Gen g(program, opt);
    return g.run();
}

// ===========================================================================
// identifier mangling
// ===========================================================================
namespace {

bool isReservedIdent(const std::string& n) {
    static const std::set<std::string> kw = {
        // C++ keywords
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
        "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
        "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        // names used by the emitted program / runtime
        "main", "std", "py", "argc", "argv", "cout", "cerr", "cin", "endl", "printf", "sprintf",
        "snprintf", "malloc", "free", "abort", "exit", "memcpy", "memset", "strlen", "size_t",
        "ptr", "nullptr_t", "dlopen",
        // identifiers reserved by the generated code
        "__T", "__Rest", "__Kw", "__r", "__py_it", "__py_e", "__py_step",
    };
    if (kw.count(n)) return true;
    // generated template parameters T0, T1, ...
    if (n.size() >= 2 && n[0] == 'T') {
        bool digits = true;
        for (std::size_t i = 1; i < n.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(n[i]))) digits = false;
        if (digits) return true;
    }
    return false;
}

std::string mangleName(const std::string& n) {
    return isReservedIdent(n) ? n + "_" : n;
}

// Python builtins that the code generator translates itself. They must keep
// their name even when it is a C++ keyword (`int(...)`, `float(...)`, ...).
bool isPyBuiltinFn(const std::string& n) {
    static const std::set<std::string> b = {
        "print",    "len",      "range",    "int",      "float",  "str",      "repr",
        "bool",     "abs",      "round",    "min",      "max",    "sum",      "sorted",
        "enumerate", "zip",     "list",     "set",      "dict",   "tuple",    "input",
        "chr",      "ord",      "isinstance", "any",    "all",    "format",   "factorial",
        "gcd",      "exit",     "id",       "hash",     "map",    "filter",   "reversed",
        "type",     "pow",      "divmod",   "hex",      "oct",    "bin",      "open",
        "next",     "iter",     "object",   "super",    "slice",  "frozenset", "bytes",
    };
    return b.count(n) != 0;
}

void mangleExpr(Expr* e);

void mangleExprList(std::vector<ExprP>& v) {
    for (auto& x : v) mangleExpr(x.get());
}

void mangleExpr(Expr* e) {
    if (!e) return;
    switch (e->kind) {
        case EK::Name:
            e->s = mangleName(e->s);
            break;
        case EK::Attr:
            // Attribute names are member names; mangle so they stay consistent
            // with the struct field declarations.
            e->s = mangleName(e->s);
            break;
        case EK::Lambda:
            for (auto& a : e->argNames) a = mangleName(a);
            break;
        case EK::Call:
            // A builtin callee keeps its Python name: `int(x)` must stay `int`,
            // not become the C++ keyword-mangled `int_`.
            if (e->a && e->a->kind == EK::Name && isPyBuiltinFn(e->a->s)) {
                // leave the callee untouched
            } else {
                mangleExpr(e->a.get());
            }
            for (auto& x : e->items) mangleExpr(x.get());
            for (auto& kv : e->kwargs) mangleExpr(kv.second.get());
            return;
        default:
            break;
    }
    for (auto& kv : e->kwargs) kv.first = mangleName(kv.first);
    for (auto& p : e->parts) mangleExpr(p.expr.get());
    for (auto& d : e->defaults) mangleExpr(d.get());
    mangleExprList(e->compTargets);
    mangleExprList(e->compIters);
    for (auto& cl : e->compIfsNested) mangleExprList(cl);
    mangleExpr(e->a.get());
    mangleExpr(e->b.get());
    mangleExpr(e->c.get());
    mangleExpr(e->d.get());
    mangleExprList(e->items);
}

void mangleNameListString(std::string& s) {
    // Comma-separated identifier list used by `global` / `nonlocal`.
    std::string out, cur;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ',';
        if (c == ',') {
            std::string t = cur;
            std::size_t a = 0, b = t.size();
            while (a < b && std::isspace(static_cast<unsigned char>(t[a]))) a++;
            while (b > a && std::isspace(static_cast<unsigned char>(t[b - 1]))) b--;
            std::string core = t.substr(a, b - a);
            out += std::string(a, ' ') + mangleName(core) + std::string(t.size() - b, ' ');
            out += ',';
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!out.empty()) out.pop_back();
    s = out;
}

void mangleStmtList(std::vector<StmtP>& body);

void mangleStmt(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case SK::FuncDef:
            s->s = mangleName(s->s);
            for (auto& p : s->params) p.name = mangleName(p.name);
            break;
        case SK::ClassDef:
            s->s = mangleName(s->s);
            break;
        case SK::Global:
        case SK::Nonlocal:
            mangleNameListString(s->s);
            break;
        default:
            break;
    }
    mangleExpr(s->a.get());
    mangleExpr(s->b.get());
    mangleExpr(s->c.get());
    mangleExpr(s->iter.get());
    mangleExprList(s->targets);
    mangleExprList(s->values);
    for (auto& p : s->params) mangleExpr(p.def.get());
    for (auto& h : s->handlers) {
        h.name = mangleName(h.name);
        mangleStmtList(h.body);
    }
    mangleStmtList(s->body);
    mangleStmtList(s->orelse);
    mangleStmtList(s->finalbody);
}

void mangleStmtList(std::vector<StmtP>& body) {
    for (auto& s : body) mangleStmt(s.get());
}

}  // namespace

void mangleReservedNames(std::vector<StmtP>& program) { mangleStmtList(program); }
