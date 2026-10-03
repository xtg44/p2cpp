// native.cpp -- direct Python -> C++ backend.
//
// This backend has one job: turn Python into the C++ a person would have
// written, using nothing but the standard library. There is no py:: runtime,
// no support header, and nothing to link beyond libstdc++/libc++.
//
// Two kinds of feedback are produced:
//   * warnings -- the construct is translated, but a Python nuance cannot be
//     reproduced (float repr, dict insertion order, set iteration order, ...).
//   * errors   -- the construct cannot be translated without silently changing
//     behaviour. The tool refuses rather than emit wrong code.
#include "native.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ===========================================================================
// small helpers
// ===========================================================================

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

std::string escapeCpp(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            default: r += c; break;
        }
    }
    return r;
}

std::string blend(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

// Split a template argument list on top-level commas: "A, B<C, D>" -> {A, B<C, D>}
std::vector<std::string> splitTop(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '<' || c == '(' || c == '[') depth++;
        if (c == '>' || c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    for (auto& x : out) {
        size_t a = x.find_first_not_of(" \t");
        size_t b = x.find_last_not_of(" \t");
        x = (a == std::string::npos) ? "" : x.substr(a, b - a + 1);
    }
    return out;
}

std::string tplInner(const std::string& t) {
    size_t lt = t.find('<');
    if (lt == std::string::npos || t.empty() || t.back() != '>') return "";
    return t.substr(lt + 1, t.size() - lt - 2);
}

bool isIntT(const std::string& t) {
    return t == "long long" || t == "int" || t == "short" || t == "unsigned long long" ||
           t == "unsigned";
}
bool isFloatT(const std::string& t) { return t == "double" || t == "float"; }
bool isNumT(const std::string& t) { return isIntT(t) || isFloatT(t); }
bool isStrT(const std::string& t) { return t == "std::string"; }
bool isVecT(const std::string& t) { return startsWith(t, "std::vector<"); }
bool isSetT(const std::string& t) { return startsWith(t, "std::set<"); }
bool isMapT(const std::string& t) { return startsWith(t, "std::map<"); }
bool isPairT(const std::string& t) { return startsWith(t, "std::pair<"); }
bool isTupleT(const std::string& t) { return startsWith(t, "std::tuple<"); }
// Any type whose Python str() is not what `std::cout << v` would print.
bool isContainerT(const std::string& t) {
    return isVecT(t) || isSetT(t) || isMapT(t) || isPairT(t) || isTupleT(t);
}

// A literal `0` / `0.0` on the right of `/`, `//` or `%`. This is the one case
// where Python's ZeroDivisionError can be reproduced exactly: the divisor is
// known at compile time, so the generated C++ can throw instead of returning
// inf (or, for `%`/`//` on integers, invoking undefined behaviour).
bool isZeroConst(const Expr* e) {
    if (!e) return false;
    if (e->kind == EK::IntLit) return e->i == 0;
    if (e->kind == EK::FloatLit) return e->numLit == 0.0;
    return false;
}

// A value whose assignment after a default-initialised declaration is a no-op:
// `0`, `0.0`, `""`, an empty list/set/dict, or `None`. `x = 0` right after
// `long long x{};` does nothing, so the emitter drops it.
bool isDefaultValue(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case EK::IntLit: return e->i == 0;
        case EK::FloatLit: return e->numLit == 0.0;
        case EK::StrLit: return e->s.empty();
        case EK::NoneLit: return true;
        case EK::ListLit:
        case EK::SetLit:
        case EK::DictLit: return e->items.empty();
        default: return false;
    }
}

// Element type as Python sees it: iterating a str yields 1-character strings.
std::string elemOf(const std::string& t) {
    if (isVecT(t) || isSetT(t)) return tplInner(t);
    if (isStrT(t)) return "std::string";
    return "";
}
std::string keyOf(const std::string& t) {
    if (!isMapT(t)) return "";
    auto p = splitTop(tplInner(t));
    return p.size() == 2 ? p[0] : "";
}
std::string valOf(const std::string& t) {
    if (!isMapT(t)) return "";
    auto p = splitTop(tplInner(t));
    return p.size() == 2 ? p[1] : "";
}

// ===========================================================================
// the emitter
// ===========================================================================

struct ForPlan {
    bool ok = false;
    std::string why;
    std::string header;                 // "for (...)" without the brace
    std::vector<std::string> prologue;  // per-iteration bindings, emitted first
};

class Nat {
public:
    Nat(const std::vector<StmtP>& prog, bool comments) : prog(prog), comments(comments) {}

    NativeResult run();

private:
    const std::vector<StmtP>& prog;
    bool comments;

    std::ostringstream body;
    int ind = 0;
    std::set<std::string> inc;
    std::vector<std::string> warnings, errors;

    std::map<std::string, const Stmt*> funcs;
    std::vector<std::string> funcOrder;
    std::map<std::string, std::string> funcRet;  // memo, "" = not yet known
    std::map<std::string, const Stmt*> klasses;
    std::vector<std::string> classOrder;
    // class name -> member name -> member type, resolved once in emitClass so
    // typeOf() can look up `a.x` from any scope (emitFuncs, methods, main).
    std::map<std::string, std::map<std::string, std::string>> classMembers;
    std::set<std::string> modules;
    std::map<std::string, std::vector<std::string>> ctorArgs;
    // Argument types of every call to a user function, collected from the call
    // sites before signatures are emitted. When a parameter is passed the same
    // type at every call site, the signature is specialised to it (instead of
    // leaving it a template), so `bubble_sort(data)` becomes
    // `bubble_sort(std::vector<long long> arr)` rather than a function template.
    // Indexed by position among the ordinary parameters (self/*args/**kwargs
    // are skipped, matching how signature() walks them).
    std::map<std::string, std::vector<std::string>> funcArgTypes;

    std::vector<std::map<std::string, std::string>> scopes;
    bool inFunction = false;
    std::string breakFlag;
    int lambdaSeq = 0;
    // Names that are already bound by the current function's parameter list, so
    // that `a, b = b, a % b` inside a function does not redeclare them.
    std::set<std::string> paramNames;
    // Names that have to be defined where they are assigned (`auto lg = make()`),
    // because their type is a closure: not default-constructible, not assignable.
    std::set<std::string> inPlaceNames, inPlaceDone;
    // For-loop targets that are read after their loop (so they need an explicit
    // declaration). A target absent from this set and read only inside its loop
    // is declared straight in the range-for header instead.
    std::set<std::string> leakedForTargets;
    // Variables that have just been default-initialised by a declaration and
    // have not yet been given a real value. A following `x = 0` / `x = []`
    // assignment is then a no-op (the declaration already zeroes it) and is
    // dropped for readability. Assigning anything else removes the name.
    std::set<std::string> defaultInitNames;

    // Values are printed inline (floats via operator<<, containers via an
    // inline lambda), so the only helpers still emitted are the format-spec
    // ones below (f"{x:b}", :.1%, :, :^10), which std::iomanip cannot express.
    bool fmtBin = false, fmtPct = false, fmtThousands = false, fmtCenter = false;
    bool warnedDictOrder = false;
    // Set when the program uses random.randint/random/uniform/choice: std::rand()
    // without a seed returns the same sequence every run, so main() seeds it.
    bool usesRandom = false;
    // Web-scraping helpers (emitted only when the program needs them).
    bool useFetch = false;    // requests.get / urllib -> curl via popen
    bool useFindall = false;  // re.findall -> std::regex, collect every match
    bool useReSub = false;    // re.sub with a replacement string
    bool useJson = false;     // json.loads / json.dumps -> a minimal parser
    bool useCsv = false;      // csv -> <fstream> rows

    // ---------------------------------------------------------------- output
    void need(const char* h) { inc.insert(h); }
    void line(const std::string& s) {
        for (int i = 0; i < ind; ++i) body << "    ";
        body << s << "\n";
    }
    void blank() { body << "\n"; }
    void err(int ln, const std::string& m) {
        errors.push_back("line " + std::to_string(ln) + ": " + m);
    }
    void warn(int ln, const std::string& m) {
        warnings.push_back("line " + std::to_string(ln) + ": " + m);
    }

    // ---------------------------------------------------------------- scopes
    void push() { scopes.emplace_back(); }
    void pop() { scopes.pop_back(); }
    void bind(const std::string& n, const std::string& t) {
        if (!scopes.empty()) scopes.back()[n] = t;
    }
    std::string typeOfName(const std::string& n) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return f->second;
        }
        return "";
    }
    std::string capture() const { return inFunction ? "[&]" : "[]"; }

    // ------------------------------------------------------------ type rules
    std::string typeOf(const Expr* e);
    std::string callType(const Expr* e);
    // Return type of a user function, read off its `return` statements.
    std::string funcReturnType(const std::string& name);
    // Return type of a class method, read off its `return` statements (member
    // types bound so `return self.x` resolves).
    std::string methodReturnType(const std::string& klass, const std::string& method);
    // The C++ type of a class member, read off its `self.x = ...` in `__init__`.
    std::string memberType(const std::string& klass, const std::string& member);
    // The type of a class attribute (a module-level assignment in the class body).
    std::string staticMemberType(const std::string& klass, const std::string& member);
    std::string scanReturn(const std::vector<StmtP>& b);
    // True when `name` calls itself (directly, or transitively within its own
    // body). A recursive function must not use a deduced `auto` return type.
    bool isRecursive(const std::string& name);
    // The C++ return type spelling: `auto` normally, but a recursive function
    // needs a concrete type (or `void` when it returns nothing).
    std::string returnType(const std::string& name);
    // True when `f` defines a nested function and returns it.
    bool returnsClosure(const std::string& name);
    void emitNestedFunc(const Stmt* f);
    // A nested function's parameter with no annotation is typed std::string when
    // it only ever appears where a string is required.
    bool bodyUsesAsString(const std::vector<StmtP>& b, const std::string& n);
    bool exprUsesAsString(const Expr* e, const std::string& n);
    // True when the body mutates the object named `n` in place (subscript write,
    // an in-place method, or augmented assignment) rather than rebinding it.
    bool bodyMutates(const std::vector<StmtP>& b, const std::string& n);
    bool exprMutates(const Expr* e, const std::string& n);
    // A parameter's Python type: its annotation, else its default value, else
    // std::string when it only ever appears in a string context.
    std::string paramType(const Param& p, const std::vector<StmtP>& body);
    // Like paramType, but falls back to the type agreed on by every call site
    // of the owning function (the same type signature() uses to specialise the
    // parameter), so the body is typed consistently with the specialised
    // signature instead of treating the parameter as an unknown template.
    std::string paramTypeWithCallSite(const std::string& fn, size_t idx, const Param& p,
                                      const std::vector<StmtP>& body);
    std::string compElementType(const Expr* e, bool wantKey);
    std::string pythonToCppType(const std::string& py);

    // ------------------------------------------------------- expression text
    std::string ex(const Expr* e);
    std::string exP(const Expr* e);
    std::string binOp(const Expr* e);
    std::string setBinOp(const Expr* e, const std::string& op);
    std::string compare(const Expr* e);
    std::string call(const Expr* e);
    std::string argList(const Expr* e, size_t from = 0);
    std::string moduleCall(const Expr* e, const std::string& mod, const std::string& fn);
    std::string strMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string listMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string dictMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string setMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string exprMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string stmtMethod(const Expr* e, const std::string& recv, const std::string& m);
    std::string subscript(const Expr* e);
    std::string sliceText(const Expr* e);
    // `.begin()` and `.end()` have to come from the same object, so an operand
    // that is not a plain variable gets bound to a local first.
    void bindSequence(const Expr* arg, const std::string& text, std::string& obj,
                      std::string& prefix, std::string& suffix);
    std::string elementRef(const Expr* baseE, const Expr* idxE, bool mapInsert = false);
    std::string divZeroMessage(const Expr* e);
    void noteDictOrder(int ln);
    std::string listLit(const Expr* e);
    std::string tupleLit(const Expr* e);
    std::string dictLit(const Expr* e);
    std::string fstrValue(const Expr* e);
    std::string outChain(const Expr* e);
    // A value as Python's str() would render it. Plain `operator<<` does not
    // print containers at all, so those become an inline lambda; floats print
    // as `std::cout` prints them (5, not 5.0) -- the difference is accepted.
    std::string reprText(const Expr* e);
    // An inline expression that renders a container as "[1, 2]" / "{1, 2}" /
    // "{'a': 1}" without any emitted helper function.
    std::string containerText(const std::string& t, const std::string& v);
    // The string form of one element (used by containerText, recursively).
    std::string elemAsStr(const std::string& t, const std::string& v);
    // True when evaluating the expression can raise at run time.
    bool exprMayThrow(const Expr* e);
    // True when the expression/statement tree calls into the `random` module
    // (so main() must seed std::rand()).
    bool exprUsesRandom(const Expr* e);
    bool bodyUsesRandom(const std::vector<StmtP>& b);
    // True when `e` is a `re.search(...)` / `re.match(...)` call (so a
    // following `.group(n)` can be translated).
    bool isReSearchCall(const Expr* e);
    std::string reprHelpers();
    // Forward declarations for the helpers above, which are emitted *below*
    // main() so that the translated program stays at the top of the file.
    std::string reprHelperDecls();
    std::string specHelpers();
    std::string scrapeHelpers();
    std::string applySpec(const std::string& value, const std::string& valType,
                          const std::string& spec, int ln);
    std::string compExpr(const Expr* e);
    std::string lambdaExpr(const Expr* e);
    // Text of a receiver that will be used for `.size()` / `.substr()`: a bare
    // string literal is a const char[N] and has no members, so wrap it.
    std::string recvText(const Expr* e);

    // ------------------------------------------------------------- statements
    void emitBody(const std::vector<StmtP>& b);
    void emitStmt(const Stmt* s);
    void emitAssign(const Stmt* s);
    bool emitInputAssign(const Expr* target, const Expr* value);
    void emitAugAssign(const Stmt* s);
    void emitAnnAssign(const Stmt* s);
    void emitPrint(const Expr* call);
    void emitTry(const Stmt* s);
    void emitFor(const Stmt* s);
    void emitWhile(const Stmt* s);
    void emitIf(const Stmt* s);

    struct Bind {
        const Expr* first = nullptr;
        int line = 0;
        std::string ann;
        bool aug = false;
        // For-loop targets: the type depends on the iterable, which may not be
        // known until the declaration is emitted, so keep the pieces.
        const Expr* forIter = nullptr;
        size_t forIdx = 0;
        size_t forCount = 0;
        // True when a for-loop target is read *after* its loop (Python leaves
        // the last element bound). Such a name needs an explicit declaration;
        // one that is never read outside the loop can instead be declared
        // directly in the range-for header (`for (const auto& x : arr)`).
        bool forLeaked = false;
    };
    void collectBinds(const std::vector<StmtP>& b, std::map<std::string, Bind>& out,
                      std::vector<std::string>* order = nullptr);
    std::vector<std::string> targetNames(const Expr* t);
    std::vector<std::string> forTargetTypes(const Expr* iter, size_t count);
    void emitLocalDecls(const std::map<std::string, Bind>& binds,
                        const std::vector<std::string>& order);
    void declareValue(const std::string& name, const Bind& b);
    ForPlan planFor(const Expr* iter, const std::vector<std::string>& names, bool declareTargets,
                    bool inlineTarget = false);
    void markForLeaks(const std::vector<StmtP>& body, std::map<std::string, Bind>& binds);

    // ------------------------------------------------------------- functions
    void emitFuncs();
    void emitClass(const std::string& name, const Stmt* c);
    void collectCtorArgs(const Expr* e, std::map<std::string, std::vector<std::string>>& out);
    void collectCtorArgsStmt(const Stmt* s,
                             std::map<std::string, std::vector<std::string>>& out);
    void collectFuncCallArgs(const Expr* e, std::map<std::string, std::vector<std::string>>& out);
    void collectFuncCallArgsStmt(const Stmt* s,
                                 std::map<std::string, std::vector<std::string>>& out);
    // Iteratively propagate types across functions: the parameter type of a
    // function that returns an object, and the type of an object passed to
    // another function, are both inferred by repeatedly walking the bodies
    // (binding parameters and tracking local variables) until fixpoint.
    void propagateCallTypes(const std::vector<StmtP>& body);
    void propagateCallTypesStmt(const Stmt* s);
    std::string signature(const Stmt* f, const std::string& cxxName, bool withDefaults,
                          std::string& tmpl, const std::string& typeKey = "",
                          bool allowTemplate = true);
};

// ===========================================================================
// types
// ===========================================================================

std::string Nat::pythonToCppType(const std::string& py) {
    if (py == "int") return "long long";
    if (py == "float") return "double";
    if (py == "str") return "std::string";
    if (py == "bool") return "bool";
    if (py == "list") {
        need("vector");
        return "std::vector<long long>";
    }
    if (py == "set") {
        need("set");
        return "std::set<long long>";
    }
    if (py == "dict") {
        need("map");
        return "std::map<std::string, long long>";
    }
    if (py == "tuple") {
        need("tuple");
        return "std::tuple<>";
    }
    if (startsWith(py, "list[")) {
        need("vector");
        return "std::vector<" + pythonToCppType(tplInner(py)) + ">";
    }
    if (startsWith(py, "set[")) {
        need("set");
        return "std::set<" + pythonToCppType(tplInner(py)) + ">";
    }
    if (startsWith(py, "dict[")) {
        need("map");
        auto parts = splitTop(tplInner(py));
        if (parts.size() == 2)
            return "std::map<" + pythonToCppType(parts[0]) + ", " + pythonToCppType(parts[1]) + ">";
    }
    return "";
}

std::string Nat::callType(const Expr* e) {
    const Expr* c = e->a.get();
    if (!c) return "";
    // Method calls: needed so that the type of `t = s.upper()` is known and
    // `t.lower()` can be translated.
    if (c->kind == EK::Attr) {
        const Expr* obj = c->a.get();
        // re.search(...).group(n) yields the captured text (a string).
        if (c->s == "group" && obj && isReSearchCall(obj)) return "std::string";
        if (obj && obj->kind == EK::Name && modules.count(obj->s)) {
            const std::string& m = c->s;
            if (obj->s == "math") {
                if (m == "floor" || m == "ceil" || m == "trunc" || m == "isqrt" ||
                    m == "gcd" || m == "factorial")
                    return "long long";
                if (m == "isnan" || m == "isinf" || m == "isfinite") return "bool";
                return "double";
            }
            if (obj->s == "random") {
                if (m == "randint") return "long long";
                if (m == "random" || m == "uniform") return "double";
                return "";
            }
            if (obj->s == "time") {
                if (m == "time") return "double";
                return "";
            }
            if (obj->s == "requests") {
                if (m == "get") return "std::string";  // the body text
                return "";
            }
            if (obj->s == "re") {
                if (m == "findall") return "std::vector<std::string>";
                if (m == "sub" || m == "search" || m == "match") return "std::string";
                return "";
            }
            if (obj->s == "json") {
                if (m == "loads") return "std::map<std::string, std::string>";
                if (m == "dumps") return "std::string";
                return "";
            }
            return "";
        }
        std::string rt = typeOf(obj);
        const std::string& m = c->s;
        if (isStrT(rt)) {
            need("string");
            if (m == "upper" || m == "lower" || m == "strip" || m == "lstrip" ||
                m == "rstrip" || m == "title" || m == "capitalize" || m == "replace" ||
                m == "join" || m == "zfill")
                return "std::string";
            if (m == "split") {
                need("vector");
                return "std::vector<std::string>";
            }
            if (m == "find" || m == "count") return "long long";
            if (m == "startswith" || m == "endswith" || m == "isdigit" || m == "isalpha" ||
                m == "isspace")
                return "bool";
            return "";
        }
        if (isVecT(rt) || isSetT(rt)) {
            if (m == "index" || m == "count") return "long long";
            if (m == "copy" || m == "union" || m == "intersection" || m == "difference" ||
                m == "symmetric_difference")
                return rt;
            if (m == "pop" && e->items.empty()) return elemOf(rt);
            if (m == "issubset" || m == "issuperset") return "bool";
            return "";
        }
        if (isMapT(rt)) {
            if (m == "get") {
                std::string v = valOf(rt);
                if (!v.empty()) return v;
                if (e->items.size() > 1) return typeOf(e->items[1].get());
                return "";
            }
            if (m == "keys" || m == "values" || m == "items") {
                need("vector");
                if (m == "keys") return "std::vector<" + keyOf(rt) + ">";
                if (m == "values") return "std::vector<" + valOf(rt) + ">";
                need("utility");
                return "std::vector<std::pair<" + keyOf(rt) + ", " + valOf(rt) + ">>";
            }
            if (m == "copy") return rt;
            return "";
        }
        // A method call on a user-class instance: resolve its return type from
        // the class's method body (so `str(p.name())` etc. format correctly).
        if (!rt.empty() && klasses.count(rt))
            return methodReturnType(rt, m);
        return "";
    }
    if (c->kind != EK::Name) return "";
    const std::string& n = c->s;
    auto argT = [&](size_t i) -> std::string {
        if (i >= e->items.size()) return "";
        return typeOf(e->items[i].get());
    };

    if (n == "len" || n == "int" || n == "ord") return "long long";
    if (n == "round") return e->items.size() >= 2 ? "double" : "long long";
    if (n == "float" || n == "pow") return "double";
    if (n == "str" || n == "repr" || n == "input" || n == "format" || n == "chr") {
        need("string");
        return "std::string";
    }
    if (n == "bool") return "bool";
    if (n == "abs") {
        std::string t = argT(0);
        return isNumT(t) ? t : "";
    }
    if (n == "sum") {
        std::string t = argT(0);
        if (isNumT(t)) return t;
        std::string el = elemOf(t);
        return isNumT(el) ? el : "";
    }
    if (n == "min" || n == "max") {
        if (e->items.size() == 1) return elemOf(argT(0));
        std::string a = argT(0), b = argT(1);
        if (isNumT(a) && isNumT(b)) return (isIntT(a) && isIntT(b)) ? a : "double";
        return (a == b) ? a : "";
    }
    if (n == "sorted") {
        std::string t = argT(0);
        if (isVecT(t)) return t;
        if (isSetT(t)) {
            need("vector");
            return "std::vector<" + elemOf(t) + ">";
        }
        return "";
    }
    if (n == "reversed") {
        std::string t = argT(0);
        if (isVecT(t)) return t;
        if (isStrT(t)) return "std::string";
        return "";
    }
    if (n == "list") {
        need("vector");
        if (e->items.empty()) return "std::vector<long long>";
        const Expr* a0 = e->items[0].get();
        if (a0->kind == EK::Call && a0->a && a0->a->kind == EK::Name && a0->a->s == "range")
            return "std::vector<long long>";
        std::string t = argT(0);
        if (isStrT(t)) return "std::vector<std::string>";
        if (isVecT(t) || isSetT(t)) return "std::vector<" + elemOf(t) + ">";
        return "";
    }
    if (n == "map" || n == "filter") {
        std::string el = elemOf(argT(1));
        if (el.empty()) return "";
        need("vector");
        return "std::vector<" + el + ">";
    }
    if (klasses.count(n)) return n;
    if (funcs.count(n)) return funcReturnType(n);
    return "";
}

// The Python type of what a user function returns, taken from its `return`
// statements. The C++ side is a template, so the parameter types are unknown;
// binding them to their annotations still resolves the common cases
// (`return a / b` is always a float in Python).
std::string Nat::funcReturnType(const std::string& name) {
    auto memo = funcRet.find(name);
    if (memo != funcRet.end()) return memo->second;
    auto f = funcs.find(name);
    if (f == funcs.end()) return "";
    funcRet[name] = "";  // also makes a recursive call terminate
    push();
    size_t ti = 0;  // template-parameter counter (for the T<n> fallback)
    size_t pi = 0;  // ordinary-parameter index, matching funcArgTypes[]
    for (const auto& p : f->second->params) {
        if (p.name == "self" || p.isKwStar) continue;
        if (p.isStar) {
            // `*args` maps to a parameter pack; its element type is unknown, so
            // leave it untyped (a `return` of it cannot be resolved here).
            ti++;
            continue;
        }
        if (!p.annotation.empty()) {
            bind(p.name, pythonToCppType(p.annotation));
        } else if (p.def) {
            bind(p.name, typeOf(p.def.get()));
        } else {
            // No annotation/default. Prefer the concrete type every call site
            // agreed on (which signature() also used to specialise the
            // parameter); only fall back to T<n> when that is unavailable.
            std::string concrete;
            auto it = funcArgTypes.find(name);
            if (it != funcArgTypes.end() && pi < it->second.size()) {
                const std::string& t = it->second[pi];
                if (!t.empty() && t != "\x01") concrete = t;
            }
            bind(p.name, concrete.empty() ? "T" + std::to_string(ti) : concrete);
        }
        ti++;
        pi++;
    }
    std::string r = scanReturn(f->second->body);
    // A `return result` whose type is a local variable (e.g. `result = []` then
    // `return result`) needs the locals' types too. Resolve them from the body's
    // assignments, then retry.
    if (r.empty()) {
        std::map<std::string, Bind> binds;
        std::vector<std::string> border;
        collectBinds(f->second->body, binds, &border);
        for (const auto& n : border) {
            auto it = binds.find(n);
            if (it == binds.end()) continue;
            std::string t;
            if (!it->second.ann.empty()) t = pythonToCppType(it->second.ann);
            else if (it->second.first) t = typeOf(it->second.first);
            else if (it->second.forIter) {
                auto ts = forTargetTypes(it->second.forIter, it->second.forCount);
                if (it->second.forIdx < ts.size()) t = ts[it->second.forIdx];
            }
            if (!t.empty()) bind(n, t);
        }
        r = scanReturn(f->second->body);
    }
    pop();
    funcRet[name] = r;
    return r;
}

// The C++ type of a class member, read off its `self.x = ...` in `__init__`.
// Prefer the already-resolved table (filled in emitClass, which runs before
// emitFuncs and main); fall back to reading `__init__` directly for callers
// that run before the class is emitted (e.g. methodReturnType's own scan).
std::string Nat::memberType(const std::string& klass, const std::string& member) {
    auto pub = classMembers.find(klass);
    if (pub != classMembers.end()) {
        auto it = pub->second.find(member);
        if (it != pub->second.end()) return it->second;
    }
    auto k = klasses.find(klass);
    if (k == klasses.end()) return "";
    for (const auto& m : k->second->body) {
        if (m->kind != SK::FuncDef || m->s != "__init__") continue;
        for (const auto& st : m->body) {
            const Expr* tgt = st->kind == SK::Assign && st->targets.size() == 1
                                  ? st->targets[0].get()
                                  : (st->kind == SK::AnnAssign ? st->a.get() : nullptr);
            if (!tgt || tgt->kind != EK::Attr || !tgt->a || tgt->a->kind != EK::Name ||
                tgt->a->s != "self" || tgt->s != member)
                continue;
            if (st->kind == SK::AnnAssign && st->b) {
                std::string a = pythonToCppType(ex(st->b.get()));
                if (!a.empty()) return a;
            }
            const Expr* val = st->kind == SK::Assign ? (st->values.empty()
                                                            ? nullptr
                                                            : st->values[0].get())
                                                     : st->c.get();
            std::string t = typeOf(val);
            if (!t.empty()) return t;
        }
    }
    return "";
}

// The type of a class attribute, read off a module-level assignment in the
// class body (`version = 3`) or an annotated assignment (`name: str = "x"`).
std::string Nat::staticMemberType(const std::string& klass, const std::string& member) {
    auto k = klasses.find(klass);
    if (k == klasses.end()) return "";
    for (const auto& m : k->second->body) {
        if (m->kind == SK::Assign && m->targets.size() == 1 &&
            m->targets[0]->kind == EK::Name && m->targets[0]->s == member &&
            !m->values.empty()) {
            std::string t = typeOf(m->values[0].get());
            if (!t.empty()) return t;
        } else if (m->kind == SK::AnnAssign && m->a && m->a->kind == EK::Name &&
                   m->a->s == member && m->b) {
            std::string t = pythonToCppType(ex(m->b.get()));
            if (!t.empty()) return t;
        }
    }
    return "";
}

// Return type of a class method. `self.x` must resolve, so the class's member
// types are first collected from its `__init__` (self.x = ...) and bound, then
// the method's own parameters, before scanning its `return` statements.
std::string Nat::methodReturnType(const std::string& klass, const std::string& method) {
    std::string key = klass + "." + method;
    auto memo = funcRet.find(key);
    if (memo != funcRet.end()) return memo->second;
    auto k = klasses.find(klass);
    if (k == klasses.end()) return "";
    const Stmt* cls = k->second;
    const Stmt* meth = nullptr;
    const Stmt* init = nullptr;
    for (const auto& m : cls->body) {
        if (m->kind != SK::FuncDef) continue;
        if (m->s == method) meth = m.get();
        if (m->s == "__init__") init = m.get();
    }
    if (!meth) return "";
    funcRet[key] = "";  // terminate any recursion
    push();
    // Bind member types (self.x = ...). Prefer the table already resolved by
    // emitClass; fall back to reading __init__ if it hasn't run yet.
    bind("self", klass);
    auto pub = classMembers.find(klass);
    if (pub != classMembers.end()) {
        for (const auto& kv : pub->second) bind("self." + kv.first, kv.second);
    } else if (init) {
        for (const auto& st : init->body) {
            const Expr* tgt = st->kind == SK::Assign && st->targets.size() == 1
                                  ? st->targets[0].get()
                                  : (st->kind == SK::AnnAssign ? st->a.get() : nullptr);
            if (!tgt || tgt->kind != EK::Attr || !tgt->a || tgt->a->kind != EK::Name ||
                tgt->a->s != "self")
                continue;
            const Expr* val = st->kind == SK::Assign ? (st->values.empty() ? nullptr
                                                                           : st->values[0].get())
                                                     : st->c.get();
            std::string t = typeOf(val);
            if (!t.empty()) bind("self." + tgt->s, t);
        }
    }
    // Bind the method's parameters.
    size_t pi = 0;
    for (const auto& p : meth->params) {
        if (p.name == "self" || p.isKwStar) continue;
        if (p.isStar) {
            pi++;
            continue;
        }
        if (!p.annotation.empty()) bind(p.name, pythonToCppType(p.annotation));
        else if (p.def) bind(p.name, typeOf(p.def.get()));
        pi++;
    }
    std::string r = scanReturn(meth->body);
    pop();
    funcRet[key] = r;
    return r;
}

std::string Nat::scanReturn(const std::vector<StmtP>& b) {
    for (const auto& s : b) {
        if (!s) continue;
        if (s->kind == SK::Return && s->a) {
            std::string t = typeOf(s->a.get());
            if (!t.empty()) return t;
        }
        for (const std::vector<StmtP>* nest : {&s->body, &s->orelse, &s->finalbody}) {
            std::string t = scanReturn(*nest);
            if (!t.empty()) return t;
        }
        for (const auto& h : s->handlers) {
            std::string t = scanReturn(h.body);
            if (!t.empty()) return t;
        }
    }
    return "";
}

// `def outer(): def inner(): ... ; return inner` builds a closure. Its type has
// neither a default constructor nor an assignment operator, so the variable
// that receives it must be defined at the point of the call.
bool Nat::returnsClosure(const std::string& name) {
    auto f = funcs.find(name);
    if (f == funcs.end()) return false;
    std::set<std::string> nested;
    auto scan = [&](auto&& self, const std::vector<StmtP>& b) -> void {
        for (const auto& s : b) {
            if (!s) continue;
            if (s->kind == SK::FuncDef) nested.insert(s->s);
            self(self, s->body);
            self(self, s->orelse);
            self(self, s->finalbody);
        }
    };
    scan(scan, f->second->body);
    if (nested.empty()) return false;
    for (const auto& s : f->second->body)
        if (s && s->kind == SK::Return && s->a && s->a->kind == EK::Name &&
            nested.count(s->a->s))
            return true;
    return false;
}

// A function that can (directly or indirectly) call itself — i.e. it lies on a
// cycle in the call graph — is recursive. C++ cannot deduce the return type of
// such a function (`auto f()` is ill-formed when `f` recurs), so it needs an
// explicit return type. Mutual recursion (`a -> b -> a`) counts too.
bool Nat::isRecursive(const std::string& name) {
    if (!funcs.count(name)) return false;

    // Build a call graph: callees[name] = set of user functions `name` calls.
    std::map<std::string, std::set<std::string>> callees;
    for (const auto& [fn, stmt] : funcs) {
        std::set<std::string> called;
        auto scanExpr = [&](auto&& self, const Expr* e) -> void {
            if (!e) return;
            if (e->kind == EK::Call && e->a && e->a->kind == EK::Name &&
                funcs.count(e->a->s))
                called.insert(e->a->s);
            self(self, e->a.get());
            self(self, e->b.get());
            self(self, e->c.get());
            self(self, e->d.get());
            for (const auto& it : e->items) self(self, it.get());
            for (const auto& it : e->compIters) self(self, it.get());
            for (const auto& it : e->compTargets) self(self, it.get());
            for (const auto& p : e->kwargs) self(self, p.second.get());
            for (const auto& p : e->parts)
                if (p.isExpr) self(self, p.expr.get());
        };
        auto scanStmt = [&](auto&& self, const std::vector<StmtP>& b) -> void {
            for (const auto& s : b) {
                if (!s) continue;
                scanExpr(scanExpr, s->a.get());
                scanExpr(scanExpr, s->b.get());
                scanExpr(scanExpr, s->c.get());
                scanExpr(scanExpr, s->iter.get());
                for (const auto& t : s->targets) scanExpr(scanExpr, t.get());
                for (const auto& v : s->values) scanExpr(scanExpr, v.get());
                self(self, s->body);
                self(self, s->orelse);
                self(self, s->finalbody);
                for (const auto& h : s->handlers) self(self, h.body);
            }
        };
        scanStmt(scanStmt, stmt->body);
        callees[fn] = std::move(called);
    }

    // Depth-first search from `name`: does any path lead back to `name`?
    std::set<std::string> visiting;
    bool back = false;
    auto dfs = [&](auto&& self, const std::string& cur) -> void {
        visiting.insert(cur);
        auto it = callees.find(cur);
        if (it != callees.end())
            for (const auto& nxt : it->second) {
                if (nxt == name) { back = true; return; }  // back edge to self
                if (!visiting.count(nxt)) self(self, nxt);
                if (back) return;
            }
        visiting.erase(cur);
    };
    dfs(dfs, name);
    return back;
}

std::string Nat::returnType(const std::string& name) {
    // A non-recursive function can leave its return type to `auto`.
    if (!isRecursive(name)) return "auto";
    // Recursive: the type must be spelled out. Read it from the return
    // statements; a function with no `return` returns nothing, i.e. void.
    std::string t = funcReturnType(name);
    return t.empty() ? "void" : t;
}

bool Nat::exprUsesAsString(const Expr* e, const std::string& n) {
    if (!e) return false;
    if (e->kind == EK::FStr)
        for (const auto& p : e->parts)
            if (p.isExpr && p.expr && p.expr->kind == EK::Name && p.expr->s == n) return true;
    if (e->kind == EK::BinOp && e->s == "+") {
        bool ls = e->a && e->a->kind == EK::StrLit, rs = e->b && e->b->kind == EK::StrLit;
        if (ls && e->b && e->b->kind == EK::Name && e->b->s == n) return true;
        if (rs && e->a && e->a->kind == EK::Name && e->a->s == n) return true;
    }
    for (const Expr* c : {e->a.get(), e->b.get(), e->c.get(), e->d.get()})
        if (exprUsesAsString(c, n)) return true;
    for (const auto& it : e->items)
        if (exprUsesAsString(it.get(), n)) return true;
    for (const auto& p : e->parts)
        if (exprUsesAsString(p.expr.get(), n)) return true;
    for (const auto& kv : e->kwargs)
        if (exprUsesAsString(kv.second.get(), n)) return true;
    for (const auto& it : e->compIters)
        if (exprUsesAsString(it.get(), n)) return true;
    for (const auto& it : e->compTargets)
        if (exprUsesAsString(it.get(), n)) return true;
    for (const auto& v : e->compIfsNested)
        for (const auto& it : v)
            if (exprUsesAsString(it.get(), n)) return true;
    return false;
}

// Can evaluating this expression raise at run time? Python evaluates every
// print() argument before writing anything, so when one of them can raise the
// arguments have to be evaluated into temporaries first: a plain `<<` chain
// writes the earlier ones as it goes and would leave partial output behind.
bool Nat::exprMayThrow(const Expr* e) {
    if (!e) return false;
    if (e->kind == EK::Subscript) return true;  // reads go through .at()
    if (e->kind == EK::BinOp && !divZeroMessage(e).empty()) return true;
    if (e->kind == EK::Call) {
        const Expr* c = e->a.get();
        if (c && c->kind == EK::Name) {
            const std::string& f = c->s;
            if (funcs.count(f) || klasses.count(f))
                return true;  // a call into user code may raise anything
        } else if (c && c->kind == EK::Attr && (c->s == "index" || c->s == "pop" ||
                                                c->s == "remove")) {
            return true;
        }
    }
    for (const Expr* c : {e->a.get(), e->b.get(), e->c.get(), e->d.get()})
        if (exprMayThrow(c)) return true;
    for (const auto& it : e->items)
        if (exprMayThrow(it.get())) return true;
    for (const auto& p : e->parts)
        if (p.isExpr && exprMayThrow(p.expr.get())) return true;
    for (const auto& kv : e->kwargs)
        if (exprMayThrow(kv.second.get())) return true;
    return false;
}

bool Nat::bodyUsesAsString(const std::vector<StmtP>& b, const std::string& n) {
    for (const auto& s : b) {
        if (!s) continue;
        if (exprUsesAsString(s->a.get(), n) || exprUsesAsString(s->b.get(), n) ||
            exprUsesAsString(s->c.get(), n) || exprUsesAsString(s->iter.get(), n))
            return true;
        for (const auto& t : s->targets)
            if (exprUsesAsString(t.get(), n)) return true;
        for (const auto& v : s->values)
            if (exprUsesAsString(v.get(), n)) return true;
        if (bodyUsesAsString(s->body, n) || bodyUsesAsString(s->orelse, n) ||
            bodyUsesAsString(s->finalbody, n))
            return true;
        for (const auto& h : s->handlers)
            if (bodyUsesAsString(h.body, n)) return true;
    }
    return false;
}

bool Nat::exprUsesRandom(const Expr* e) {
    if (!e) return false;
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Attr) {
        const Expr* obj = e->a->a.get();
        if (obj && obj->kind == EK::Name && obj->s == "random") return true;
    }
    for (const Expr* c : {e->a.get(), e->b.get(), e->c.get(), e->d.get()})
        if (exprUsesRandom(c)) return true;
    for (const auto& it : e->items)
        if (exprUsesRandom(it.get())) return true;
    for (const auto& p : e->parts)
        if (p.isExpr && exprUsesRandom(p.expr.get())) return true;
    for (const auto& kv : e->kwargs)
        if (exprUsesRandom(kv.second.get())) return true;
    return false;
}

bool Nat::bodyUsesRandom(const std::vector<StmtP>& b) {
    for (const auto& s : b) {
        if (!s) continue;
        if (exprUsesRandom(s->a.get()) || exprUsesRandom(s->b.get()) ||
            exprUsesRandom(s->c.get()) || exprUsesRandom(s->iter.get()))
            return true;
        for (const auto& t : s->targets)
            if (exprUsesRandom(t.get())) return true;
        for (const auto& v : s->values)
            if (exprUsesRandom(v.get())) return true;
        if (bodyUsesRandom(s->body) || bodyUsesRandom(s->orelse) ||
            bodyUsesRandom(s->finalbody))
            return true;
        for (const auto& h : s->handlers)
            if (bodyUsesRandom(h.body)) return true;
    }
    return false;
}

bool Nat::isReSearchCall(const Expr* e) {
    if (!e || e->kind != EK::Call || !e->a || e->a->kind != EK::Attr) return false;
    const Expr* obj = e->a->a.get();
    if (!obj || obj->kind != EK::Name || obj->s != "re") return false;
    return e->a->s == "search" || e->a->s == "match";
}

// The in-place mutators: calling any of these on a container changes it, which
// in Python is visible to the caller. A parameter used this way must be passed
// by reference, or the mutation is thrown away with the by-value copy.
static const std::set<std::string>& inPlaceMethods() {
    static const std::set<std::string> m = {
        "append", "extend", "insert", "pop", "remove", "clear", "sort", "reverse",
        "add", "discard", "update",
    };
    return m;
}

// Is `n` used as the receiver of an in-place method, the target of a subscript
// write, or the left side of an augmented assignment -- all of which mutate the
// object `n` names rather than rebind it?
bool Nat::exprMutates(const Expr* e, const std::string& n) {
    if (!e) return false;
    if (e->kind == EK::Attr && e->a && e->a->kind == EK::Name && e->a->s == n &&
        inPlaceMethods().count(e->s))
        return true;
    // n[i] = v  (assignment target) is handled at the statement level; here we
    // only catch n used inside a nested expression that itself mutates.
    for (const Expr* c : {e->a.get(), e->b.get(), e->c.get(), e->d.get()})
        if (exprMutates(c, n)) return true;
    for (const auto& it : e->items)
        if (exprMutates(it.get(), n)) return true;
    for (const auto& p : e->parts)
        if (p.isExpr && exprMutates(p.expr.get(), n)) return true;
    for (const auto& kv : e->kwargs)
        if (exprMutates(kv.second.get(), n)) return true;
    return false;
}

// A subscript write target `n[...]` mutates the container named `n`. This also
// recurses into `n[i], n[j] = ...`, where the target is a tuple of subscripts.
static bool subscriptBaseIs(const Expr* t, const std::string& n) {
    if (!t) return false;
    if (t->kind == EK::Subscript)
        return t->a && t->a->kind == EK::Name && t->a->s == n;
    if (t->kind == EK::TupleLit || t->kind == EK::ListLit) {
        for (const auto& it : t->items)
            if (subscriptBaseIs(it.get(), n)) return true;
        return false;
    }
    if (t->kind == EK::Starred) return subscriptBaseIs(t->a.get(), n);
    return false;
}

bool Nat::bodyMutates(const std::vector<StmtP>& b, const std::string& n) {
    for (const auto& s : b) {
        if (!s) continue;
        if (s->kind == SK::Assign)
            for (const auto& t : s->targets)
                if (subscriptBaseIs(t.get(), n)) return true;
        if (s->kind == SK::AugAssign && s->a && s->a->kind == EK::Name && s->a->s == n)
            return true;
        if (exprMutates(s->a.get(), n) || exprMutates(s->b.get(), n) ||
            exprMutates(s->c.get(), n) || exprMutates(s->iter.get(), n))
            return true;
        for (const auto& v : s->values)
            if (exprMutates(v.get(), n)) return true;
        if (bodyMutates(s->body, n) || bodyMutates(s->orelse, n) ||
            bodyMutates(s->finalbody, n))
            return true;
        for (const auto& h : s->handlers)
            if (bodyMutates(h.body, n)) return true;
    }
    return false;
}

std::string Nat::paramType(const Param& p, const std::vector<StmtP>& body) {
    std::string r;
    if (!p.annotation.empty()) r = pythonToCppType(p.annotation);
    else if (p.def) {
        r = typeOf(p.def.get());
        if (r.empty()) r.clear();
    }
    if (r.empty() && bodyUsesAsString(body, p.name)) r = "std::string";
    if (r.find("std::string") != std::string::npos) need("string");
    return r;
}

std::string Nat::paramTypeWithCallSite(const std::string& fn, size_t idx, const Param& p,
                                       const std::vector<StmtP>& body) {
    std::string r = paramType(p, body);
    if (!r.empty()) return r;
    // No annotation/default and not string-only: look at the call sites. When
    // they all agree on one type, use it; otherwise the body keeps treating the
    // parameter as an unknown template (decltype fallbacks still apply).
    auto it = funcArgTypes.find(fn);
    if (it != funcArgTypes.end() && idx < it->second.size()) {
        const std::string& t = it->second[idx];
        if (!t.empty() && t != "\x01") return t;
    }
    return r;
}

// `def inner(...)` inside another function becomes a lambda that captures the
// enclosing locals by reference, which is what a Python closure does.
void Nat::emitNestedFunc(const Stmt* f) {
    std::vector<std::string> ps;
    for (const auto& p : f->params) {
        if (p.name == "self") continue;
        if (p.isStar || p.isKwStar) {
            err(f->line, "`*" + p.name + "` in a nested function is not translated in native mode");
            continue;
        }
        std::string t = paramType(p, f->body);
        if (t.empty()) {
            err(f->line,
                "native mode needs a type for the parameter '" + p.name + "' of the nested "
                "function '" + f->s + "'; annotate it, e.g. `" + p.name + ": str`");
            t = "long long";
        }
        ps.push_back(t + " " + p.name +
                     (p.def ? " = " + ex(p.def.get()) : std::string()));
    }
    line("auto " + f->s + " = " + capture() + "(" + blend(ps, ", ") + ") {");
    ind++;
    push();
    paramNames.clear();
    for (const auto& p : f->params) {
        if (p.name == "self" || p.isKwStar) continue;
        bind(p.name, paramType(p, f->body));
        paramNames.insert(p.name);
    }
    std::map<std::string, Bind> binds;
    std::vector<std::string> border;
    collectBinds(f->body, binds, &border);
    markForLeaks(f->body, binds);
    emitLocalDecls(binds, border);
    emitBody(f->body);
    pop();
    ind--;
    line("};");
    bind(f->s, "");
}

std::string Nat::compElementType(const Expr* e, bool wantKey) {
    push();
    for (size_t i = 0; i < e->compTargets.size(); ++i) {
        std::vector<std::string> names = targetNames(e->compTargets[i].get());
        std::vector<std::string> ts = forTargetTypes(e->compIters[i].get(), names.size());
        for (size_t k = 0; k < names.size() && k < ts.size(); ++k) bind(names[k], ts[k]);
    }
    std::string t = typeOf(wantKey ? e->b.get() : e->a.get());
    pop();
    return t;
}

std::string Nat::typeOf(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case EK::IntLit: return "long long";
        case EK::FloatLit: return "double";
        case EK::StrLit: return "std::string";
        case EK::BoolLit: return "bool";
        case EK::FStr: return "std::string";
        case EK::Name: return typeOfName(e->s);
        case EK::ListLit: {
            need("vector");
            std::string el = "long long";
            for (const auto& it : e->items) {
                std::string t = typeOf(it.get());
                if (!t.empty()) {
                    el = t;
                    break;
                }
            }
            return "std::vector<" + el + ">";
        }
        case EK::SetLit: {
            need("set");
            std::string el = "long long";
            for (const auto& it : e->items) {
                std::string t = typeOf(it.get());
                if (!t.empty()) {
                    el = t;
                    break;
                }
            }
            return "std::set<" + el + ">";
        }
        case EK::DictLit: {
            need("map");
            std::string k = "std::string", v = "long long";
            if (e->items.size() >= 2) {
                std::string a = typeOf(e->items[0].get()), b = typeOf(e->items[1].get());
                if (!a.empty()) k = a;
                if (!b.empty()) v = b;
            }
            return "std::map<" + k + ", " + v + ">";
        }
        case EK::Call: return callType(e);
        case EK::Slice: return typeOf(e->a.get());
        case EK::TupleLit: {
            need("tuple");
            std::vector<std::string> parts;
            for (const auto& it : e->items) {
                std::string t = typeOf(it.get());
                parts.push_back(t.empty() ? "long long" : t);
            }
            return "std::tuple<" + blend(parts, ", ") + ">";
        }
        case EK::Subscript: {
            std::string bt = typeOf(e->a.get());
            if (isVecT(bt) || isSetT(bt)) return elemOf(bt);
            if (isStrT(bt)) return "std::string";
            if (isMapT(bt)) return valOf(bt);
            return "";
        }
        case EK::BinOp: {
            const std::string& op = e->s;
            std::string a = typeOf(e->a.get()), b = typeOf(e->b.get());
            if (op == "+" || op == "*") {
                if (isStrT(a) || isStrT(b)) return "std::string";
                if (isVecT(a)) return a;
                if (isIntT(a) && isIntT(b)) return "long long";
                if (isNumT(a) || isNumT(b)) return "double";
                return "";
            }
            if (op == "|" || op == "&" || op == "^" || op == "-") {
                if (isSetT(a)) return a;
                if (isSetT(b)) return b;
            }
            if (op == "/") return "double";
            if (op == "//") return (isIntT(a) && isIntT(b)) ? "long long" : "double";
            if (op == "%" || op == "**") return (isIntT(a) && isIntT(b)) ? "long long" : "double";
            if (isIntT(a) && isIntT(b)) return "long long";
            if (isNumT(a) || isNumT(b)) return "double";
            return "";
        }
        case EK::UnaryOp:
            if (e->s == "not") return "bool";
            return typeOf(e->a.get());
        case EK::BoolOp:
        case EK::Compare: return "bool";
        case EK::Attr: {
            const Expr* obj = e->a.get();
            if (obj && obj->kind == EK::Name && modules.count(obj->s)) {
                if (obj->s == "math" || obj->s == "cmath") {
                    const std::string& m = e->s;
                    if (m == "pi" || m == "e" || m == "tau" || m == "inf" || m == "nan")
                        return "double";
                }
            }
            // `self.x`: the member type bound while inferring a method's return.
            if (obj && obj->kind == EK::Name && obj->s == "self") {
                std::string t = typeOfName("self." + e->s);
                if (!t.empty()) return t;
            }
            // `obj.x` where obj is a user-class instance: its member type.
            if (obj) {
                std::string rt = typeOf(obj);
                if (!rt.empty() && klasses.count(rt)) {
                    std::string t = memberType(rt, e->s);
                    if (!t.empty()) return t;
                }
            }
            // `Config.version`: a class attribute.
            if (obj && obj->kind == EK::Name && klasses.count(obj->s)) {
                std::string t = staticMemberType(obj->s, e->s);
                if (!t.empty()) return t;
            }
            return "";
        }
        case EK::IfExp: {
            std::string a = typeOf(e->a.get()), c = typeOf(e->c.get());
            if (a == c) return a;
            if (a.empty()) return c;
            if (c.empty()) return a;
            return "";
        }
        case EK::ListComp: {
            need("vector");
            std::string el = compElementType(e, false);
            return el.empty() ? "" : "std::vector<" + el + ">";
        }
        case EK::SetComp: {
            need("set");
            std::string el = compElementType(e, false);
            return el.empty() ? "" : "std::set<" + el + ">";
        }
        case EK::DictComp: {
            need("map");
            std::string k = compElementType(e, true);
            std::string v = compElementType(e, false);
            if (k.empty() || v.empty()) return "";
            return "std::map<" + k + ", " + v + ">";
        }
        default: return "";
    }
}

// ===========================================================================
// expressions
// ===========================================================================

std::string Nat::exP(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case EK::IntLit:
        case EK::FloatLit:
        case EK::BoolLit:
        case EK::StrLit:
        case EK::Name:
        case EK::Call:
        case EK::Subscript:
        case EK::Attr:
        case EK::ListLit:
        case EK::SetLit:
        case EK::DictLit:
        case EK::TupleLit: return ex(e);
        default: return "(" + ex(e) + ")";
    }
}

std::string Nat::listLit(const Expr* e) {
    need("vector");
    std::string t = typeOf(e);
    if (t.empty()) t = "std::vector<long long>";
    if (e->items.empty()) return t + "{}";
    std::vector<std::string> parts;
    for (const auto& it : e->items) parts.push_back(ex(it.get()));
    return t + "{" + blend(parts, ", ") + "}";
}

std::string Nat::tupleLit(const Expr* e) {
    need("tuple");
    std::vector<std::string> parts;
    for (const auto& it : e->items) parts.push_back(ex(it.get()));
    return "std::make_tuple(" + blend(parts, ", ") + ")";
}

std::string Nat::dictLit(const Expr* e) {
    need("map");
    std::string t = typeOf(e);
    if (t.empty()) t = "std::map<std::string, long long>";
    if (e->items.size() < 2) return t + "{}";
    std::vector<std::string> parts;
    for (size_t i = 0; i + 1 < e->items.size(); i += 2)
        parts.push_back("{" + ex(e->items[i].get()) + ", " + ex(e->items[i + 1].get()) + "}");
    return t + "{" + blend(parts, ", ") + "}";
}

// The exact ZeroDivisionError text CPython prints for `a <op> 0`, or "" when
// this expression is not a division by a literal zero.
std::string Nat::divZeroMessage(const Expr* e) {
    if (!e || e->kind != EK::BinOp || !isZeroConst(e->b.get())) return "";
    std::string ta = typeOf(e->a.get());
    bool whole = (ta.empty() || isIntT(ta)) && isIntT(typeOf(e->b.get()));
    if (e->s == "/") return whole ? "division by zero" : "float division by zero";
    if (e->s == "//")
        return whole ? "integer division or modulo by zero" : "float floor division by zero";
    if (e->s == "%") return whole ? "integer modulo by zero" : "float modulo by zero";
    return "";
}

// `xs[i]`, `s[i]` or `d[k]` with Python's rules: negative indices count from the
// end, and an out-of-range index raises instead of reading past the end of the
// buffer. `.at()` gives exactly that, so it is used for every read. `mapInsert`
// is for `d[k] = v`, where Python creates the key rather than raising.
std::string Nat::elementRef(const Expr* baseE, const Expr* idxE, bool mapInsert) {
    std::string bt = typeOf(baseE);
    std::string base = recvText(baseE);
    if (!idxE) return base + ".at(0)";
    std::string it = exP(idxE);
    long long negConst = 0;
    bool isNeg = false;
    if (idxE->kind == EK::IntLit && idxE->i < 0) {
        negConst = -idxE->i;
        isNeg = true;
    } else if (idxE->kind == EK::UnaryOp && idxE->s == "-" && idxE->a &&
               idxE->a->kind == EK::IntLit) {
        negConst = idxE->a->i;
        isNeg = true;
    }
    if (isNeg) it = base + ".size() - " + std::to_string(negConst);
    if (isMapT(bt)) return base + (mapInsert ? "[" : ".at(") + it + (mapInsert ? "]" : ")");
    if (isStrT(bt) || isVecT(bt)) return base + ".at(" + it + ")";
    return base + "[" + it + "]";
}

std::string Nat::subscript(const Expr* e) {
    std::string bt = typeOf(e->a.get());
    std::string acc = elementRef(e->a.get(), e->b.get());
    if (isStrT(bt)) {
        need("string");
        return "std::string(1, " + acc + ")";
    }
    return acc;
}

// `.begin()` and `.end()` must come from the same object: for anything that is
// not a plain variable the operand is materialised into a local first, because
// two temporaries would be two different containers.
void Nat::bindSequence(const Expr* arg, const std::string& text, std::string& obj,
                       std::string& prefix, std::string& suffix) {
    if (!arg || arg->kind == EK::Name || arg->kind == EK::Attr || arg->kind == EK::Subscript) {
        obj = text;
        return;
    }
    obj = "__s";
    prefix = capture() + "() { auto __s = " + text + "; return ";
    suffix = "; }()";
}

// A dict's type is std::map, which is what C++ has; the price is that iteration
// and printing follow key order where Python follows insertion order. Say so
// once, rather than letting the difference show up unexplained in the output.
void Nat::noteDictOrder(int ln) {
    if (warnedDictOrder) return;
    warnedDictOrder = true;
    warn(ln,
         "a dict is a std::map here, so it iterates and prints in key order, not the "
         "insertion order Python uses");
}

// `xs[a:b]`, `s[a:b]` and `xs[::-1]` -- the part of slicing that maps onto C++
// iterators. A negative constant bound counts from the end, as in Python.
std::string Nat::sliceText(const Expr* e) {
    const Expr* base = e->a.get();
    std::string bt = typeOf(base);
    if (!isVecT(bt) && !isStrT(bt)) {
        err(e->line, "native mode only translates a slice of a list or a str");
        return "{}";
    }
    if (base->kind == EK::Call) {
        err(e->line,
            "native mode cannot slice the result of a call (the slice would evaluate it "
            "twice); assign it to a variable first");
        return "{}";
    }
    std::string b = recvText(base);
    auto bound = [&](const Expr* x, bool isHi) -> std::string {
        if (!x) return isHi ? "static_cast<long long>(" + b + ".size())" : "0";
        if (x->kind == EK::IntLit && x->i < 0)
            return "static_cast<long long>(" + b + ".size()) - " + std::to_string(-x->i);
        if (x->kind == EK::UnaryOp && x->s == "-" && x->a && x->a->kind == EK::IntLit)
            return "static_cast<long long>(" + b + ".size()) - " + std::to_string(x->a->i);
        return ex(x);
    };
    const Expr* st = e->d.get();
    if (st) {
        auto isStep = [&](long long want) {
            if (st->kind == EK::IntLit) return st->i == want;
            return want == -1 && st->kind == EK::UnaryOp && st->s == "-" && st->a &&
                   st->a->kind == EK::IntLit && st->a->i == 1;
        };
        if (isStep(-1) && !e->b && !e->c) {
            need(isStrT(bt) ? "string" : "vector");
            return bt + "(" + b + ".rbegin(), " + b + ".rend())";
        }
        if (!isStep(1)) {
            err(e->line,
                "native mode does not translate a slice step other than 1, or -1 as "
                "the whole-sequence `[::-1]`");
            return "{}";
        }
    }
    std::string lo = bound(e->b.get(), false), hi = bound(e->c.get(), true);
    if (isStrT(bt)) {
        need("string");
        return b + ".substr(static_cast<std::size_t>(" + lo + "), static_cast<std::size_t>(" +
               hi + " - (" + lo + ")))";
    }
    need("vector");
    return bt + "(" + b + ".begin() + (" + lo + "), " + b + ".begin() + (" + hi + "))";
}

std::string Nat::applySpec(const std::string& value, const std::string& valType,
                           const std::string& spec, int ln) {
    if (spec.empty()) return value;
    std::string pre, post;
    size_t j = 0;
    char fill = 0, align = 0;
    if (spec.size() >= 2 && (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
        fill = spec[0];
        align = spec[1];
        j = 2;
    } else if (spec[0] == '<' || spec[0] == '>' || spec[0] == '^') {
        align = spec[0];
        j = 1;
    }
    int width = 0;
    bool zeroPad = false;
    while (j < spec.size() && std::isdigit(static_cast<unsigned char>(spec[j]))) {
        if (spec[j] == '0' && width == 0) zeroPad = true;
        width = width * 10 + (spec[j++] - '0');
    }
    int prec = -1;
    if (j < spec.size() && spec[j] == '.') {
        ++j;
        prec = 0;
        while (j < spec.size() && std::isdigit(static_cast<unsigned char>(spec[j])))
            prec = prec * 10 + (spec[j++] - '0');
    }
    char type = j < spec.size() ? spec[j++] : 0;
    bool comma = false;
    if (type == ',') {  // `{x:,}` has no type letter
        comma = true;
        type = 0;
    }
    for (; j < spec.size(); ++j) {
        if (spec[j] == ',') comma = true;
        else if (spec[j] == '%') type = '%';
        else if (spec[j] == 'b') type = 'b';
        else if (spec[j] == 'n')
            err(ln, "the 'n' format type is not translated in native mode");
        else
            err(ln, std::string("native mode does not translate the format type '") + spec[j] +
                        "'");
    }

    // The value as Python would stringify it: needed whenever the spec works on
    // the text rather than on the number.
    auto asString = [&]() -> std::string {
        if (isStrT(valType)) return value;
        if (valType == "bool") return "(" + value + " ? \"True\" : \"False\")";
        return "std::to_string(" + value + ")";
    };

    // Specs that std::iomanip cannot express get their own one-line helper.
    std::string core = value;
    bool coreIsString = false;
    if (type == 'b') {
        if (!isIntT(valType)) {
            err(ln, "the 'b' format type needs an int in native mode");
            return value;
        }
        fmtBin = true;
        need("string");
        core = "p2cpp_bin(" + value + ")";
        coreIsString = true;
    } else if (type == '%') {
        if (!isFloatT(valType)) {
            err(ln, "the '%' format type needs a float in native mode");
            return value;
        }
        fmtPct = true;
        need("string");
        core = "p2cpp_pct(" + value + ", " + std::to_string(prec >= 0 ? prec : 6) + ")";
        coreIsString = true;
    } else if (comma) {
        if (!isIntT(valType)) {
            err(ln, "the ',' flag needs an int in native mode");
            return value;
        }
        fmtThousands = true;
        need("string");
        core = "p2cpp_thousands(" + value + ")";
        coreIsString = true;
    }

    if (align == '^') {
        if (width <= 0) return core;
        fmtCenter = true;
        need("string");
        std::string s = coreIsString ? core : asString();
        return "p2cpp_center(" + s + ", " + std::to_string(width) + ", '" +
               (fill ? fill : ' ') + "')";
    }

    need("iomanip");
    if (fill) pre += "std::setfill('" + std::string(1, fill) + "') << ";
    if (zeroPad && !fill) pre += "std::setfill('0') << ";
    if (width > 0) pre += "std::setw(" + std::to_string(width) + ") << ";
    if (align == '>') pre += "std::right << ";
    if (align == '<') pre += "std::left << ";
    if (type == 'f' || type == 'F') {
        pre += "std::fixed << ";
        if (prec >= 0) pre += "std::setprecision(" + std::to_string(prec) + ") << ";
        post += " << std::defaultfloat << std::setprecision(6)";
    } else if (type == 'x' || type == 'X' || type == 'o') {
        pre += (type == 'o') ? "std::oct << " : "std::hex << ";
        post += " << std::dec";
    } else if (type == 'e' || type == 'E' || type == 'g' || type == 'G' || type == 'a') {
        pre += "std::scientific << ";
        if (prec >= 0) pre += "std::setprecision(" + std::to_string(prec) + ") << ";
        post += " << std::defaultfloat << std::setprecision(6)";
    } else if (prec >= 0 && !coreIsString) {
        pre += "std::setprecision(" + std::to_string(prec) + ") << ";
        post += " << std::setprecision(6)";
    }
    if (align == '>') post += " << std::left";
    if (align == '<') post += " << std::right";
    if (fill || zeroPad) post += " << std::setfill(' ')";
    return pre + core + post;
}

// A value rendered the way Python's str() would, as far as plain `operator<<`
// allows. Floats are printed as `std::cout` prints them (5, not 5.0 -- the
// difference is accepted); containers have no `operator<<` at all, so they are
// printed by an inline lambda that builds "[1, 2]" / "{1, 2}" / "{'a': 1}".
std::string Nat::reprText(const Expr* e) {
    std::string t = typeOf(e);
    if (t == "bool") return "(" + ex(e) + " ? \"True\" : \"False\")";
    if (t == "double" || t == "float") return exP(e);  // 5.0 prints as 5
    if (isContainerT(t)) {
        need("string");
        if (isMapT(t)) noteDictOrder(e->line);
        return containerText(t, exP(e));
    }
    // A user-class instance with a `__str__`: Python's print(obj) calls it.
    if (klasses.count(t) && !methodReturnType(t, "__str__").empty())
        return exP(e) + ".__str__()";
    return ex(e);
}

// The string form of one element, used when a container is printed. Numeric
// elements go through std::to_string (a float element prints 5, not 5.0);
// nested containers recurse; everything else is printed directly.
std::string Nat::elemAsStr(const std::string& t, const std::string& v) {
    if (t.empty()) return "std::to_string(" + v + ")";
    if (t == "bool") return "(" + v + " ? \"True\" : \"False\")";
    if (isStrT(t)) return v;
    if (isContainerT(t)) return containerText(t, v);
    return "std::to_string(" + v + ")";
}

// An inline immediately-invoked lambda that renders a container, so no helper
// function has to be emitted. `auto&&` binds the value whether it is an lvalue
// or a temporary (a slice, a sorted copy, a list-comprehension result ...).
std::string Nat::containerText(const std::string& t, const std::string& v) {
    need("string");
    std::string c = capture();
    std::string open = "[", close = "]";
    std::string elemType = elemOf(t);
    std::string body;
    if (isMapT(t)) {
        need("map");
        open = "{", close = "}";
        std::string k = keyOf(t), val = valOf(t);
        body = "for (const auto& __kv : __c) { if (!__f) __o += \", \"; __f = false; __o += " +
               elemAsStr(k, "__kv.first") + " + \": \" + " + elemAsStr(val, "__kv.second") +
               "; }";
    } else if (isPairT(t)) {
        need("utility");
        auto el = splitTop(tplInner(t));
        return c + "() { auto&& __c = " + v + "; std::string __o = \"(\"; __o += " +
               elemAsStr(el[0], "__c.first") + " + \", \" + " +
               elemAsStr(el[1], "__c.second") + "; return __o + \")\"; }()";
    } else if (isTupleT(t)) {
        need("tuple");
        // Expand the tuple with std::apply; each element is rendered through
        // std::to_string (a float element prints 5, not 5.0 -- accepted).
        return c + "() { auto&& __c = " + v + "; std::string __o = \"(\"; bool __f = true; "
               "std::apply([&](const auto&... __x){ ((__o += (__f ? \"\" : \", \"), __f = false, "
               "__o += std::to_string(__x)), ...); }, __c); return __o + \")\"; }()";
    } else {
        if (isSetT(t)) {
            need("set");
            open = "{", close = "}";
        } else {
            need("vector");
        }
        body = "for (const auto& __x : __c) { if (!__f) __o += \", \"; __f = false; __o += " +
               elemAsStr(elemType, "__x") + "; }";
    }
    return c + "() { auto&& __c = " + v + "; std::string __o = \"" + open +
           "\"; bool __f = true; " + body + " return __o + \"" + close + "\"; }()";
}

// The only helpers still emitted are the format-spec ones (f"{x:b}", :.1%,
// :, and :^10), which std::iomanip cannot express, plus the web-scraping ones
// (curl download, regex findall, JSON, CSV), which the standard library has no
// one-expression spelling for. Values are printed inline (floats via
// operator<<, containers via an inline lambda), so no p2cpp_repr runtime
// survives.
std::string Nat::reprHelpers() {
    std::string s = specHelpers();
    std::string c = scrapeHelpers();
    if (!s.empty() && !c.empty()) s += "\n";
    s += c;
    return s.empty() ? "" : s + "\n";
}

// The helper definitions are emitted below main(), so the translated program
// reads first. These are the declarations that let it call them.
std::string Nat::reprHelperDecls() {
    bool any = fmtBin || fmtPct || fmtThousands || fmtCenter || useFetch || useFindall ||
               useReSub || useJson || useCsv;
    if (!any) return "";
    std::string s;
    if (fmtBin) s += "[[maybe_unused]] static std::string p2cpp_bin(long long v);\n";
    if (fmtPct) s += "[[maybe_unused]] static std::string p2cpp_pct(double v, int prec);\n";
    if (fmtThousands) s += "[[maybe_unused]] static std::string p2cpp_thousands(long long v);\n";
    if (fmtCenter)
        s += "[[maybe_unused]] static std::string p2cpp_center(const std::string& s, int w, char fill);\n";
    if (useFetch) s += "[[maybe_unused]] static std::string p2cpp_fetch(const std::string& url);\n";
    if (useFindall || useReSub)
        s += "[[maybe_unused]] static std::string p2cpp_py2cpp_re(const std::string& p);\n";
    if (useFindall)
        s += "[[maybe_unused]] static std::vector<std::string> p2cpp_findall(const std::string& text, const std::string& pattern);\n";
    if (useReSub)
        s += "[[maybe_unused]] static std::string p2cpp_re_sub(const std::string& text, const std::string& pattern, const std::string& repl);\n";
    if (useJson)
        s += "[[maybe_unused]] static std::map<std::string, std::string> p2cpp_json_parse(const std::string& text);\n"
             "[[maybe_unused]] static std::string p2cpp_json_dump(const std::map<std::string, std::string>& m);\n";
    if (useCsv)
        s += "[[maybe_unused]] static std::string p2cpp_csv_row(const std::vector<std::string>& cells);\n"
             "[[maybe_unused]] static void p2cpp_csv_save(const std::string& path, const std::vector<std::vector<std::string>>& rows);\n";
    return "// Helpers used above; defined at the end of this file.\n" + s + "\n";
}

// Format specs std::iomanip cannot express. Emitted only when a program uses
// one of them.
std::string Nat::specHelpers() {
    std::string s;
    if (fmtBin) {
        need("string");
        s +=
            "// f\"{x:b}\" -- binary, without the 0b prefix.\n"
            "[[maybe_unused]] static std::string p2cpp_bin(long long v) {\n"
            "    if (v == 0) return \"0\";\n"
            "    bool neg = v < 0;\n"
            "    unsigned long long u = neg ? 0ULL - static_cast<unsigned long long>(v)\n"
            "                               : static_cast<unsigned long long>(v);\n"
            "    std::string r;\n"
            "    for (; u; u >>= 1) r.insert(r.begin(), static_cast<char>('0' + (u & 1ULL)));\n"
            "    return neg ? \"-\" + r : r;\n"
            "}\n";
    }
    if (fmtPct) {
        need("string");
        need("sstream");
        need("iomanip");
        s +=
            "// f\"{x:.1%}\" -- a percentage with a fixed number of decimals.\n"
            "[[maybe_unused]] static std::string p2cpp_pct(double v, int prec) {\n"
            "    std::ostringstream o;\n"
            "    o << std::fixed << std::setprecision(prec) << v * 100.0;\n"
            "    return o.str() + \"%\";\n"
            "}\n";
    }
    if (fmtThousands) {
        need("string");
        s +=
            "// f\"{x:,}\" -- ',' every three digits.\n"
            "[[maybe_unused]] static std::string p2cpp_thousands(long long v) {\n"
            "    std::string d = std::to_string(v), o;\n"
            "    bool neg = !d.empty() && d[0] == '-';\n"
            "    if (neg) d = d.substr(1);\n"
            "    for (std::size_t i = 0; i < d.size(); ++i) {\n"
            "        if (i && (d.size() - i) % 3 == 0) o += ',';\n"
            "        o += d[i];\n"
            "    }\n"
            "    return neg ? \"-\" + o : o;\n"
            "}\n";
    }
    if (fmtCenter) {
        need("string");
        s +=
            "// f\"{x:^10}\" -- centring, which std::setw cannot do.\n"
            "[[maybe_unused]] static std::string p2cpp_center(const std::string& s, int w, "
            "char fill) {\n"
            "    if (static_cast<long long>(s.size()) >= w) return s;\n"
            "    std::size_t n = static_cast<std::size_t>(w - "
            "static_cast<long long>(s.size()));\n"
            "    std::size_t left = n / 2;\n"
            "    return std::string(left, fill) + s + std::string(n - left, fill);\n"
            "}\n";
    }
    return s;
}

// Web-scraping helpers, emitted only when the program calls into `requests`/
// `urllib`/`re`/`json`/`csv`. The standard library has no HTTP client, no JSON
// and no "find all regex matches", so these spell it out with the tools it does
// have: `curl` (bundled with macOS/Linux/Windows 10+) for downloads, `<regex>`
// for matching, `<fstream>` for CSV, and a small hand-rolled JSON reader.
std::string Nat::scrapeHelpers() {
    std::string s;
    if (useFetch) {
        need("string");
        need("cstdio");
        need("cstdlib");
        s +=
            "// requests.get(url) / urllib.request.urlopen(url) -- download via the\n"
            "// system `curl` (present on macOS, Linux and Windows 10+), reading the\n"
            "// whole body into a std::string. No HTTP client in the standard library.\n"
            "[[maybe_unused]] static std::string p2cpp_fetch(const std::string& url) {\n"
            "    std::string q;\n"
            "    for (char c : url) { if (c == '\\'') q += \"'\\\\''\"; else q += c; }\n"
            "    std::string cmd = \"curl -s '\" + q + \"'\";\n"
            "    FILE* p = popen(cmd.c_str(), \"r\");\n"
            "    if (!p) return std::string();\n"
            "    std::string out; char buf[4096];\n"
            "    std::size_t n;\n"
            "    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);\n"
            "    pclose(p);\n"
            "    return out;\n"
            "}\n";
    }
    if (useFindall || useReSub) {
        need("string");
        s +=
            "// std::regex (ECMAScript) has no non-greedy quantifiers, so Python's\n"
            "// `*?` / `+?` / `??` are relaxed to `*` / `+` / `?`. Prefer a negated\n"
            "// class for the same effect: re.findall(\"<tag>([^<]*)</tag>\", s).\n"
            "[[maybe_unused]] static std::string p2cpp_py2cpp_re(const std::string& p) {\n"
            "    std::string o;\n"
            "    for (std::size_t i = 0; i < p.size(); ++i) {\n"
            "        if (p[i] == '?' && i + 1 < p.size() &&\n"
            "            (p[i + 1] == '*' || p[i + 1] == '+' || p[i + 1] == '?')) {\n"
            "            continue;  // drop the non-greedy marker\n"
            "        }\n"
            "        o += p[i];\n"
            "    }\n"
            "    return o;\n"
            "}\n";
    }
    if (useFindall) {
        need("string");
        need("vector");
        need("regex");
        s +=
            "// re.findall(pattern, text) -- every non-overlapping match. Like\n"
            "// Python, a pattern with a capture group yields the group, not the\n"
            "// whole match (so re.findall(\"(\\\\w+)\", s) returns the words).\n"
            "[[maybe_unused]] static std::vector<std::string> p2cpp_findall(\n"
            "        const std::string& text, const std::string& pattern) {\n"
            "    std::vector<std::string> out;\n"
            "    std::regex re(p2cpp_py2cpp_re(pattern));\n"
            "    for (std::sregex_iterator it(text.begin(), text.end(), re), end;\n"
            "         it != end; ++it) {\n"
            "        const std::smatch& m = *it;\n"
            "        out.push_back(m.size() > 1 ? m.str(1) : m.str(0));\n"
            "    }\n"
            "    return out;\n"
            "}\n";
    }
    if (useReSub) {
        need("string");
        need("regex");
        s +=
            "// re.sub(pattern, repl, text) -- replace every match with `repl`.\n"
            "[[maybe_unused]] static std::string p2cpp_re_sub(const std::string& text,\n"
            "        const std::string& pattern, const std::string& repl) {\n"
            "    return std::regex_replace(text, std::regex(p2cpp_py2cpp_re(pattern)), repl);\n"
            "}\n";
    }
    if (useJson) {
        need("string");
        need("vector");
        need("map");
        need("cctype");
        s +=
            "// A minimal JSON reader/writer. Values are stored as strings, so a\n"
            "// number stays a std::string (\"42\") rather than a long long; use\n"
            "// int(d[\"n\"]) to convert. Objects and arrays of scalars are supported;\n"
            "// nested structures are flattened to their string representation.\n"
            "[[maybe_unused]] static std::string p2cpp_json_unescape(const std::string& s) {\n"
            "    std::string o;\n"
            "    for (std::size_t i = 0; i < s.size(); ++i) {\n"
            "        if (s[i] != '\\\\' || i + 1 >= s.size()) { o += s[i]; continue; }\n"
            "        char c = s[++i];\n"
            "        if (c == 'n') o += '\\n'; else if (c == 't') o += '\\t';\n"
            "        else if (c == 'r') o += '\\r'; else if (c == '\"') o += '\"';\n"
            "        else if (c == '\\\\') o += '\\\\'; else { o += '\\\\'; o += c; }\n"
            "    }\n"
            "    return o;\n"
            "}\n"
            "[[maybe_unused]] static std::string p2cpp_json_escape(const std::string& s) {\n"
            "    std::string o;\n"
            "    for (char c : s) {\n"
            "        if (c == '\"') o += \"\\\\\\\"\";\n"
            "        else if (c == '\\\\') o += \"\\\\\\\\\";\n"
            "        else if (c == '\\n') o += \"\\\\n\";\n"
            "        else o += c;\n"
            "    }\n"
            "    return o;\n"
            "}\n"
            "[[maybe_unused]] static std::map<std::string, std::string> p2cpp_json_parse(\n"
            "        const std::string& text) {\n"
            "    std::map<std::string, std::string> m;\n"
            "    std::size_t i = 0, n = text.size();\n"
            "    auto skip = [&]() { while (i < n && std::isspace((unsigned char)text[i])) ++i; };\n"
            "    if (i < n && text[i] == '{') {\n"
            "        ++i;\n"
            "        while (true) {\n"
            "            skip(); if (i >= n) break;\n"
            "            if (text[i] == '}') { ++i; break; }\n"
            "            if (text[i] == '\"') {\n"
            "                ++i; std::string key;\n"
            "                while (i < n && text[i] != '\"') { key += text[i]; ++i; }\n"
            "                ++i; skip();\n"
            "                if (i < n && text[i] == ':') ++i;\n"
            "                skip();\n"
            "                std::string val;\n"
            "                if (i < n && text[i] == '\"') {\n"
            "                    ++i; while (i < n && text[i] != '\"') { val += text[i]; ++i; }\n"
            "                    ++i;\n"
            "                    m[key] = p2cpp_json_unescape(val);\n"
            "                } else {\n"
            "                    while (i < n && text[i] != ',' && text[i] != '}') { val += text[i]; ++i; }\n"
            "                    // trim\n"
            "                    std::size_t b = val.find_first_not_of(\" \\t\\r\\n\");\n"
            "                    std::size_t e2 = val.find_last_not_of(\" \\t\\r\\n\");\n"
            "                    val = (b == std::string::npos) ? std::string() : val.substr(b, e2 - b + 1);\n"
            "                    m[key] = val;\n"
            "                }\n"
            "                skip();\n"
            "                if (i < n && text[i] == ',') ++i;\n"
            "            } else {\n"
            "                while (i < n && text[i] != '\"') ++i;\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    return m;\n"
            "}\n"
            "[[maybe_unused]] static std::string p2cpp_json_dump(\n"
            "        const std::map<std::string, std::string>& m) {\n"
            "    std::string o = \"{\";\n"
            "    bool first = true;\n"
            "    for (const auto& kv : m) {\n"
            "        if (!first) o += \", \";\n"
            "        first = false;\n"
            "        o += \"\\\"\" + kv.first + \"\\\": \\\"\" + p2cpp_json_escape(kv.second) + \"\\\"\";\n"
            "    }\n"
            "    return o + \"}\";\n"
            "}\n";
    }
    if (useCsv) {
        need("string");
        need("vector");
        need("fstream");
        s +=
            "// csv -- one row of comma-separated values, quoted when a cell holds a\n"
            "// comma or a quote (RFC 4180).\n"
            "[[maybe_unused]] static std::string p2cpp_csv_row(\n"
            "        const std::vector<std::string>& cells) {\n"
            "    std::string o;\n"
            "    for (std::size_t i = 0; i < cells.size(); ++i) {\n"
            "        if (i) o += ',';\n"
            "        const std::string& c = cells[i];\n"
            "        bool q = c.find(',') != std::string::npos || c.find('\"') != std::string::npos\n"
            "                 || c.find('\\n') != std::string::npos;\n"
            "        if (!q) { o += c; continue; }\n"
            "        o += '\"';\n"
            "        for (char ch : c) { if (ch == '\"') o += \"\\\"\\\"\"; else o += ch; }\n"
            "        o += '\"';\n"
            "    }\n"
            "    return o;\n"
            "}\n"
            "// csv.save(filename, rows) -- write a list of rows (each a list of\n"
            "// strings) to a file, one row per line.\n"
            "[[maybe_unused]] static void p2cpp_csv_save(const std::string& path,\n"
            "        const std::vector<std::vector<std::string>>& rows) {\n"
            "    std::ofstream f(path);\n"
            "    for (const auto& r : rows) f << p2cpp_csv_row(r) << \"\\n\";\n"
            "}\n";
    }
    return s;
}

std::string Nat::outChain(const Expr* e) {
    if (e->kind == EK::FStr) {
        std::string s;
        bool first = true;
        for (const auto& p : e->parts) {
            if (!p.isExpr) {
                if (p.literal.empty()) continue;
                if (!first) s += " << ";
                s += "\"" + escapeCpp(p.literal) + "\"";
                first = false;
                continue;
            }
            std::string v;
            if (p.spec.empty()) {
                v = outChain(p.expr.get());
            } else if (typeOf(p.expr.get()) == "bool") {
                v = "(" + ex(p.expr.get()) + " ? \"True\" : \"False\")";
            } else if (isContainerT(typeOf(p.expr.get()))) {
                need("string");
                warn(e->line, "a format specifier on a container is ignored in native mode");
                v = containerText(typeOf(p.expr.get()), exP(p.expr.get()));
            } else {
                v = ex(p.expr.get());
            }
            if (!first) s += " << ";
            s += applySpec(v, typeOf(p.expr.get()), p.spec, e->line);
            first = false;
        }
        return s.empty() ? "\"\"" : s;
    }
    return reprText(e);
}

std::string Nat::fstrValue(const Expr* e) {
    need("string");
    std::string s;
    for (const auto& p : e->parts) {
        std::string piece;
        if (!p.isExpr) {
            if (p.literal.empty()) continue;
            piece = "\"" + escapeCpp(p.literal) + "\"";
        } else {
            if (!p.spec.empty())
                warn(e->line,
                     "a format specifier in an expression position is ignored in native mode; "
                     "put the f-string in print(...) instead");
            std::string t = typeOf(p.expr.get());
            std::string v = ex(p.expr.get());
            if (isStrT(t)) piece = v;
            else if (t == "bool") piece = "std::string(" + v + " ? \"True\" : \"False\")";
            else if (t == "double" || t == "float") piece = "std::to_string(" + v + ")";
            else if (isContainerT(t)) piece = containerText(t, v);
            else piece = "std::to_string(" + v + ")";
        }
        if (s.empty()) s = "std::string(" + piece + ")";
        else s += " + " + piece;
    }
    return s.empty() ? "std::string()" : s;
}

// ===========================================================================
// operators
// ===========================================================================

std::string Nat::setBinOp(const Expr* e, const std::string& op) {
    need("set");
    std::string t = typeOf(e);
    if (t.empty()) {
        err(e->line, "native mode needs to know the element type of this set operation");
        return "{}";
    }
    std::string a = exP(e->a.get()), b = exP(e->b.get());
    std::string cap = capture();
    if (op == "-")
        return cap + "() { " + t + " __r; for (const auto& __x : " + a + ") if (!" + b +
               ".count(__x)) __r.insert(__x); return __r; }()";
    if (op == "&")
        return cap + "() { " + t + " __r; for (const auto& __x : " + a + ") if (" + b +
               ".count(__x)) __r.insert(__x); return __r; }()";
    if (op == "|")
        return cap + "() { " + t + " __r(" + a + "); __r.insert(" + b + ".begin(), " + b +
               ".end()); return __r; }()";
    return cap + "() { " + t + " __r; for (const auto& __x : " + a + ") if (!" + b +
           ".count(__x)) __r.insert(__x); for (const auto& __x : " + b + ") if (!" + a +
           ".count(__x)) __r.insert(__x); return __r; }()";
}

std::string Nat::binOp(const Expr* e) {
    const std::string& op = e->s;
    std::string ta = typeOf(e->a.get()), tb = typeOf(e->b.get());

    if ((op == "|" || op == "&" || op == "^" || op == "-") &&
        (isSetT(ta) || isSetT(tb)))
        return setBinOp(e, op);

    std::string a = exP(e->a.get()), b = exP(e->b.get());
    std::string cap = capture();

    if (op == "*" && isStrT(ta) && isIntT(tb)) {
        need("string");
        return "std::string(" + a + ").append(" + b + ", std::string())";
    }
    if (op == "*" && isIntT(ta) && isStrT(tb)) {
        need("string");
        return "std::string(" + b + ").append(" + a + ", std::string())";
    }
    // `[1, 2] * 3` repeats the list; there is no operator for that in C++.
    if (op == "*" && isVecT(ta) && isIntT(tb)) {
        need("vector");
        return cap + "() { auto __s = " + a + "; " + ta + " __r; for (long long __i = 0; __i < (" +
               b + "); ++__i) __r.insert(__r.end(), __s.begin(), __s.end()); return __r; }()";
    }
    if (op == "*" && isIntT(ta) && isVecT(tb)) {
        need("vector");
        return cap + "() { auto __s = " + b + "; " + tb + " __r; for (long long __i = 0; __i < (" +
               a + "); ++__i) __r.insert(__r.end(), __s.begin(), __s.end()); return __r; }()";
    }
    if (op == "+" && (isStrT(ta) || isStrT(tb))) {
        need("string");
        // `"n = " + n` must stringify the way Python's str() would.
        auto asStr = [&](const std::string& text, const std::string& t) -> std::string {
            if (t == "bool") return "(" + text + " ? \"True\" : \"False\")";
            if (t == "double" || t == "float") return "std::to_string(" + text + ")";
            if (isContainerT(t)) return containerText(t, text);
            return "std::to_string(" + text + ")";
        };
        if (isStrT(ta) && !tb.empty() && !isStrT(tb)) return a + " + " + asStr(b, tb);
        if (isStrT(tb) && !ta.empty() && !isStrT(ta)) return asStr(a, ta) + " + " + b;
        if (e->a->kind == EK::StrLit && e->b->kind == EK::StrLit)
            return "std::string(" + a + ") + " + b;
        return a + " + " + b;
    }
    // Python raises ZeroDivisionError where C++ silently produces inf (or hits
    // undefined behaviour). When the divisor is a literal zero we can keep the
    // behaviour by throwing; a throw-expression is legal inside ?:.
    auto guarded = [&](const std::string& core) -> std::string {
        std::string msg = divZeroMessage(e);
        if (msg.empty()) return core;
        need("stdexcept");
        return "(" + b + " == 0 ? throw std::domain_error(\"" + msg + "\") : " + core + ")";
    };
    if (op == "/") {
        if (isIntT(ta) && isIntT(tb)) return guarded("static_cast<double>(" + a + ") / " + b);
        if (ta.empty() && isIntT(tb)) return guarded("static_cast<double>(" + a + ") / " + b);
        return guarded(a + " / " + b);
    }
    if (op == "//") {
        need("cmath");
        if (isIntT(ta) && isIntT(tb))
            return guarded("static_cast<long long>(std::floor(static_cast<double>(" + a +
                           ") / static_cast<double>(" + b + ")))");
        return guarded("std::floor(" + a + " / " + b + ")");
    }
    if (op == "%") {
        if (isIntT(ta) && isIntT(tb))
            return guarded("((" + a + " % " + b + ") + " + b + ") % " + b);
        need("cmath");
        return guarded("std::fmod(std::fmod(" + a + ", " + b + ") + " + b + ", " + b + ")");
    }
    if (op == "**") {
        need("cmath");
        if (isIntT(ta) && isIntT(tb))
            return "static_cast<long long>(std::llround(std::pow(static_cast<double>(" + a +
                   "), static_cast<double>(" + b + "))))";
        return "std::pow(" + a + ", " + b + ")";
    }
    return a + " " + op + " " + b;
}

std::string Nat::compare(const Expr* e) {
    std::string out;
    for (size_t i = 0; i + 1 < e->items.size(); ++i) {
        const Expr* L = e->items[i].get();
        const Expr* R = e->items[i + 1].get();
        const std::string& op = i < e->ops.size() ? e->ops[i] : "==";
        std::string piece;
        if (op == "in" || op == "not in") {
            std::string t = typeOf(R);
            std::string rr = exP(R), ll = exP(L);
            if (isStrT(t)) {
                need("string");
                piece = rr + ".find(" + ll + ") != std::string::npos";
            } else if (isVecT(t)) {
                need("algorithm");
                piece = "std::find(" + rr + ".begin(), " + rr + ".end(), " + ll + ") != " + rr +
                        ".end()";
            } else if (isSetT(t) || isMapT(t)) {
                piece = rr + ".count(" + ll + ") > 0";
            } else {
                err(e->line,
                    "native mode cannot test membership in an operand whose type it does not "
                    "know");
                piece = "false";
            }
            if (op == "not in") piece = "!(" + piece + ")";
        } else if (op == "is" || op == "is not") {
            err(e->line, "`is` / `is not` compare object identity and have no C++ equivalent");
            piece = "false";
        } else {
            piece = exP(L) + " " + op + " " + exP(R);
        }
        if (!out.empty()) out += " && ";
        out += piece;
    }
    return out.empty() ? "false" : out;
}

// ===========================================================================
// calls
// ===========================================================================

std::string Nat::argList(const Expr* e, size_t from) {
    std::vector<std::string> parts;
    for (size_t i = from; i < e->items.size(); ++i) parts.push_back(ex(e->items[i].get()));
    for (const auto& kw : e->kwargs) parts.push_back(ex(kw.second.get()));
    return blend(parts, ", ");
}

std::string Nat::moduleCall(const Expr* e, const std::string& mod, const std::string& fn) {
    std::string x = e->items.empty() ? std::string("0") : ex(e->items[0].get());
    if (mod == "math") {
        need("cmath");
        if (fn == "sqrt") return "std::sqrt(" + x + ")";
        if (fn == "fabs" || fn == "abs") return "std::fabs(" + x + ")";
        if (fn == "pow") return "std::pow(" + argList(e) + ")";
        if (fn == "exp") return "std::exp(" + x + ")";
        if (fn == "log") return "std::log(" + x + ")";
        if (fn == "log2") return "std::log2(" + x + ")";
        if (fn == "log10") return "std::log10(" + x + ")";
        if (fn == "sin") return "std::sin(" + x + ")";
        if (fn == "cos") return "std::cos(" + x + ")";
        if (fn == "tan") return "std::tan(" + x + ")";
        if (fn == "asin") return "std::asin(" + x + ")";
        if (fn == "acos") return "std::acos(" + x + ")";
        if (fn == "atan") return "std::atan(" + x + ")";
        if (fn == "atan2") return "std::atan2(" + argList(e) + ")";
        if (fn == "hypot") return "std::hypot(" + argList(e) + ")";
        if (fn == "fmod") return "std::fmod(" + argList(e) + ")";
        if (fn == "floor") return "static_cast<long long>(std::floor(" + x + "))";
        if (fn == "ceil") return "static_cast<long long>(std::ceil(" + x + "))";
        if (fn == "trunc") return "static_cast<long long>(std::trunc(" + x + "))";
        if (fn == "isqrt") return "static_cast<long long>(std::sqrt(" + x + "))";
        if (fn == "isnan") return "std::isnan(" + x + ")";
        if (fn == "isinf") return "std::isinf(" + x + ")";
        if (fn == "isfinite") return "std::isfinite(" + x + ")";
        if (fn == "degrees") return "(" + x + " * 57.29577951308232)";
        if (fn == "radians") return "(" + x + " * 0.017453292519943295)";
        if (fn == "gcd") {
            need("numeric");
            return "static_cast<long long>(std::gcd(" + argList(e) + "))";
        }
        if (fn == "factorial") {
            need("stdexcept");
            return capture() + "() { long long __n = " + x +
                   "; if (__n < 0) throw std::invalid_argument(\"factorial() not defined for "
                   "negative values\"); long long __r = 1; for (long long __i = 2; __i <= "
                   "__n; ++__i) __r *= __i; return __r; }()";
        }
    }
    if (mod == "random") {
        need("cstdlib");
        usesRandom = true;
        warn(e->line,
             "p2cpp --native uses std::rand(), whose sequence differs from Python's, so random "
             "results will not match CPython");
        if (fn == "random") return "(static_cast<double>(std::rand()) / (RAND_MAX + 1.0))";
        if (fn == "uniform" && e->items.size() >= 2) {
            std::string a = ex(e->items[0].get()), b = ex(e->items[1].get());
            return "(" + a + " + (static_cast<double>(std::rand()) / (RAND_MAX + 1.0)) * (" + b +
                   " - " + a + "))";
        }
        if (fn == "randint" && e->items.size() >= 2) {
            std::string a = ex(e->items[0].get()), b = ex(e->items[1].get());
            return "(" + a + " + static_cast<long long>(std::rand()) % ((" + b + ") - (" + a +
                   ") + 1))";
        }
        if (fn == "choice") return x + "[static_cast<std::size_t>(std::rand()) % " + x + ".size()]";
        if (fn == "seed") return "(std::srand(static_cast<unsigned>(" + x + ")), 0)";
    }
    if (mod == "time") {
        if (fn == "time") {
            need("chrono");
            return "std::chrono::duration<double>(std::chrono::system_clock::now()"
                   ".time_since_epoch()).count()";
        }
        if (fn == "sleep") {
            need("thread");
            need("chrono");
            return "(std::this_thread::sleep_for(std::chrono::duration<double>(" + x +
                   ")), 0)";
        }
    }
    if (mod == "sys" && fn == "exit") {
        need("cstdlib");
        return "(std::exit(" + (e->items.empty() ? std::string("0") : x) + "), 0)";
    }
    // --- web scraping -----------------------------------------------------
    if (mod == "requests") {
        if (fn == "get") {
            useFetch = true;
            // requests.get(url).text -> the body. We model the Response object
            // as its text, so `.text`/`.content` are not separate steps.
            return "p2cpp_fetch(" + exP(e->items[0].get()) + ")";
        }
        err(e->line, "native mode translates requests.get(); '" + fn + "' is not supported");
        return "\"\"";
    }
    if (mod == "urllib") {
        err(e->line, "native mode translates urllib.request.urlopen(); use requests.get() instead");
        return "\"\"";
    }
    if (mod == "re") {
        if (fn == "findall") {
            useFindall = true;
            // Python: re.findall(pattern, string); arguments are (pattern, text).
            std::string pat = exP(e->items[0].get());
            std::string text = exP(e->items[1].get());
            return "p2cpp_findall(" + text + ", " + pat + ")";
        }
        if (fn == "sub") {
            useReSub = true;
            std::string pat = exP(e->items[0].get());
            std::string repl = exP(e->items[1].get());
            std::string text = exP(e->items[2].get());
            return "p2cpp_re_sub(" + text + ", " + pat + ", " + repl + ")";
        }
        if (fn == "search" || fn == "match") {
            useFindall = true;  // pulls in p2cpp_py2cpp_re + <regex>
            std::string pat = exP(e->items[0].get());
            std::string text = exP(e->items[1].get());
            return "(" + capture() + "() { std::smatch __m; std::string __s = " + text +
                   "; std::regex __re(p2cpp_py2cpp_re(" + pat + ")); return std::regex_search(__s, __m, __re)"
                   " ? __m.str() : std::string(); }())";
        }
        err(e->line, "native mode translates re.findall/sub/search/match; '" + fn + "' is not supported");
        return "\"\"";
    }
    if (mod == "json") {
        if (fn == "loads") {
            useJson = true;
            return "p2cpp_json_parse(" + exP(e->items[0].get()) + ")";
        }
        if (fn == "dumps") {
            useJson = true;
            return "p2cpp_json_dump(" + exP(e->items[0].get()) + ")";
        }
        err(e->line, "native mode translates json.loads/dumps; '" + fn + "' is not supported");
        return "\"\"";
    }
    if (mod == "csv") {
        if (fn == "save") {
            useCsv = true;
            return "p2cpp_csv_save(" + exP(e->items[0].get()) + ", " +
                   exP(e->items[1].get()) + ")";
        }
        if (fn == "row") {
            useCsv = true;
            return "p2cpp_csv_row(" + exP(e->items[0].get()) + ")";
        }
        err(e->line, "native mode translates csv.save(filename, rows) and csv.row(cells); '" +
                         fn + "' is not supported");
        return "\"\"";
    }
    err(e->line, "native mode does not translate " + mod + "." + fn + "()");
    return "0";
}

std::string Nat::strMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string cap = capture();
    need("string");
    if (m == "upper" || m == "lower") {
        need("cctype");
        std::string f = (m == "upper") ? "std::toupper" : "std::tolower";
        return cap + "() { std::string __s = " + r + "; for (char& __c : __s) __c = "
                     "static_cast<char>(" + f + "(static_cast<unsigned char>(__c))); return "
                     "__s; }()";
    }
    if (m == "title" || m == "capitalize") {
        need("cctype");
        // capitalize(): first character upper, the rest lower. title(): the same,
        // restarted after every non-alphanumeric character.
        std::string mode = (m == "title") ? "title" : "cap";
        return cap + "() { std::string __s = " + r + "; bool __start = true; for (char& __c : __s) "
                     "{ bool __al = std::isalnum(static_cast<unsigned char>(__c)) != 0; "
                     "if (__al) { __c = static_cast<char>(__start ? "
                     "std::toupper(static_cast<unsigned char>(__c)) : "
                     "std::tolower(static_cast<unsigned char>(__c))); } " +
                     (mode == "cap" ? std::string("__start = false; ")
                                    : std::string("__start = !__al; ")) +
                     "} return __s; }()";
    }
    if (m == "strip" || m == "lstrip" || m == "rstrip") {
        need("cctype");
        if (!e->items.empty())
            warn(e->line, "strip(chars) strips whitespace only in native mode");
        std::string left = (m == "rstrip") ? "false" : "true";
        std::string right = (m == "lstrip") ? "false" : "true";
        return cap + "() { std::string __s = " + r +
               "; std::size_t __a = 0, __b = __s.size(); while (" + left +
               " && __a < __b && std::isspace(static_cast<unsigned char>(__s[__a]))) ++__a; "
               "while (" + right +
               " && __b > __a && std::isspace(static_cast<unsigned char>(__s[__b - 1]))) "
               "--__b; return __s.substr(__a, __b - __a); }()";
    }
    if (m == "startswith") {
        std::string p = e->items.empty() ? std::string("\"\"") : ex(e->items[0].get());
        return "(" + r + ".rfind(" + p + ", 0) == 0)";
    }
    if (m == "endswith") {
        std::string p = e->items.empty() ? std::string("\"\"") : ex(e->items[0].get());
        return cap + "() { std::string __p = " + p + "; return __p.size() <= " + r +
               ".size() && " + r + ".compare(" + r + ".size() - __p.size(), __p.size(), __p) "
               "== 0; }()";
    }
    if (m == "find") {
        std::string p = e->items.empty() ? std::string("\"\"") : ex(e->items[0].get());
        return cap + "() { auto __p = " + r + ".find(" + p +
               "); return __p == std::string::npos ? -1LL : static_cast<long long>(__p); }()";
    }
    if (m == "count") {
        std::string p = e->items.empty() ? std::string("\"\"") : ex(e->items[0].get());
        return cap + "() { std::string __s = " + r + ", __n = " + p +
               "; if (__n.empty()) return 0LL; long long __c = 0; for (std::size_t __i = "
               "__s.find(__n); __i != std::string::npos; __i = __s.find(__n, __i + __n.size())) "
               "++__c; return __c; }()";
    }
    if (m == "replace") {
        if (e->items.size() < 2) return r;
        std::string a = ex(e->items[0].get()), b = ex(e->items[1].get());
        return cap + "() { std::string __s = " + r + ", __a = " + a + ", __b = " + b +
               "; if (__a.empty()) return __s; for (std::size_t __p = __s.find(__a); __p != "
               "std::string::npos; __p = __s.find(__a, __p + __b.size())) __s.replace(__p, "
               "__a.size(), __b); return __s; }()";
    }
    if (m == "split") {
        need("vector");
        need("sstream");
        std::string sep = e->items.empty() ? std::string("\" \"") : ex(e->items[0].get());
        return cap + "() { std::vector<std::string> __v; std::string __s = " + r +
               ", __d = " + sep +
               "; if (__d.empty() || __d == \" \") { std::istringstream __is(__s); "
               "std::string __w; while (__is >> __w) __v.push_back(__w); return __v; } "
               "std::size_t __p = 0, __q; while ((__q = __s.find(__d, __p)) != "
               "std::string::npos) { __v.push_back(__s.substr(__p, __q - __p)); __p = __q + "
               "__d.size(); } __v.push_back(__s.substr(__p)); return __v; }()";
    }
    if (m == "join") {
        std::string it = e->items.empty() ? std::string("{}") : ex(e->items[0].get());
        return cap + "() { std::string __o; bool __f = true; for (const auto& __x : " + it +
               ") { if (!__f) __o += " + r + "; __o += __x; __f = false; } return __o; }()";
    }
    if (m == "isdigit" || m == "isalpha" || m == "isspace") {
        need("cctype");
        std::string f = (m == "isdigit") ? "std::isdigit"
                        : (m == "isalpha") ? "std::isalpha"
                                           : "std::isspace";
        return cap + "() { if (" + r + ".empty()) return false; for (char __c : " + r +
               ") if (!" + f + "(static_cast<unsigned char>(__c))) return false; return true; "
               "}()";
    }
    return "";
}

std::string Nat::listMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string cap = capture();
    if (m == "index") {
        need("algorithm");
        need("stdexcept");
        std::string v = e->items.empty() ? std::string("0") : ex(e->items[0].get());
        return cap + "() { auto __p = std::find(" + r + ".begin(), " + r + ".end(), " + v +
               "); if (__p == " + r + ".end()) throw std::out_of_range(\"list.index: not "
               "found\"); return static_cast<long long>(__p - " + r + ".begin()); }()";
    }
    if (m == "count") {
        need("algorithm");
        std::string v = e->items.empty() ? std::string("0") : ex(e->items[0].get());
        return "static_cast<long long>(std::count(" + r + ".begin(), " + r + ".end(), " + v +
               "))";
    }
    if (m == "copy") return r;
    if (m == "pop" && e->items.size() == 1) {
        std::string i = ex(e->items[0].get());
        return cap + "() { auto __v = " + r + "[" + i + "]; " + r + ".erase(" + r +
               ".begin() + " + i + "); return __v; }()";
    }
    return "";
}

std::string Nat::dictMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string cap = capture();
    std::string t = typeOf(e->a->a.get());
    std::string k = keyOf(t), v = valOf(t);
    need("map");
    if (m == "get") {
        if (e->items.empty()) return "";
        std::string key = ex(e->items[0].get());
        std::string dflt = e->items.size() > 1 ? ex(e->items[1].get()) : std::string();
        std::string vt = v.empty() ? "long long" : v;
        if (dflt.empty()) {
            if (isStrT(vt)) dflt = "\"\"";
            else if (isNumT(vt)) dflt = "0";
            else dflt = vt + "{}";
        }
        return cap + "() { auto __p = " + r + ".find(" + key + "); return __p == " + r +
               ".end() ? " + dflt + " : __p->second; }()";
    }
    if (m == "keys" || m == "values") {
        need("vector");
        std::string et = (m == "keys") ? k : v;
        if (et.empty()) {
            err(e->line, "native mode needs to know the dict's " +
                             std::string(m == "keys" ? "key" : "value") + " type here");
            return "{}";
        }
        std::string acc = (m == "keys") ? "__x.first" : "__x.second";
        return cap + "() { std::vector<" + et + "> __v; for (const auto& __x : " + r +
               ") __v.push_back(" + acc + "); return __v; }()";
    }
    if (m == "items") {
        need("vector");
        need("utility");
        if (k.empty() || v.empty()) {
            err(e->line, "native mode needs to know the dict's key and value types here");
            return "{}";
        }
        return cap + "() { std::vector<std::pair<" + k + ", " + v + ">> __v; for (const auto& "
                     "__x : " + r + ") __v.push_back(__x); return __v; }()";
    }
    return "";
}

std::string Nat::setMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string cap = capture();
    std::string t = typeOf(e->a->a.get());
    need("set");
    if (m == "union" || m == "intersection" || m == "difference" ||
        m == "symmetric_difference") {
        if (e->items.empty()) return r;
        std::string o = ex(e->items[0].get());
        if (elemOf(t).empty()) {
            err(e->line, "native mode needs to know the set's element type here");
            return "{}";
        }
        if (m == "union")
            return cap + "() { " + t + " __v(" + r + "); __v.insert(" + o +
                   ".begin(), " + o + ".end()); return __v; }()";
        if (m == "intersection")
            return cap + "() { " + t + " __v; for (const auto& __x : " + r + ") if (" + o +
                   ".count(__x)) __v.insert(__x); return __v; }()";
        if (m == "difference")
            return cap + "() { " + t + " __v; for (const auto& __x : " + r + ") if (!" + o +
                   ".count(__x)) __v.insert(__x); return __v; }()";
        return cap + "() { " + t + " __v; for (const auto& __x : " + r + ") if (!" + o +
               ".count(__x)) __v.insert(__x); for (const auto& __x : " + o + ") if (!" + r +
               ".count(__x)) __v.insert(__x); return __v; }()";
    }
    if (m == "issubset" || m == "issuperset") {
        if (e->items.empty()) return "false";
        std::string o = ex(e->items[0].get());
        std::string a = (m == "issubset") ? r : o;
        std::string b = (m == "issubset") ? o : r;
        return cap + "() { for (const auto& __x : " + a + ") if (!" + b +
               ".count(__x)) return false; return true; }()";
    }
    return "";
}

std::string Nat::stmtMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string t = typeOf(e->a->a.get());
    std::string cap = capture();
    auto arg = [&](size_t i) {
        return i < e->items.size() ? ex(e->items[i].get()) : std::string("0");
    };
    if (isVecT(t)) {
        if (m == "append") return r + ".push_back(" + arg(0) + ");";
        if (m == "extend") {
            std::string o = arg(0);
            return r + ".insert(" + r + ".end(), " + o + ".begin(), " + o + ".end());";
        }
        if (m == "insert")
            return r + ".insert(" + r + ".begin() + " + arg(0) + ", " + arg(1) + ");";
        if (m == "pop") {
            if (e->items.empty()) return r + ".pop_back();";
            return r + ".erase(" + r + ".begin() + " + arg(0) + ");";
        }
        if (m == "remove") {
            need("algorithm");
            // Python removes the first match only, not every one.
            std::string v = arg(0);
            return cap + "() { auto __p = std::find(" + r + ".begin(), " + r + ".end(), " + v +
                   "); if (__p != " + r + ".end()) " + r + ".erase(__p); }();";
        }
        if (m == "clear") return r + ".clear();";
        if (m == "sort") {
            need("algorithm");
            if (!e->kwargs.empty() || !e->items.empty())
                warn(e->line, "sort(key=/reverse=) is ignored in native mode");
            return "std::sort(" + r + ".begin(), " + r + ".end());";
        }
        if (m == "reverse") {
            need("algorithm");
            return "std::reverse(" + r + ".begin(), " + r + ".end());";
        }
    }
    if (isSetT(t)) {
        if (m == "add") return r + ".insert(" + arg(0) + ");";
        if (m == "discard" || m == "remove") return r + ".erase(" + arg(0) + ");";
        if (m == "clear") return r + ".clear();";
        if (m == "update") {
            std::string o = arg(0);
            return r + ".insert(" + o + ".begin(), " + o + ".end());";
        }
    }
    if (isMapT(t)) {
        if (m == "update") {
            std::string o = arg(0);
            return r + ".insert(" + o + ".begin(), " + o + ".end());";
        }
        if (m == "clear") return r + ".clear();";
        if (m == "pop") return r + ".erase(" + arg(0) + ");";
    }
    return "";
}

std::string Nat::exprMethod(const Expr* e, const std::string& r, const std::string& m) {
    std::string t = typeOf(e->a->a.get());
    if (isStrT(t)) {
        std::string s = strMethod(e, r, m);
        if (!s.empty()) return s;
    }
    if (isVecT(t)) {
        std::string s = listMethod(e, r, m);
        if (!s.empty()) return s;
    }
    if (isSetT(t)) {
        std::string s = setMethod(e, r, m);
        if (!s.empty()) return s;
    }
    if (isMapT(t)) {
        std::string s = dictMethod(e, r, m);
        if (!s.empty()) return s;
    }
    return "";
}

std::string Nat::call(const Expr* e) {
    const Expr* c = e->a.get();
    if (!c) return "";
    if (c->kind == EK::Attr) {
        const Expr* obj = c->a.get();
        std::string recv = obj ? recvText(obj) : "";
        if (obj && obj->kind == EK::Name && modules.count(obj->s))
            return moduleCall(e, obj->s, c->s);
        // re.search(...).group(n) / re.match(...).group(n): the match is
        // re-run inline and the nth capture group is returned (Python's most
        // common scraping idiom). group() == the whole match, group(1) == the
        // first parenthesised group.
        if (c->s == "group" && obj && isReSearchCall(obj)) {
            useFindall = true;  // pulls in p2cpp_py2cpp_re + <regex>
            const Expr* pat = obj->items[0].get();
            const Expr* text = obj->items[1].get();
            std::string idx = e->items.empty() ? std::string("0")
                                               : ex(e->items[0].get());
            return "(" + capture() + "() { std::smatch __m; std::string __s = " +
                   exP(text) + "; if (!std::regex_search(__s, __m, std::regex(p2cpp_py2cpp_re(" +
                   exP(pat) + ")))) return std::string(); return __m.str(" + idx +
                   "); }())";
        }
        std::string m = exprMethod(e, recv, c->s);
        if (!m.empty()) return m;
        // A method call on a user-class instance: `p.dist(...)` maps directly to
        // `p.dist(...)` since the class methods are emitted as member functions.
        if (obj) {
            std::string rt = typeOf(obj);
            if (!rt.empty() && klasses.count(rt))
                return recv + "." + c->s + "(" + argList(e) + ")";
        }
        err(e->line, "native mode does not translate `." + c->s +
                         "()` here; the core set of str/list/dict/set methods is in README");
        return "0";
    }
    if (c->kind != EK::Name) return exP(c) + "(" + argList(e) + ")";
    const std::string& n = c->s;

    if (n == "print" || n == "input") {
        err(e->line, "`" + n +
                         "()` is a statement in native mode and cannot appear inside an "
                         "expression");
        return "0";
    }
    if (n == "len") return "static_cast<long long>(" + exP(e->items[0].get()) + ".size())";
    if (n == "int") {
        std::string t = typeOf(e->items[0].get());
        if (isStrT(t)) {
            need("string");
            return "std::stoll(" + ex(e->items[0].get()) + ")";
        }
        return "static_cast<long long>(" + ex(e->items[0].get()) + ")";
    }
    if (n == "float") {
        std::string t = typeOf(e->items[0].get());
        if (isStrT(t)) {
            need("string");
            return "std::stod(" + ex(e->items[0].get()) + ")";
        }
        return "static_cast<double>(" + ex(e->items[0].get()) + ")";
    }
    if (n == "str" || n == "repr") {
        if (e->items.empty()) return "\"\"";
        std::string t = typeOf(e->items[0].get());
        std::string v = ex(e->items[0].get());
        need("string");
        if (isStrT(t)) {
            // str(s) strips the quotes a literal carries; repr(s) puts them back.
            if (n == "repr")
                return capture() + "() { auto& __c = " + v +
                       "; return \"'\" + __c + \"'\"; }()";
            return v;
        }
        if (t == "bool") return "std::string(" + v + " ? \"True\" : \"False\")";
        if (isFloatT(t)) return "std::to_string(" + v + ")";
        if (isContainerT(t)) return containerText(t, v);
        return "std::to_string(" + v + ")";
    }
    if (n == "bool") return "static_cast<bool>(" + ex(e->items[0].get()) + ")";
    if (n == "abs") {
        std::string t = typeOf(e->items[0].get());
        if (isFloatT(t)) {
            need("cmath");
            return "std::fabs(" + ex(e->items[0].get()) + ")";
        }
        need("cstdlib");
        return "std::llabs(" + ex(e->items[0].get()) + ")";
    }
    if (n == "round") {
        need("cmath");
        if (e->items.size() >= 2)
            return capture() + "() { double __p = std::pow(10.0, static_cast<double>(" +
                   ex(e->items[1].get()) + ")); return std::round(" + ex(e->items[0].get()) +
                   " * __p) / __p; }()";
        std::string t = typeOf(e->items[0].get());
        if (isFloatT(t)) return "std::llround(" + ex(e->items[0].get()) + ")";
        return ex(e->items[0].get());
    }
    if (n == "sum") {
        need("numeric");
        std::string t = typeOf(e->items[0].get());
        std::string v = exP(e->items[0].get());
        std::string init;
        if (e->items.size() > 1) {
            std::string st = ex(e->items[1].get());
            init = isFloatT(t) || isFloatT(typeOf(e->items[1].get()))
                       ? "static_cast<double>(" + st + ")"
                       : "static_cast<long long>(" + st + ")";
        } else {
            init = isFloatT(t) || isFloatT(elemOf(t)) ? "0.0" : "0LL";
        }
        return "std::accumulate(" + v + ".begin(), " + v + ".end(), " + init + ")";
    }
    if (n == "min" || n == "max") {
        need("algorithm");
        bool isMin = (n == "min");
        // key= handling
        if (!e->kwargs.empty() && e->items.size() == 1) {
            const Expr* keyFn = nullptr;
            for (const auto& kw : e->kwargs)
                if (kw.first == "key") keyFn = kw.second.get();
            std::string v = exP(e->items[0].get());
            std::string obj, prefix, suffix;
            bindSequence(e->items[0].get(), v, obj, prefix, suffix);
            if (keyFn && keyFn->kind == EK::Name && keyFn->s == "len") {
                // max_element wants the same "less" comparator as min_element:
                // it returns the first element nothing else outranks, which is
                // Python's tie-breaking too.
                std::string cmp =
                    "[](const auto& __a, const auto& __b) { return __a.size() < __b.size(); }";
                need("algorithm");
                return prefix + "*std::" + std::string(isMin ? "min_element" : "max_element") +
                       "(" + obj + ".begin(), " + obj + ".end(), " + cmp + ")" + suffix;
            }
            err(e->line, "native mode only supports key=len here");
            return "0";
        }
        if (e->items.size() == 1) {
            std::string v = exP(e->items[0].get());
            std::string obj, prefix, suffix;
            bindSequence(e->items[0].get(), v, obj, prefix, suffix);
            need("algorithm");
            return prefix + "*std::" + std::string(isMin ? "min_element" : "max_element") + "(" +
                   obj + ".begin(), " + obj + ".end())" + suffix;
        }
        if (e->items.size() == 2) {
            std::string a = ex(e->items[0].get()), b = ex(e->items[1].get());
            std::string ta = typeOf(e->items[0].get()), tb = typeOf(e->items[1].get());
            if (isNumT(ta) && isNumT(tb) && ta != tb) {
                bool wantDouble = isFloatT(ta) || isFloatT(tb);
                std::string ct = wantDouble ? "double" : "long long";
                return "std::" + std::string(isMin ? "min" : "max") + "<" + ct + ">(static_cast<" +
                       ct + ">(" + a + "), static_cast<" + ct + ">(" + b + "))";
            }
            return "std::" + std::string(isMin ? "min" : "max") + "(" + a + ", " + b + ")";
        }
        // min(3, 1, 2): a braced list has no .begin() and two temporaries would
        // not belong to the same sequence, so build one vector inside a lambda.
        std::string el = "long long";
        for (const auto& it : e->items) {
            std::string t = typeOf(it.get());
            if (isFloatT(t)) {
                el = t;
                break;
            }
            if (isIntT(t)) el = t;
        }
        need("vector");
        need("algorithm");
        return capture() + "() { std::vector<" + el + "> __v{" + argList(e) +
               "}; return *std::" + std::string(isMin ? "min_element" : "max_element") +
               "(__v.begin(), __v.end()); }()";
    }
    if (n == "sorted") {
        std::string cap = capture();
        std::string t = typeOf(e->items[0].get());
        std::string v = exP(e->items[0].get());
        std::string copy;
        std::string rt = t;
        if (isSetT(t)) {
            need("vector");
            rt = "std::vector<" + elemOf(t) + ">";
            copy = rt + " __v(" + v + ".begin(), " + v + ".end());";
        } else if (isVecT(t)) {
            copy = "auto __v = " + v + ";";
        } else {
            err(e->line, "native mode can only sort a list or a set here");
            return "{}";
        }
        need("algorithm");
        bool reverse = false;
        const Expr* keyFn = nullptr;
        for (const auto& kw : e->kwargs) {
            if (kw.first == "reverse" && kw.second && kw.second->kind == EK::BoolLit)
                reverse = kw.second->boolLit;
            else if (kw.first == "key") keyFn = kw.second.get();
        }
        std::string cmp;
        if (keyFn && keyFn->kind == EK::Name && keyFn->s == "len")
            cmp = reverse ? "[&](const auto& __a, const auto& __b) { return __a.size() > "
                            "__b.size(); }"
                          : "[&](const auto& __a, const auto& __b) { return __a.size() < "
                            "__b.size(); }";
        else if (keyFn) {
            std::string k = ex(keyFn);
            cmp = reverse ? "[&](const auto& __a, const auto& __b) { return " + k + "(__a) > " +
                                k + "(__b); }"
                          : "[&](const auto& __a, const auto& __b) { return " + k + "(__a) < " +
                                k + "(__b); }";
        } else if (reverse) {
            cmp = "std::greater<>()";
        }
        std::string callTxt = cmp.empty()
                                  ? "std::sort(__v.begin(), __v.end());"
                                  : "std::sort(__v.begin(), __v.end(), " + cmp + ");";
        return cap + "() { " + copy + " " + callTxt + " return __v; }()";
    }
    if (n == "reversed") {
        std::string cap = capture();
        std::string t = typeOf(e->items[0].get());
        std::string v = exP(e->items[0].get());
        if (isStrT(t)) {
            need("string");
            need("algorithm");
            return cap + "() { std::string __s = " + v +
                   "; std::reverse(__s.begin(), __s.end()); return __s; }()";
        }
        need("algorithm");
        return cap + "() { auto __v = " + v +
               "; std::reverse(__v.begin(), __v.end()); return __v; }()";
    }
    if (n == "list") {
        std::string cap = capture();
        need("vector");
        if (e->items.empty()) return "std::vector<long long>{}";
        const Expr* a0 = e->items[0].get();
        if (a0->kind == EK::Call && a0->a && a0->a->kind == EK::Name && a0->a->s == "range") {
            std::string init = "0", cond, step = "1";
            if (a0->items.size() == 1) {
                cond = ex(a0->items[0].get());
            } else if (a0->items.size() == 2) {
                init = ex(a0->items[0].get());
                cond = ex(a0->items[1].get());
            } else if (a0->items.size() >= 3) {
                init = ex(a0->items[0].get());
                cond = ex(a0->items[1].get());
                step = ex(a0->items[2].get());
            } else {
                return "std::vector<long long>{}";
            }
            return cap + "() { std::vector<long long> __v; for (long long __i = " + init +
                   "; (" + step + ") > 0 ? __i < " + cond + " : __i > " + cond + "; __i += " +
                   step + ") __v.push_back(__i); return __v; }()";
        }
        std::string t = typeOf(a0);
        std::string v = exP(a0);
        if (isVecT(t)) return v;
        if (isSetT(t))
            return cap + "() { std::vector<" + elemOf(t) + "> __v(" + v + ".begin(), " + v +
                   ".end()); return __v; }()";
        if (isStrT(t))
            return cap + "() { std::vector<std::string> __v; for (char __c : " + v +
                   ") __v.push_back(std::string(1, __c)); return __v; }()";
        err(e->line, "native mode cannot translate list() for this argument");
        return "0";
    }
    if (n == "tuple") {
        need("tuple");
        if (e->items.empty()) return "std::make_tuple()";
        return "std::make_tuple(" + argList(e) + ")";
    }
    if (n == "set") {
        need("set");
        if (e->items.empty()) return "std::set<long long>{}";
        std::string t = typeOf(e->items[0].get());
        std::string v = exP(e->items[0].get());
        if (isSetT(t)) return v;
        std::string el = elemOf(t);
        if (el.empty()) {
            err(e->line, "native mode needs to know the element type for set()");
            return "{}";
        }
        return "std::set<" + el + ">(" + v + ".begin(), " + v + ".end())";
    }
    if (n == "chr") {
        need("string");
        return "std::string(1, static_cast<char>(" + ex(e->items[0].get()) + "))";
    }
    if (n == "ord") {
        std::string t = typeOf(e->items[0].get());
        std::string v = ex(e->items[0].get());
        return "static_cast<long long>(" + (isStrT(t) ? v + "[0]" : v) + ")";
    }
    if (n == "any" || n == "all") {
        need("algorithm");
        std::string v = exP(e->items[0].get());
        return std::string(n == "any" ? "std::any_of(" : "std::all_of(") + v + ".begin(), " + v +
               ".end(), [](const auto& __x) { return static_cast<bool>(__x); })";
    }
    if (n == "pow") {
        need("cmath");
        return "std::pow(" + argList(e) + ")";
    }
    if (n == "reversed") return ex(e->items[0].get());
    if (n == "type" || n == "isinstance" || n == "id" || n == "hash" || n == "format" ||
        n == "map" || n == "filter" || n == "enumerate" || n == "zip") {
        err(e->line, "`" + n + "()` is not supported in an expression in native mode");
        return "0";
    }
    return n + "(" + argList(e) + ")";
}

std::string Nat::ex(const Expr* e) {
    if (!e) return "";
    switch (e->kind) {
        case EK::IntLit: return std::to_string(e->i);
        case EK::FloatLit: {
            std::ostringstream o;
            o.precision(17);
            o << e->numLit;
            std::string r = o.str();
            if (r.find('.') == std::string::npos && r.find('e') == std::string::npos &&
                r.find("inf") == std::string::npos && r.find("nan") == std::string::npos)
                r += ".0";
            return r;
        }
        case EK::StrLit: return "\"" + escapeCpp(e->s) + "\"";
        case EK::BoolLit: return e->boolLit ? "true" : "false";
        case EK::NoneLit: return "nullptr";
        case EK::Name: return e->s;
        case EK::FStr: return fstrValue(e);
        case EK::ListLit: return listLit(e);
        case EK::SetLit: {
            need("set");
            std::string t = typeOf(e);
            if (t.empty()) t = "std::set<long long>";
            if (e->items.empty()) return t + "{}";
            std::vector<std::string> parts;
            for (const auto& it : e->items) parts.push_back(ex(it.get()));
            return t + "{" + blend(parts, ", ") + "}";
        }
        case EK::DictLit: return dictLit(e);
        case EK::TupleLit: return tupleLit(e);
        case EK::BinOp: return binOp(e);
        case EK::UnaryOp:
            if (e->s == "not") return "!(" + ex(e->a.get()) + ")";
            if (e->s == "-") return "-" + exP(e->a.get());
            if (e->s == "+") return "+" + exP(e->a.get());
            return "~" + exP(e->a.get());
        case EK::BoolOp: {
            std::vector<std::string> parts;
            for (const auto& it : e->items) parts.push_back(exP(it.get()));
            return "(" + blend(parts, e->s == "and" ? " && " : " || ") + ")";
        }
        case EK::Compare: return compare(e);
        case EK::Call: return call(e);
        case EK::Attr: {
            const Expr* obj = e->a.get();
            if (obj && obj->kind == EK::Name && modules.count(obj->s)) {
                const std::string& m = e->s;
                if (obj->s == "math") {
                    // Spell these as plain double literals: the exact value
                    // CPython exposes for math.pi/math.e/math.tau. They need no
                    // header (unlike M_PI, which MSVC only defines with
                    // _USE_MATH_DEFINES), so they compile everywhere.
                    if (m == "pi") return "3.141592653589793";
                    if (m == "e") return "2.718281828459045";
                    if (m == "tau") return "6.283185307179586";
                    if (m == "inf") {
                        need("limits");
                        return "std::numeric_limits<double>::infinity()";
                    }
                    if (m == "nan") {
                        need("limits");
                        return "std::numeric_limits<double>::quiet_NaN()";
                    }
                }
                err(e->line, "native mode does not translate the attribute " + obj->s + "." + m);
                return "0";
            }
            // `requests.get(url).text` (or `.content`): the Response is already
            // modelled as its body string, so the attribute is just the string.
            if ((e->s == "text" || e->s == "content") && obj &&
                isStrT(typeOf(obj)))
                return exP(obj);
            // `Config.version`: a class attribute, accessed through `::` in C++.
            if (obj && obj->kind == EK::Name && klasses.count(obj->s))
                return obj->s + "::" + e->s;
            return exP(obj) + "." + e->s;
        }
        case EK::Subscript: return subscript(e);
        case EK::Slice: return sliceText(e);
        case EK::IfExp:
            return "(" + ex(e->b.get()) + " ? " + ex(e->a.get()) + " : " + ex(e->c.get()) + ")";
        case EK::Lambda: return lambdaExpr(e);
        case EK::ListComp:
        case EK::SetComp:
        case EK::DictComp: return compExpr(e);
        case EK::Starred: return ex(e->a.get());
    }
    return "";
}

std::string Nat::lambdaExpr(const Expr* e) {    std::string sig;
    for (size_t i = 0; i < e->argNames.size(); ++i) {
        if (i) sig += ", ";
        const std::string& a = e->argNames[i];
        if (!a.empty() && a[0] == '*') sig += "auto... " + a.substr(1);
        else sig += "auto " + a;
    }
    return capture() + "(" + sig + ") { return " + ex(e->a.get()) + "; }";
}

std::string Nat::recvText(const Expr* e) {
    std::string v = exP(e);
    if (e && e->kind == EK::StrLit) {
        need("string");
        return "std::string(" + v + ")";
    }
    return v;
}

// ===========================================================================
// statements
// ===========================================================================

std::string Nat::compExpr(const Expr* e) {
    bool isDict = e->kind == EK::DictComp;
    bool isSet = e->kind == EK::SetComp;
    std::string ct = typeOf(e);
    if (ct.empty()) {
        err(e->line, "native mode cannot determine the element type of this comprehension; "
                     "annotate the collections it iterates");
        return "{}";
    }
    std::string cap = capture();
    auto pad = [&](size_t d) {
        return std::string(static_cast<size_t>(ind + static_cast<int>(d)) * 4, ' ');
    };
    std::string s = cap + "() {\n";
    push();
    size_t depth = 1;
    s += pad(1) + ct + " __r;\n";
    for (size_t i = 0; i < e->compTargets.size(); ++i) {
        std::vector<std::string> names = targetNames(e->compTargets[i].get());
        ForPlan plan = planFor(e->compIters[i].get(), names, true);
        if (!plan.ok) {
            err(e->line, plan.why);
            pop();
            return "{}";
        }
        s += pad(depth) + plan.header + " {\n";
        depth++;
        for (const auto& pr : plan.prologue) s += pad(depth) + pr + "\n";
        std::vector<std::string> ts = forTargetTypes(e->compIters[i].get(), names.size());
        for (size_t k = 0; k < names.size() && k < ts.size(); ++k) bind(names[k], ts[k]);
        if (i < e->compIfsNested.size()) {
            for (const auto& c : e->compIfsNested[i]) {
                s += pad(depth) + "if (" + ex(c.get()) + ") {\n";
                depth++;
            }
        }
    }
    if (isDict)
        s += pad(depth) + "__r[" + ex(e->b.get()) + "] = " + ex(e->a.get()) + ";\n";
    else if (isSet)
        s += pad(depth) + "__r.insert(" + ex(e->a.get()) + ");\n";
    else
        s += pad(depth) + "__r.push_back(" + ex(e->a.get()) + ");\n";
    pop();
    while (depth > 1) {
        depth--;
        s += pad(depth) + "}\n";
    }
    s += pad(1) + "return __r;\n";
    s += std::string(static_cast<size_t>(ind) * 4, ' ') + "}()";
    return s;
}

std::vector<std::string> Nat::targetNames(const Expr* t) {
    std::vector<std::string> out;
    if (!t) return out;
    if (t->kind == EK::Name) {
        out.push_back(t->s);
    } else if (t->kind == EK::TupleLit || t->kind == EK::ListLit) {
        for (const auto& it : t->items)
            for (const auto& n : targetNames(it.get())) out.push_back(n);
    } else if (t->kind == EK::Starred) {
        for (const auto& n : targetNames(t->a.get())) out.push_back(n);
    }
    return out;
}

// The Python type of each name bound by `for ... in <iter>`.
std::vector<std::string> Nat::forTargetTypes(const Expr* iter, size_t count) {
    std::vector<std::string> r(count, std::string());
    if (!iter || count == 0) return r;
    auto isCallTo = [](const Expr* e, const char* name) {
        return e && e->kind == EK::Call && e->a && e->a->kind == EK::Name && e->a->s == name;
    };
    if (isCallTo(iter, "range")) {
        for (auto& x : r) x = "long long";
        return r;
    }
    if (isCallTo(iter, "enumerate") && !iter->items.empty()) {
        r[0] = "long long";
        if (count > 1) r[1] = elemOf(typeOf(iter->items[0].get()));
        return r;
    }
    if (isCallTo(iter, "zip") && iter->items.size() >= 2) {
        r[0] = elemOf(typeOf(iter->items[0].get()));
        if (count > 1) r[1] = elemOf(typeOf(iter->items[1].get()));
        return r;
    }
    if (iter->kind == EK::Call && iter->a && iter->a->kind == EK::Attr &&
        iter->a->s == "items" && iter->a->a) {
        std::string t = typeOf(iter->a->a.get());
        r[0] = keyOf(t);
        if (count > 1) r[1] = valOf(t);
        return r;
    }
    std::string t = typeOf(iter);
    if (isStrT(t)) {
        r[0] = "std::string";
        return r;
    }
    if (isVecT(t) || isSetT(t)) {
        std::string el = elemOf(t);
        if (count == 1) {
            r[0] = el;
        } else if (isPairT(el)) {
            auto p = splitTop(tplInner(el));
            if (p.size() == 2) {
                r[0] = p[0];
                r[1] = p[1];
            } else {
                r[0] = el;
            }
        } else {
            r[0] = el;
        }
        return r;
    }
    if (isMapT(t)) {
        r[0] = keyOf(t);
        if (count > 1) r[1] = valOf(t);
        return r;
    }
    return r;
}

ForPlan Nat::planFor(const Expr* iter, const std::vector<std::string>& names, bool declareTargets,
                     bool inlineTarget) {
    ForPlan p;
    if (!iter || names.empty()) {
        p.why = "unsupported for-loop";
        return p;
    }
    auto assign = [&](const std::string& n, const std::string& v) {
        p.prologue.push_back((declareTargets ? "auto " + n : n) + " = " + v + ";");
    };
    auto isCallTo = [](const Expr* e, const char* name) {
        return e && e->kind == EK::Call && e->a && e->a->kind == EK::Name && e->a->s == name;
    };

    if (isCallTo(iter, "range")) {
        if (names.size() != 1) {
            p.why = "`for ... in range()` takes exactly one loop variable";
            return p;
        }
        std::string init = "0", cond, step = "1";
        if (iter->items.empty()) {
            p.why = "range() needs at least one argument";
            return p;
        }
        if (iter->items.size() == 1) {
            cond = ex(iter->items[0].get());
        } else {
            init = ex(iter->items[0].get());
            cond = ex(iter->items[1].get());
            if (iter->items.size() >= 3) step = ex(iter->items[2].get());
        }
        const std::string& v = names[0];
        std::string decl = declareTargets ? "long long " : "";
        if (step == "1") {
            p.header = "for (" + decl + v + " = " + init + "; " + v + " < " + cond + "; ++" + v +
                       ")";
        } else if (step == "-1") {
            p.header = "for (" + decl + v + " = " + init + "; " + v + " > " + cond + "; --" + v +
                       ")";
        } else {
            p.header = "for (" + decl + v + " = " + init + "; (" + step + ") > 0 ? " + v + " < " +
                       cond + " : " + v + " > " + cond + "; " + v + " += " + step + ")";
        }
        p.ok = true;
        return p;
    }

    if (isCallTo(iter, "enumerate") && !iter->items.empty()) {
        if (names.size() != 2) {
            p.why = "`for i, x in enumerate(...)` takes two loop variables";
            return p;
        }
        const Expr* seq = iter->items[0].get();
        std::string sv = recvText(seq), st = typeOf(seq);
        if (seq->kind == EK::Call)
            warn(iter->line, "this expression is re-evaluated on every iteration of the loop");
        p.header = "for (std::size_t __i0 = 0; __i0 < " + sv + ".size(); ++__i0)";
        assign(names[0], "static_cast<long long>(__i0)");
        assign(names[1], isStrT(st) ? sv + ".substr(__i0, 1)" : sv + "[__i0]");
        p.ok = true;
        return p;
    }

    if (isCallTo(iter, "zip") && iter->items.size() >= 2) {
        if (names.size() != iter->items.size()) {
            p.why = "each argument of zip() needs its own loop variable";
            return p;
        }
        need("algorithm");
        std::vector<std::string> sv;
        for (const auto& it : iter->items) sv.push_back(recvText(it.get()));
        std::string bound = sv[0] + ".size()";
        for (size_t i = 1; i < sv.size(); ++i)
            bound = "std::min(" + bound + ", " + sv[i] + ".size())";
        p.header = "for (std::size_t __i0 = 0; __i0 < " + bound + "; ++__i0)";
        for (size_t i = 0; i < sv.size(); ++i) {
            std::string st = typeOf(iter->items[i].get());
            assign(names[i], isStrT(st) ? sv[i] + ".substr(__i0, 1)" : sv[i] + "[__i0]");
        }
        p.ok = true;
        return p;
    }

    if (iter->kind == EK::Call && iter->a && iter->a->kind == EK::Attr &&
        iter->a->s == "items" && iter->a->a) {
        if (names.size() != 2) {
            p.why = "`for k, v in d.items()` takes two loop variables";
            return p;
        }
        std::string dv = exP(iter->a->a.get());
        p.header = "for (const auto& __e0 : " + dv + ")";
        assign(names[0], "__e0.first");
        assign(names[1], "__e0.second");
        p.ok = true;
        return p;
    }

    std::string it = typeOf(iter), iv = recvText(iter);
    if (iv.empty()) {
        p.why = "unsupported for-loop";
        return p;
    }
    if (isStrT(it)) {
        p.header = "for (std::size_t __i0 = 0; __i0 < " + iv + ".size(); ++__i0)";
        assign(names[0], iv + ".substr(__i0, 1)");
        p.ok = true;
        return p;
    }
    if (isVecT(it) || isSetT(it)) {
        std::string el = elemOf(it);
        if (names.size() == 1) {
            if (inlineTarget) {
                // The loop variable is read only inside the loop: declare it
                // directly in the header, exactly as a person would write it.
                p.header = "for (const auto& " + names[0] + " : " + iv + ")";
                p.ok = true;
                return p;
            }
            p.header = "for (const auto& __e0 : " + iv + ")";
            assign(names[0], "__e0");
            p.ok = true;
            return p;
        }
        if (names.size() == 2 && isPairT(el)) {
            p.header = "for (const auto& __e0 : " + iv + ")";
            assign(names[0], "__e0.first");
            assign(names[1], "__e0.second");
            p.ok = true;
            return p;
        }
        p.why = "cannot unpack this element into " + std::to_string(names.size()) + " names";
        return p;
    }
    if (isMapT(it)) {
        noteDictOrder(iter->line);
        p.header = "for (const auto& __e0 : " + iv + ")";
        if (names.size() == 1) {
            assign(names[0], "__e0.first");
        } else if (names.size() == 2) {
            assign(names[0], "__e0.first");
            assign(names[1], "__e0.second");
        } else {
            p.why = "a dict iterates as one or two names";
            return p;
        }
        p.ok = true;
        return p;
    }
    if (names.size() == 1) {
        if (inlineTarget) {
            p.header = "for (const auto& " + names[0] + " : " + iv + ")";
            p.ok = true;
            return p;
        }
        p.header = "for (const auto& __e0 : " + iv + ")";
        assign(names[0], "__e0");
        p.ok = true;
        return p;
    }
    p.why = "native mode does not know what " + iv + " iterates over";
    return p;
}

void Nat::collectBinds(const std::vector<StmtP>& b, std::map<std::string, Bind>& out,
                       std::vector<std::string>* order) {
    auto touch = [&](const std::string& n, const Expr* v, int ln, bool aug) {
        if (out.find(n) == out.end()) {
            Bind nb;
            nb.first = v;
            nb.line = ln;
            nb.aug = aug;
            out[n] = nb;
            if (order) order->push_back(n);
        }
    };
    // A for-loop target gets its type from the iterable. Only remember that if
    // the name is not already a variable of its own (`x = 0` before the loop
    // keeps `x`'s declared type).
    auto setFor = [&](const std::string& n, const Expr* iter, size_t idx, size_t cnt) {
        auto f = out.find(n);
        if (f == out.end()) return;
        if (f->second.first || !f->second.ann.empty() || f->second.forIter) return;
        f->second.forIter = iter;
        f->second.forIdx = idx;
        f->second.forCount = cnt;
    };
    for (const auto& s : b) {
        switch (s->kind) {
            case SK::FuncDef:
            case SK::ClassDef:
            case SK::Import:
            case SK::FromImport:
            case SK::Global:
            case SK::Nonlocal: continue;
            case SK::Assign: {
                const Expr* v = s->values.empty() ? nullptr : s->values[0].get();
                for (const auto& t : s->targets)
                    for (const auto& n : targetNames(t.get())) touch(n, v, s->line, false);
                break;
            }
            case SK::AugAssign: {
                for (const auto& n : targetNames(s->a.get())) touch(n, nullptr, s->line, true);
                break;
            }
            case SK::AnnAssign: {
                std::string annT = s->b ? pythonToCppType(ex(s->b.get())) : std::string();
                for (const auto& n : targetNames(s->a.get())) {
                    touch(n, s->c.get(), s->line, false);
                    auto f = out.find(n);
                    if (f != out.end() && f->second.ann.empty()) f->second.ann = annT;
                }
                break;
            }
            case SK::For: {
                std::vector<std::string> names = targetNames(s->a.get());
                for (size_t i = 0; i < names.size(); ++i) {
                    touch(names[i], nullptr, s->line, false);
                    setFor(names[i], s->iter.get(), i, names.size());
                }
                break;
            }
            default: break;
        }
        collectBinds(s->body, out, order);
        collectBinds(s->orelse, out, order);
        collectBinds(s->finalbody, out, order);
        for (const auto& h : s->handlers) collectBinds(h.body, out, order);
    }
}

// Decide, for every for-loop target in `body`, whether it is read after its
// loop. A target read only inside its own loop can be declared straight in the
// range-for header; one read later (Python keeps the last element bound) must
// keep an explicit declaration that the loop assigns into.
void Nat::markForLeaks(const std::vector<StmtP>& body, std::map<std::string, Bind>& binds) {
    // This set is per-scope: recompute it for the current body rather than
    // accumulating stale names from a previously emitted function.
    leakedForTargets.clear();
    // Count reads of one name in an expression.
    auto readsExpr = [&](auto&& self, const Expr* e, const std::string& name) -> int {
        if (!e) return 0;
        int n = (e->kind == EK::Name && e->s == name) ? 1 : 0;
        n += self(self, e->a.get(), name);
        n += self(self, e->b.get(), name);
        n += self(self, e->c.get(), name);
        n += self(self, e->d.get(), name);
        for (const auto& it : e->items) n += self(self, it.get(), name);
        for (const auto& it : e->compIters) n += self(self, it.get(), name);
        for (const auto& cl : e->compIfsNested)
            for (const auto& it : cl) n += self(self, it.get(), name);
        for (const auto& p : e->kwargs) n += self(self, p.second.get(), name);
        for (const auto& p : e->parts)
            if (p.isExpr) n += self(self, p.expr.get(), name);
        return n;
    };
    // Count reads of one name in a statement subtree.
    std::function<int(const Stmt*, const std::string&)> readsStmt;
    readsStmt = [&](const Stmt* s, const std::string& name) -> int {
        if (!s) return 0;
        int n = 0;
        switch (s->kind) {
            case SK::FuncDef:
            case SK::ClassDef:
                return 0;  // nested scope
            case SK::Assign:
                n += s->values.empty() ? 0 : readsExpr(readsExpr, s->values[0].get(), name);
                break;
            case SK::AugAssign:
                n += readsExpr(readsExpr, s->a.get(), name);
                n += s->b ? readsExpr(readsExpr, s->b.get(), name) : 0;
                break;
            case SK::AnnAssign:
                n += s->c ? readsExpr(readsExpr, s->c.get(), name) : 0;
                break;
            case SK::For:
                n += readsExpr(readsExpr, s->iter.get(), name);
                break;
            case SK::If:
            case SK::While:
                n += readsExpr(readsExpr, s->a.get(), name);
                break;
            case SK::ExprStmt:
            case SK::Return:
                n += readsExpr(readsExpr, s->a.get(), name);
                break;
            default:
                n += readsExpr(readsExpr, s->a.get(), name);
                n += readsExpr(readsExpr, s->b.get(), name);
                n += readsExpr(readsExpr, s->c.get(), name);
                break;
        }
        for (const auto& x : s->body) n += readsStmt(x.get(), name);
        for (const auto& x : s->orelse) n += readsStmt(x.get(), name);
        for (const auto& x : s->finalbody) n += readsStmt(x.get(), name);
        for (const auto& h : s->handlers)
            for (const auto& x : h.body) n += readsStmt(x.get(), name);
        return n;
    };

    // For each for-loop statement, compare the target's reads in the whole body
    // against its reads inside that loop; the difference is "after the loop".
    std::function<void(const std::vector<StmtP>&)> walk;
    walk = [&](const std::vector<StmtP>& b) {
        for (const auto& s : b) {
            if (!s) continue;
            if (s->kind == SK::For && s->a) {
                for (const auto& nm : targetNames(s->a.get())) {
                    auto it = binds.find(nm);
                    if (it == binds.end() || !it->second.forIter) continue;
                    // Reads of nm in the loop's own body (the "inside" part).
                    int inside = 0;
                    for (const auto& x : s->body) inside += readsStmt(x.get(), nm);
                    for (const auto& x : s->orelse) inside += readsStmt(x.get(), nm);
                    // Reads of nm in the whole function/global body.
                    int total = 0;
                    for (const auto& x : body) total += readsStmt(x.get(), nm);
                    if (total > inside) {
                        it->second.forLeaked = true;
                        leakedForTargets.insert(nm);
                    }
                }
            }
            walk(s->body);
            walk(s->orelse);
            walk(s->finalbody);
            for (const auto& h : s->handlers) walk(h.body);
        }
    };
    walk(body);
}

void Nat::declareValue(const std::string& name, const Bind& b) {
    // A declared type that names std::string needs <string>, even though
    // typeOf() no longer has that side effect (so a bare "hi" literal does not
    // pull the header in). Re-establish it here, at the one place a concrete
    // std::string object is actually declared.
    auto declT = [&](const std::string& t) {
        if (t.find("std::string") != std::string::npos) need("string");
    };
    if (!b.ann.empty()) {
        declT(b.ann);
        line(b.ann + " " + name + "{};");
        defaultInitNames.insert(name);
        bind(name, b.ann);
        return;
    }
    if (b.forIter) {
        std::vector<std::string> ts = forTargetTypes(b.forIter, b.forCount);
        std::string ft = b.forIdx < ts.size() ? ts[b.forIdx] : std::string();
        // A single loop variable read only inside its own loop needs no
        // explicit declaration: the range-for header declares it (`const auto&
        // x`). Multi-target loops (for k, v in ...) keep their declarations.
        // Bind its type anyway so the body can resolve it, but emit nothing.
        if (b.forCount == 1 && !b.forLeaked) {
            bind(name, ft);
            return;
        }
        if (!ft.empty()) {
            declT(ft);
            line(ft + " " + name + "{};");
            defaultInitNames.insert(name);
            bind(name, ft);
            return;
        }
        // The iterable's element type could not be named (typically because the
        // iterable is an unannotated parameter, i.e. a template type). Spell the
        // loop variable's type as the iterable's value_type, which resolves once
        // the template is instantiated.
        std::string iv = recvText(b.forIter);
        if (!iv.empty()) {
            line("typename decltype(" + iv + ")::value_type " + name + "{};");
            defaultInitNames.insert(name);
            bind(name, "");
            return;
        }
    }
    std::string t = typeOf(b.first);
    if (!t.empty()) {
        declT(t);
        line(t + " " + name + "{};");
        defaultInitNames.insert(name);
        bind(name, t);
        return;
    }
    if (b.first) {
        need("type_traits");
        line("std::decay_t<decltype(" + ex(b.first) + ")> " + name + "{};");
        defaultInitNames.insert(name);
        bind(name, "");
        return;
    }
    if (b.aug) {
        err(b.line, "'" + name + "' is first used in an augmented assignment; give it an initial "
                     "value or annotate it, e.g. `" + name + ": int = 0`");
        return;
    }
    err(b.line, "native mode cannot work out the type of '" + name +
                    "'; annotate it, e.g. `" + name + ": int = 0`");
}

void Nat::emitLocalDecls(const std::map<std::string, Bind>& binds,
                         const std::vector<std::string>& order) {
    for (const auto& n : order) {
        auto kv = binds.find(n);
        if (kv == binds.end()) continue;
        if (kv->first == "self" || paramNames.count(kv->first)) continue;
        // A closure is not default-constructible, so it is defined where it is
        // assigned instead of being declared here.
        if (kv->second.first && kv->second.first->kind == EK::Call &&
            kv->second.first->a && kv->second.first->a->kind == EK::Name &&
            returnsClosure(kv->second.first->a->s)) {
            inPlaceNames.insert(kv->first);
            continue;
        }
        declareValue(kv->first, kv->second);
    }
}

void Nat::emitBody(const std::vector<StmtP>& b) {
    for (const auto& s : b) emitStmt(s.get());
}

void Nat::emitPrint(const Expr* call) {
    need("iostream");
    std::string sep = "\" \"", end = "\"\\n\"";
    for (const auto& kw : call->kwargs) {
        if (kw.first == "sep") sep = ex(kw.second.get());
        else if (kw.first == "end") end = ex(kw.second.get());
        else warn(call->line, "print() keyword '" + kw.first + "' is ignored in native mode");
    }

    bool guarded = false;
    for (const auto& it : call->items)
        if (exprMayThrow(it.get())) { guarded = true; break; }

    std::string s = "std::cout";
    if (guarded) {
        // Build every argument before writing any of it: if one of them raises,
        // Python has printed nothing, and neither should this. A block keeps the
        // temporaries from colliding across two print statements in one scope.
        line("{");
        ++ind;
        for (size_t i = 0; i < call->items.size(); ++i) {
            const Expr* a = call->items[i].get();
            std::string v = "__v" + std::to_string(i);
            if (a->kind == EK::FStr) {
                // An f-string is a stream chain, not a value; run it into a
                // string first so the format specs survive.
                need("sstream");
                std::string ss = "__ss" + std::to_string(i);
                line("std::ostringstream " + ss + ";");
                line(ss + " << " + outChain(a) + ";");
                line("std::string " + v + " = " + ss + ".str();");
            } else {
                line("auto&& " + v + " = " + outChain(a) + ";");
            }
            if (i) s += " << " + sep;
            s += " << " + v;
        }
        s += " << " + end + ";";
        line(s);
        --ind;
        line("}");
        return;
    }
    for (size_t i = 0; i < call->items.size(); ++i) {
        if (i) s += " << " + sep;
        s += " << " + outChain(call->items[i].get());
    }
    s += " << " + end + ";";
    line(s);
}

// `x = input(...)`, `x = int(input(...))` and friends need a statement, because
// reading a line is not an expression in C++.
bool Nat::emitInputAssign(const Expr* target, const Expr* value) {
    if (!target || target->kind != EK::Name || !value) return false;
    const Expr* inner = value;
    std::string conv;
    if (value->kind == EK::Call && value->a && value->a->kind == EK::Name) {
        const std::string& f = value->a->s;
        if ((f == "int" || f == "float" || f == "str") && !value->items.empty()) {
            const Expr* a0 = value->items[0].get();
            if (a0->kind == EK::Call && a0->a && a0->a->kind == EK::Name && a0->a->s == "input") {
                conv = f;
                inner = a0;
            }
        }
    }
    if (!(inner->kind == EK::Call && inner->a && inner->a->kind == EK::Name &&
          inner->a->s == "input"))
        return false;

    need("iostream");
    need("string");
    std::string prompt = inner->items.empty() ? "\"\"" : ex(inner->items[0].get());
    line("std::cout << " + prompt + " << std::flush;");
    line("std::string __line; std::getline(std::cin, __line);");
    if (conv == "int") {
        line(target->s + " = std::stoll(__line);");
    } else if (conv == "float") {
        line(target->s + " = std::stod(__line);");
    } else {
        line(target->s + " = __line;");
    }
    return true;
}

void Nat::emitAssign(const Stmt* s) {
    if (s->targets.empty()) return;
    const Expr* v = s->values.empty() ? nullptr : s->values[0].get();

    if (s->targets.size() == 1 && emitInputAssign(s->targets[0].get(), v)) return;

    // `lg = make(...)` where make builds a closure: the type has no default
    // constructor, so it is defined here rather than declared up front.
    if (v && s->targets.size() == 1 && s->targets[0]->kind == EK::Name &&
        inPlaceNames.count(s->targets[0]->s) && !inPlaceDone.count(s->targets[0]->s)) {
        line("auto " + s->targets[0]->s + " = " + ex(v) + ";");
        inPlaceDone.insert(s->targets[0]->s);
        bind(s->targets[0]->s, "");
        return;
    }

    // `x = 1 // 0` has no usable value: Python raises, so emit the raise itself.
    // (In an expression the same thing becomes a ?: with a throw-expression.)
    std::string zeroMsg = v && s->targets.size() == 1 ? divZeroMessage(v) : std::string();
    if (!zeroMsg.empty()) {
        need("stdexcept");
        line("throw std::domain_error(\"" + zeroMsg + "\");");
        return;
    }

    std::string tmp;
    if (s->targets.size() > 1 && v) {
        bool trivial = v->kind == EK::IntLit || v->kind == EK::FloatLit ||
                       v->kind == EK::StrLit || v->kind == EK::BoolLit || v->kind == EK::Name;
        if (!trivial) {
            need("type_traits");
            std::string txt = ex(v);
            tmp = "__tmp" + std::to_string(s->line);
            line("std::decay_t<decltype(" + txt + ")> " + tmp + " = " + txt + ";");
        }
    }

    for (const auto& t : s->targets) {
        const Expr* tgt = t.get();
        if (tgt->kind == EK::Name) {
            // `x = 0` / `x = []` right after `x{};` is a no-op the declaration
            // already performed: drop it. Any other value ends the "still
            // default" state, since the name now holds something real.
            if (defaultInitNames.count(tgt->s) && v && isDefaultValue(v) && tmp.empty() &&
                s->targets.size() == 1)
                continue;
            if (defaultInitNames.count(tgt->s)) defaultInitNames.erase(tgt->s);
            line(tgt->s + " = " + (tmp.empty() ? (v ? ex(v) : std::string("{}")) : tmp) + ";");
        } else if (tgt->kind == EK::Subscript) {
            line(elementRef(tgt->a.get(), tgt->b.get(), true) + " = " +
                 (tmp.empty() ? (v ? ex(v) : std::string("{}")) : tmp) + ";");
        } else if (tgt->kind == EK::Attr) {
            line(exP(tgt->a.get()) + "." + tgt->s + " = " +
                 (tmp.empty() ? (v ? ex(v) : std::string("{}")) : tmp) + ";");
        } else if (tgt->kind == EK::TupleLit) {
            if (!v) continue;
            need("tuple");
            // `a, b = x, y` -> std::tie(a, b) = ... when the targets are plain
            // names. But `arr[j], arr[j+1] = arr[j+1], arr[j]` has subscript
            // targets, which std::tie cannot bind: evaluate the whole right-hand
            // side into a temporary tuple first (so every read happens before
            // any write, matching Python's simultaneous assignment), then assign
            // element by element with std::get.
            bool allNames = true;
            for (const auto& it : tgt->items)
                if (it->kind != EK::Name) { allNames = false; break; }
            if (allNames) {
                std::vector<std::string> names = targetNames(tgt);
                line("std::tie(" + blend(names, ", ") + ") = " + ex(v) + ";");
                continue;
            }
            std::string tv = "__tup" + std::to_string(s->line);
            line("auto " + tv + " = " + ex(v) + ";");
            for (size_t k = 0; k < tgt->items.size(); ++k) {
                const Expr* el = tgt->items[k].get();
                std::string rhs = "std::get<" + std::to_string(k) + ">(" + tv + ")";
                if (el->kind == EK::Name) {
                    line(el->s + " = " + rhs + ";");
                } else if (el->kind == EK::Subscript) {
                    line(elementRef(el->a.get(), el->b.get(), true) + " = " + rhs + ";");
                } else if (el->kind == EK::Attr) {
                    line(exP(el->a.get()) + "." + el->s + " = " + rhs + ";");
                } else if (el->kind == EK::Starred) {
                    err(el->line, "native mode does not support *-unpacking into this assignment");
                } else {
                    err(el->line, "native mode does not support this assignment target");
                }
            }
        } else {
            err(s->line, "native mode does not support this assignment target");
        }
    }
}

void Nat::emitAugAssign(const Stmt* s) {
    std::string t = exP(s->a.get());
    std::string v = ex(s->b.get());
    std::string op = s->s;
    if (op.empty() || op.back() != '=') op += "=";
    std::string ta = typeOf(s->a.get()), tv = typeOf(s->b.get());
    // Augmented assignment changes the variable, so a later `x = 0` is a real
    // operation again, not a redundant default.
    if (s->a && s->a->kind == EK::Name) defaultInitNames.erase(s->a->s);

    if (op == "//=") {
        need("cmath");
        if (isIntT(ta) && isIntT(tv))
            line(t + " = static_cast<long long>(std::floor(static_cast<double>(" + t +
                 ") / static_cast<double>(" + v + ")));");
        else
            line(t + " = std::floor(" + t + " / " + v + ");");
        return;
    }
    if (op == "%=") {
        if (isIntT(ta) && isIntT(tv)) {
            line(t + " = ((" + t + " % " + v + ") + " + v + ") % " + v + ";");
        } else {
            need("cmath");
            line(t + " = std::fmod(std::fmod(" + t + ", " + v + ") + " + v + ", " + v + ");");
        }
        return;
    }
    if (op == "**=") {
        need("cmath");
        if (isIntT(ta) && isIntT(tv))
            line(t + " = static_cast<long long>(std::llround(std::pow(static_cast<double>(" + t +
                 "), static_cast<double>(" + v + "))));");
        else
            line(t + " = std::pow(" + t + ", " + v + ");");
        return;
    }
    if (op == "/=" && isIntT(ta) && isIntT(tv)) {
        line(t + " = static_cast<double>(" + t + ") / " + v + ";");
        return;
    }
    if (op == "+=" && isVecT(ta) && tv == ta) {
        line(t + ".insert(" + t + ".end(), " + v + ".begin(), " + v + ".end());");
        return;
    }
    if (op == "*=" && isStrT(ta) && isIntT(tv)) {
        need("string");
        line(t + ".append(" + v + ", std::string());");
        return;
    }
    line(t + " " + op + " " + v + ";");
}

void Nat::emitAnnAssign(const Stmt* s) {
    std::string name;
    if (s->a && s->a->kind == EK::Name) name = s->a->s;
    if (!s->c) return;
    if (name.empty()) {
        line(ex(s->a.get()) + " = " + ex(s->c.get()) + ";");
        return;
    }
    line(name + " = " + ex(s->c.get()) + ";");
}

void Nat::emitTry(const Stmt* s) {
    if (s->handlers.empty()) {
        err(s->line, "try/finally without an `except` has no C++ equivalent in native mode");
        return;
    }
    if (!s->orelse.empty())
        warn(s->line, "the `else` clause of try/except is emitted outside the try block");
    need("stdexcept");
    line("try {");
    ind++;
    defaultInitNames.clear();  // the try body may not complete
    emitBody(s->body);
    ind--;

    std::vector<std::string> types;
    for (const auto& h : s->handlers) {
        std::string name = h.type;
        size_t lp = name.find_first_of("([");
        if (lp != std::string::npos) name = name.substr(0, lp);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        std::string ct;  // empty means a catch-all
        if (name.empty() || name == "Exception" || name == "BaseException" ||
            name == "StopIteration") {
            ct = h.name.empty() ? "" : "std::exception";
        } else if (name == "ValueError" || name == "TypeError") {
            ct = "std::invalid_argument";
        } else if (name == "IndexError" || name == "KeyError") {
            ct = "std::out_of_range";
        } else if (name == "ZeroDivisionError") {
            ct = "std::domain_error";
        } else if (name == "OverflowError" || name == "RuntimeError") {
            ct = "std::runtime_error";
        } else {
            err(s->line, "native mode has no mapping for `except " + name + "`");
            ct = h.name.empty() ? "" : "std::exception";
        }
        types.push_back(ct);
    }
    // `catch (...)` must come last.
    std::vector<size_t> order;
    for (size_t i = 0; i < types.size(); ++i)
        if (!types[i].empty()) order.push_back(i);
    for (size_t i = 0; i < types.size(); ++i)
        if (types[i].empty()) order.push_back(i);
    for (size_t idx : order) {
        const Handler& h = s->handlers[idx];
        if (types[idx].empty()) {
            line("} catch (...) {");
        } else {
            line("} catch (const " + types[idx] + "& __ex) {");
        }
        ind++;
        if (!h.name.empty()) {
            need("string");
            line("std::string " + h.name + " = __ex.what();");
            bind(h.name, "std::string");
        }
        emitBody(h.body);
        ind--;
    }
    line("}");
    if (!s->orelse.empty()) emitBody(s->orelse);
    if (!s->finalbody.empty()) {
        warn(s->line, "the `finally` body runs inline after the try block in native mode, so it "
                      "is skipped if the handler re-throws");
        emitBody(s->finalbody);
    }
}

void Nat::emitFor(const Stmt* s) {
    std::vector<std::string> names = targetNames(s->a.get());
    // A single loop variable that is never read after the loop can be declared
    // directly in the range-for header; otherwise it must have been declared up
    // front (leakedForTargets) and the loop assigns into it.
    bool inlineTarget = names.size() == 1 && !leakedForTargets.count(names[0]);
    ForPlan plan = planFor(s->iter.get(), names, inlineTarget, inlineTarget);
    if (!plan.ok) {
        err(s->line, plan.why);
        return;
    }
    std::string flag;
    if (!s->orelse.empty()) {
        flag = "__brk" + std::to_string(s->line);
        line("bool " + flag + " = false;");
    }
    std::string saved = breakFlag;
    breakFlag = flag;
    line(plan.header + " {");
    ind++;
    for (const auto& p : plan.prologue) line(p);
    // A loop body may run many times (or not at all), so a default assignment
    // inside it is not redundant and must not be dropped.
    defaultInitNames.clear();
    emitBody(s->body);
    ind--;
    line("}");
    breakFlag = saved;
    if (!s->orelse.empty()) {
        line("if (!" + flag + ") {");
        ind++;
        emitBody(s->orelse);
        ind--;
        line("}");
    }
}

void Nat::emitWhile(const Stmt* s) {
    std::string flag;
    if (!s->orelse.empty()) {
        flag = "__brk" + std::to_string(s->line);
        line("bool " + flag + " = false;");
    }
    std::string saved = breakFlag;
    breakFlag = flag;
    line("while (" + ex(s->a.get()) + ") {");
    ind++;
    defaultInitNames.clear();  // loop body runs repeatedly
    emitBody(s->body);
    ind--;
    line("}");
    breakFlag = saved;
    if (!s->orelse.empty()) {
        line("if (!" + flag + ") {");
        ind++;
        emitBody(s->orelse);
        ind--;
        line("}");
    }
}

void Nat::emitIf(const Stmt* s) {
    const Stmt* cur = s;
    bool first = true;
    while (true) {
        line((first ? "if (" : "} else if (") + ex(cur->a.get()) + ") {");
        first = false;
        ind++;
        defaultInitNames.clear();  // a branch may or may not run
        emitBody(cur->body);
        ind--;
        if (!cur->orelse.empty() && cur->orelse.size() == 1 &&
            cur->orelse[0]->kind == SK::If) {
            cur = cur->orelse[0].get();
            continue;
        }
        break;
    }
    if (!cur->orelse.empty()) {
        line("} else {");
        ind++;
        defaultInitNames.clear();
        emitBody(cur->orelse);
        ind--;
    }
    line("}");
}

void Nat::emitStmt(const Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case SK::Comment:
            if (comments) line("// " + s->s);
            return;
        case SK::Pass:
        case SK::Import:
        case SK::FromImport:
        case SK::Global:
        case SK::Nonlocal:
        case SK::ClassDef:
            return;
        case SK::FuncDef:
            // A module-level def is emitted by emitFuncs; one inside a function
            // becomes a lambda, which is what a Python closure is.
            if (inFunction) emitNestedFunc(s);
            return;
        case SK::ExprStmt:
            if (s->a && s->a->kind == EK::Call && s->a->a && s->a->a->kind == EK::Name &&
                s->a->a->s == "print") {
                emitPrint(s->a.get());
                return;
            }
            if (s->a && s->a->kind == EK::Call && s->a->a && s->a->a->kind == EK::Attr) {
                std::string r = exP(s->a->a->a.get());
                std::string m = stmtMethod(s->a.get(), r, s->a->a->s);
                if (!m.empty()) {
                    line(m);
                    return;
                }
            }
            line(ex(s->a.get()) + ";");
            return;
        case SK::Assign: emitAssign(s); return;
        case SK::AugAssign: emitAugAssign(s); return;
        case SK::AnnAssign: emitAnnAssign(s); return;
        case SK::If: emitIf(s); return;
        case SK::While: emitWhile(s); return;
        case SK::For: emitFor(s); return;
        case SK::Return:
            if (s->a) line("return " + ex(s->a.get()) + ";");
            else line("return;");
            return;
        case SK::Break:
            if (!breakFlag.empty()) line("{ " + breakFlag + " = true; break; }");
            else line("break;");
            return;
        case SK::Continue: line("continue;"); return;
        case SK::Try: emitTry(s); return;
        case SK::Raise: {
            need("stdexcept");
            if (!s->a) {
                line("throw;");
                return;
            }
            const Expr* e = s->a.get();
            std::string ctor = "std::runtime_error";
            std::string msg = "\"\"";
            if (e->kind == EK::Call) {
                std::string cn = (e->a && e->a->kind == EK::Name) ? e->a->s : "";
                if (cn == "ValueError" || cn == "TypeError") ctor = "std::invalid_argument";
                else if (cn == "IndexError" || cn == "KeyError") ctor = "std::out_of_range";
                else if (cn == "ZeroDivisionError") ctor = "std::domain_error";
                if (!e->items.empty()) msg = ex(e->items[0].get());
            } else if (e->kind == EK::Name) {
                err(s->line, "`raise " + e->s + "` needs an explicit message in native mode");
            }
            line("throw " + ctor + "(" + msg + ");");
            return;
        }
        case SK::Assert:
            need("stdexcept");
            if (s->a)
                line("if (!(" + ex(s->a.get()) +
                     ")) throw std::runtime_error(\"assertion failed\");");
            return;
        case SK::Del:
            err(s->line, "`del` is not translated in native mode");
            return;
        case SK::With:
            err(s->line, "`with` is not translated in native mode (no C++ context managers)");
            return;
    }
}

// ===========================================================================
// functions and classes
// ===========================================================================

// Returns the parameter list; `tmpl` receives the template header (no newline).
// `typeKey` is the funcArgTypes lookup key: the function/method name for a
// function, or `Class.method` for a class method (whose emitted name is just
// `method`, which would otherwise not find its call-site types).
std::string Nat::signature(const Stmt* f, const std::string& cxxName, bool withDefaults,
                           std::string& tmpl, const std::string& typeKey,
                           bool allowTemplate) {
    std::vector<std::string> tps, ps;
    size_t ti = 0;   // template-parameter counter (counts *args packs too)
    size_t pi = 0;   // ordinary-parameter index, matches funcArgTypes[] positions
    const std::string& key = typeKey.empty() ? cxxName : typeKey;
    // The argument types seen at the call sites, if every site agreed on one.
    auto argTy = [&](size_t idx) -> std::string {
        auto it = funcArgTypes.find(key);
        if (it == funcArgTypes.end() || idx >= it->second.size()) return "";
        const std::string& t = it->second[idx];
        return (t.empty() || t == "\x01") ? "" : t;
    };
    for (const auto& p : f->params) {
        if (p.name == "self") continue;
        if (p.isKwStar) {
            err(f->line, "`**" + p.name + "` cannot be modelled in native mode");
            continue;
        }
        if (p.isStar) {
            std::string tn = "__A" + std::to_string(ti++);
            tps.push_back("class... " + tn);
            ps.push_back(tn + "... " + p.name);
            continue;
        }
        if (p.def) {
            std::string t = typeOf(p.def.get());
            if (t.empty()) t = "long long";
            if (t.find("std::string") != std::string::npos) need("string");
            // A mutable container parameter (list/dict/set) is modified in place
            // by Python; pass it by reference so the mutation reaches the caller.
            if (bodyMutates(f->body, p.name) && isContainerT(t)) t += "&";
            ps.push_back(t + " " + p.name +
                         (withDefaults ? " = " + ex(p.def.get()) : std::string()));
            pi++;
            continue;
        }
        // No annotation or default. Prefer a concrete type agreed on by every
        // call site; only fall back to a template parameter when the sites
        // disagree or the type is unknown (so the code stays readable instead of
        // instantiating a template for every distinct argument type).
        std::string concrete = argTy(pi);
        pi++;
        if (!concrete.empty()) {
            if (concrete.find("std::string") != std::string::npos) need("string");
            bool mut = bodyMutates(f->body, p.name);
            ps.push_back(concrete + (mut && isContainerT(concrete) ? "&" : "") + " " + p.name);
            continue;
        }
        // Constructors (and methods) must not become templates: when the type is
        // unknown, fall back to a plain `long long` so `Point(...)` stays a real
        // constructor rather than a `template <...> Point(...)`.
        if (!allowTemplate) {
            ps.push_back(std::string("long long ") + p.name);
            continue;
        }
        std::string tn = "T" + std::to_string(ti++);
        tps.push_back("class " + tn);
        // Without a usable type we fall back to a template parameter. If the
        // body mutates it in place it must be a reference, or the caller's
        // container is left untouched.
        bool mut = bodyMutates(f->body, p.name);
        ps.push_back(tn + (mut ? "&" : "") + " " + p.name);
    }
    tmpl = tps.empty() ? "" : "template <" + blend(tps, ", ") + ">";
    return cxxName + "(" + blend(ps, ", ") + ")";
}

void Nat::emitFuncs() {
    if (funcOrder.empty()) return;
    // Default arguments go on the forward declaration only: C++ forbids adding
    // them to a function template that has already been declared.
    for (const auto& n : funcOrder) {
        std::string tmpl;
        std::string sig = signature(funcs[n], n, true, tmpl);
        if (!tmpl.empty()) line(tmpl);
        line(returnType(n) + " " + sig + ";");
    }
    blank();
    for (const auto& n : funcOrder) {
        const Stmt* f = funcs[n];
        std::string tmpl;
        std::string sig = signature(f, n, false, tmpl);
        if (!tmpl.empty()) line(tmpl);
        line(returnType(n) + " " + sig + " {");
        ind++;
        inFunction = true;
        push();
        paramNames.clear();
        defaultInitNames.clear();  // per-function: no stale names from elsewhere
        size_t pi = 0;  // ordinary-parameter index, matching funcArgTypes[]
        for (const auto& p : f->params) {
            if (p.name == "self" || p.isKwStar) continue;
            if (p.isStar) continue;
            bind(p.name, paramTypeWithCallSite(n, pi, p, f->body));
            paramNames.insert(p.name);
            pi++;
        }
        std::map<std::string, Bind> binds;
        std::vector<std::string> border;
        collectBinds(f->body, binds, &border);
        markForLeaks(f->body, binds);
        emitLocalDecls(binds, border);
        emitBody(f->body);
        pop();
        inFunction = false;
        ind--;
        line("}");
        blank();
    }
}

// Argument types of every `ClassName(...)` call, used to give the constructor
// parameters concrete types and, through them, the member types.
void Nat::collectCtorArgs(const Expr* e,
                          std::map<std::string, std::vector<std::string>>& out) {
    if (!e) return;
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Name && klasses.count(e->a->s)) {
        std::vector<std::string> ts;
        for (const auto& it : e->items) ts.push_back(typeOf(it.get()));
        auto& slot = out[e->a->s];
        if (slot.size() < ts.size()) slot.resize(ts.size());
        for (size_t i = 0; i < ts.size(); ++i)
            if (slot[i].empty()) slot[i] = ts[i];
    }
    collectCtorArgs(e->a.get(), out);
    collectCtorArgs(e->b.get(), out);
    collectCtorArgs(e->c.get(), out);
    collectCtorArgs(e->d.get(), out);
    for (const auto& it : e->items) collectCtorArgs(it.get(), out);
    for (const auto& it : e->compIters) collectCtorArgs(it.get(), out);
    for (const auto& it : e->compTargets) collectCtorArgs(it.get(), out);
    for (const auto& p : e->kwargs) collectCtorArgs(p.second.get(), out);
    for (const auto& p : e->parts)
        if (p.isExpr) collectCtorArgs(p.expr.get(), out);
}

void Nat::collectCtorArgsStmt(const Stmt* s,
                              std::map<std::string, std::vector<std::string>>& out) {
    if (!s) return;
    collectCtorArgs(s->a.get(), out);
    collectCtorArgs(s->b.get(), out);
    collectCtorArgs(s->c.get(), out);
    collectCtorArgs(s->iter.get(), out);
    for (const auto& t : s->targets) collectCtorArgs(t.get(), out);
    for (const auto& t : s->values) collectCtorArgs(t.get(), out);
    for (const auto& x : s->body) collectCtorArgsStmt(x.get(), out);
    for (const auto& x : s->orelse) collectCtorArgsStmt(x.get(), out);
    for (const auto& x : s->finalbody) collectCtorArgsStmt(x.get(), out);
    for (const auto& h : s->handlers)
        for (const auto& x : h.body) collectCtorArgsStmt(x.get(), out);
}

// Argument types of every `f(...)` call to a user-defined function. Used to
// specialise the function's signature: when a parameter is passed the same
// type at every call site, it is spelled concretely rather than left as a
// template parameter. Only positional arguments contribute (keyword arguments
// are emitted as such and are not resolved against a position here); a call
// whose argument type cannot be resolved leaves that slot empty rather than
// forcing a wrong specialisation.
void Nat::collectFuncCallArgs(const Expr* e,
                              std::map<std::string, std::vector<std::string>>& out) {
    if (!e) return;
    // Both user functions and user-class constructors: `f(...)` and `Point(...)`
    // are collected under the same name so signature() can specialise them the
    // same way. (The constructor is emitted as `Point(args)`, whose name matches
    // the class, so `funcArgTypes["Point"]` supplies its parameter types.)
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Name &&
        (funcs.count(e->a->s) || klasses.count(e->a->s))) {
        auto& slot = out[e->a->s];
        for (size_t i = 0; i < e->items.size(); ++i) {
            std::string t = typeOf(e->items[i].get());
            if (t.empty()) continue;  // unknown here; a known site may still win
            if (slot.size() <= i) slot.resize(i + 1);
            if (slot[i].empty()) slot[i] = t;
            else if (slot[i] != t) slot[i] = "\x01";  // conflicting: force generic
        }
    }
    // A method call on a user-class instance: keyed by `Class.method` so the
    // method's parameters can be specialised the same way as functions.
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Attr && e->a->a) {
        std::string rt = typeOf(e->a->a.get());
        if (!rt.empty() && klasses.count(rt)) {
            auto& slot = out[rt + "." + e->a->s];
            for (size_t i = 0; i < e->items.size(); ++i) {
                std::string t = typeOf(e->items[i].get());
                if (t.empty()) continue;
                if (slot.size() <= i) slot.resize(i + 1);
                if (slot[i].empty()) slot[i] = t;
                else if (slot[i] != t) slot[i] = "\x01";
            }
        }
    }
    collectFuncCallArgs(e->a.get(), out);
    collectFuncCallArgs(e->b.get(), out);
    collectFuncCallArgs(e->c.get(), out);
    collectFuncCallArgs(e->d.get(), out);
    for (const auto& it : e->items) collectFuncCallArgs(it.get(), out);
    for (const auto& it : e->compIters) collectFuncCallArgs(it.get(), out);
    for (const auto& it : e->compTargets) collectFuncCallArgs(it.get(), out);
    for (const auto& p : e->kwargs) collectFuncCallArgs(p.second.get(), out);
    for (const auto& p : e->parts)
        if (p.isExpr) collectFuncCallArgs(p.expr.get(), out);
}

void Nat::collectFuncCallArgsStmt(const Stmt* s,
                                  std::map<std::string, std::vector<std::string>>& out) {
    if (!s) return;
    // Call sites inside a function body need that function's parameter types,
    // which are not known at collection time (they are the very types we are
    // trying to infer). Only module-level (main-body) call sites are collected,
    // where every argument's type is already resolved.
    if (s->kind == SK::FuncDef || s->kind == SK::ClassDef) return;
    collectFuncCallArgs(s->a.get(), out);
    collectFuncCallArgs(s->b.get(), out);
    collectFuncCallArgs(s->c.get(), out);
    collectFuncCallArgs(s->iter.get(), out);
    for (const auto& t : s->targets) collectFuncCallArgs(t.get(), out);
    for (const auto& t : s->values) collectFuncCallArgs(t.get(), out);
    for (const auto& x : s->body) collectFuncCallArgsStmt(x.get(), out);
    for (const auto& x : s->orelse) collectFuncCallArgsStmt(x.get(), out);
    for (const auto& x : s->finalbody) collectFuncCallArgsStmt(x.get(), out);
    for (const auto& h : s->handlers)
        for (const auto& x : h.body) collectFuncCallArgsStmt(x.get(), out);
}

// Walk a sequence of statements, resolving a call's argument types with the
// current local/parameter bindings and recording `name = expr` so later calls
// in the same body can see the type of `name`. This is what lets `make_pair`
// return a `Pair`, and `swap_pair(p)` see `p` as a `Pair`, without any
// annotation: the caller's `p = make_pair(1, 2)` propagates the type in.
void Nat::propagateCallTypes(const std::vector<StmtP>& body) {
    for (const auto& sp : body) {
        const Stmt* s = sp.get();
        if (!s) continue;
        if (s->kind == SK::ClassDef) continue;
        if (s->kind == SK::FuncDef) {
            // Enter the function body with its parameters bound to whatever
            // funcArgTypes already knows (annotations/defaults too), so that a
            // `Pair(x, y)` built from parameters, or a method call on a
            // parameter, resolves to a concrete type.
            push();
            size_t pi = 0;
            for (const auto& p : s->params) {
                if (p.name == "self" || p.isKwStar) continue;
                if (p.isStar) {
                    pi++;
                    continue;
                }
                std::string t;
                if (!p.annotation.empty()) t = pythonToCppType(p.annotation);
                else if (p.def) t = typeOf(p.def.get());
                else {
                    auto it = funcArgTypes.find(s->s);
                    if (it != funcArgTypes.end() && pi < it->second.size() &&
                        !it->second[pi].empty() && it->second[pi] != "\x01")
                        t = it->second[pi];
                }
                if (!t.empty()) bind(p.name, t);
                pi++;
            }
            propagateCallTypes(s->body);
            pop();
            continue;
        }
        propagateCallTypesStmt(s);
    }
}

void Nat::propagateCallTypesStmt(const Stmt* s) {
    if (!s) return;
    // `name = expr` (and annotated assignments) give later statements the type
    // of `name`, so `p = make_pair(1, 2)` lets `swap_pair(p)` see `p` as a Pair.
    if (s->kind == SK::Assign && s->targets.size() == 1 && !s->values.empty()) {
        std::string t = typeOf(s->values[0].get());
        if (!t.empty())
            for (const auto& n : targetNames(s->targets[0].get())) bind(n, t);
    } else if (s->kind == SK::AnnAssign && s->a) {
        std::string t = s->b ? pythonToCppType(ex(s->b.get())) : typeOf(s->c.get());
        if (!t.empty())
            for (const auto& n : targetNames(s->a.get())) bind(n, t);
    }
    // Collect the call sites reachable from this statement's expressions (with
    // the now-resolved locals), then recurse into nested blocks. Constructor
    // calls (`Pair(x, y)`) also feed ctorArgs so emitClass can type the members.
    collectFuncCallArgs(s->a.get(), funcArgTypes);
    collectFuncCallArgs(s->b.get(), funcArgTypes);
    collectFuncCallArgs(s->c.get(), funcArgTypes);
    collectFuncCallArgs(s->iter.get(), funcArgTypes);
    for (const auto& t : s->targets) collectFuncCallArgs(t.get(), funcArgTypes);
    for (const auto& v : s->values) collectFuncCallArgs(v.get(), funcArgTypes);
    collectCtorArgs(s->a.get(), ctorArgs);
    collectCtorArgs(s->b.get(), ctorArgs);
    collectCtorArgs(s->c.get(), ctorArgs);
    collectCtorArgs(s->iter.get(), ctorArgs);
    for (const auto& t : s->targets) collectCtorArgs(t.get(), ctorArgs);
    for (const auto& v : s->values) collectCtorArgs(v.get(), ctorArgs);
    propagateCallTypes(s->body);
    propagateCallTypes(s->orelse);
    propagateCallTypes(s->finalbody);
    for (const auto& h : s->handlers) propagateCallTypes(h.body);
}

void Nat::emitClass(const std::string& name, const Stmt* c) {
    if (!c->bases.empty())
        err(c->line, "native mode does not translate inheritance (`class " + name + "(" +
                         c->bases[0] + ")`); use the default mode instead");

    const Stmt* init = nullptr;
    std::vector<const Stmt*> methods;
    // Class-level assignments (`version = 3`, `name = "demo"`) become static
    // members shared by every instance, matching Python's class attributes.
    std::vector<std::string> staticOrder;
    std::map<std::string, std::string> staticType;
    std::map<std::string, const Expr*> staticInit;
    for (const auto& m : c->body) {
        if (m->kind == SK::FuncDef) {
            if (m->s == "__init__") init = m.get();
            else methods.push_back(m.get());
        } else if (m->kind == SK::Assign && m->targets.size() == 1 &&
                   m->targets[0]->kind == EK::Name && !m->values.empty()) {
            std::string t = typeOf(m->values[0].get());
            if (!t.empty()) {
                if (!staticType.count(m->targets[0]->s)) staticOrder.push_back(m->targets[0]->s);
                staticType[m->targets[0]->s] = t;
                staticInit[m->targets[0]->s] = m->values[0].get();
            }
        } else if (m->kind == SK::AnnAssign && m->a && m->a->kind == EK::Name && m->b) {
            std::string t = pythonToCppType(ex(m->b.get()));
            if (!t.empty()) {
                if (!staticType.count(m->a->s)) staticOrder.push_back(m->a->s);
                staticType[m->a->s] = t;
                staticInit[m->a->s] = m->c.get();
            }
        }
    }
    if (!init && !c->bases.empty()) return;

    // Parameter types come from the constructor call sites.
    std::map<std::string, std::string> ctorParamTy;
    if (init) {
        auto f = ctorArgs.find(name);
        if (f != ctorArgs.end()) {
            size_t idx = 0;
            for (const auto& p : init->params) {
                if (p.name == "self") continue;
                if (idx < f->second.size() && !f->second[idx].empty())
                    ctorParamTy[p.name] = f->second[idx];
                idx++;
            }
        }
    }

    push();
    for (const auto& kv : ctorParamTy) bind(kv.first, kv.second);

    std::vector<std::string> memberOrder;
    std::map<std::string, std::string> memberType;
    if (init) {
        for (const auto& st : init->body) {
            const Expr* tgt = nullptr;
            const Expr* val = nullptr;
            std::string ann;
            if (st->kind == SK::Assign && st->targets.size() == 1) {
                tgt = st->targets[0].get();
                val = st->values.empty() ? nullptr : st->values[0].get();
            } else if (st->kind == SK::AnnAssign) {
                tgt = st->a.get();
                val = st->c.get();
                if (st->b) ann = pythonToCppType(ex(st->b.get()));
            }
            if (!tgt || tgt->kind != EK::Attr || !tgt->a || tgt->a->kind != EK::Name ||
                tgt->a->s != "self")
                continue;
            std::string mt = !ann.empty() ? ann : typeOf(val);
            if (mt.empty()) {
                err(st->line, "native mode cannot work out the type of `self." + tgt->s +
                                  "`; annotate it, e.g. `self." + tgt->s + ": int = 0`");
                mt = "long long";
            }
            if (!memberType.count(tgt->s)) memberOrder.push_back(tgt->s);
            memberType[tgt->s] = mt;
        }
    }
    // Publish the resolved member types so typeOf() can resolve `a.x` from any
    // scope (a module-level function taking `Point`, a method, main, ...).
    classMembers[name] = memberType;

    line("struct " + name + " {");
    ind++;
    // A user-defined constructor with parameters suppresses the implicit default
    // constructor, but a module-level `Point p{};` still needs one. Provide it
    // explicitly unless `__init__` itself takes no arguments (in which case the
    // generated constructor already is the default constructor).
    // `= default` is only needed when some non-self parameter has *no* default
    // value: such a constructor can't be called as `Point()`, yet a default
    // constructor is still needed for `Point p{}`. If every parameter has a
    // default (e.g. `def __init__(self, start=0)`), the generated constructor
    // already doubles as the default one and `= default` would be ambiguous.
    bool ctorHasParams = init && std::any_of(init->params.begin(), init->params.end(),
                                             [](const Param& p) { return p.name != "self"; });
    if (ctorHasParams &&
        std::any_of(init->params.begin(), init->params.end(),
                    [](const Param& p) { return p.name != "self" && !p.def; }))
        line(name + "() = default;");
    for (const auto& m : memberOrder) line(memberType[m] + " " + m + "{};");
    if (!memberOrder.empty()) blank();
    // Class attributes are shared state: `static` members with an in-class
    // initialiser (C++17 inline static), so `Config.version` reads work.
    for (const auto& m : staticOrder) {
        auto it = staticInit.find(m);
        std::string init = (it != staticInit.end() && it->second) ? ex(it->second) : "{}";
        line("static inline " + staticType[m] + " " + m + " = " + init + ";");
    }
    if (!staticOrder.empty()) blank();

    auto emitMember = [&](const Stmt* f, const std::string& cxxName, bool isCtor,
                          const std::string& typeKey) {
        std::string tmpl;
        std::string sig = signature(f, cxxName, true, tmpl, typeKey, false);
        if (!tmpl.empty()) line(tmpl);
        line((isCtor ? std::string() : "auto ") + sig + " {");
        ind++;
        bool savedFn = inFunction;
        inFunction = true;
        push();
        // `self` refers to the object, exactly as in Python.
        line("auto& self = *this;");
        // Bind `self` (the class type) and every member's type so `self.area()`
        // and `self.x` (and methods on it) resolve inside the body.
        bind("self", name);
        for (const auto& kv : memberType) bind("self." + kv.first, kv.second);
        paramNames.clear();
        size_t pi = 0;  // ordinary-parameter index, matching funcArgTypes[]
        for (const auto& p : f->params) {
            if (p.name == "self" || p.isKwStar) continue;
            bind(p.name, paramTypeWithCallSite(typeKey, pi, p, f->body));
            paramNames.insert(p.name);
            pi++;
        }
        std::map<std::string, Bind> binds;
        std::vector<std::string> border;
        collectBinds(f->body, binds, &border);
        markForLeaks(f->body, binds);
        emitLocalDecls(binds, border);
        emitBody(f->body);
        pop();
        inFunction = savedFn;
        ind--;
        line("}");
    };

    if (init) emitMember(init, name, true, name);
    for (const auto* m : methods) emitMember(m, m->s, false, name + "." + m->s);

    ind--;
    line("};");
    blank();
    pop();
}

// ===========================================================================
// driver
// ===========================================================================

NativeResult Nat::run() {
    push();  // the global scope; functions and main() nest inside it
    for (const auto& s : prog) {
        if (s->kind == SK::FuncDef) {
            funcs[s->s] = s.get();
            funcOrder.push_back(s->s);
        } else if (s->kind == SK::ClassDef) {
            klasses[s->s] = s.get();
            classOrder.push_back(s->s);
        } else if (s->kind == SK::Import) {
            for (const auto& im : s->imports)
                modules.insert(im.second.empty() ? im.first : im.second);
        } else if (s->kind == SK::FromImport) {
            modules.insert(s->s);
        }
    }
    for (const auto& s : prog) collectCtorArgsStmt(s.get(), ctorArgs);

    std::map<std::string, Bind> globals;
    std::vector<std::string> gorder;
    collectBinds(prog, globals, &gorder);
    markForLeaks(prog, globals);
    for (const auto& n : gorder) bind(n, "");
    for (const auto& n : gorder) {
        auto f = globals.find(n);
        if (f != globals.end()) bind(n, typeOf(f->second.first));
    }
    // Call-site argument types are collected *after* the globals are bound, so
    // `bubble_sort(data)` can see that `data` is a std::vector<long long> and
    // specialise the signature. (Call sites inside function bodies are handled
    // separately during emitFuncs.)
    for (const auto& s : prog) collectFuncCallArgsStmt(s.get(), funcArgTypes);

    // Iteratively propagate types across functions until fixpoint: a function
    // that builds and returns an object (`return Pair(x, y)`) gives the class
    // its constructor-parameter types, and a function that receives that object
    // (`swap_pair(p)`) sees `p` as the class. Each pass re-derives the return
    // types from the newly-seen argument types, so chains of calls settle.
    for (int pass = 0; pass < 16; ++pass) {
        size_t beforeArgs = funcArgTypes.size();
        size_t beforeCtor = ctorArgs.size();
        // Re-derive every return type from the argument types known so far (a
        // cleared memo means `return Pair(x, y)` re-evaluates `x`/`y` with the
        // types the previous pass discovered).
        funcRet.clear();
        push();  // module scope for the propagation walk
        propagateCallTypes(prog);
        pop();
        if (funcArgTypes.size() == beforeArgs && ctorArgs.size() == beforeCtor) break;
    }

    // Class structs are emitted first, before any function or global that might
    // reference them: a function `dist_sq(Point a, Point b)` and a global
    // `Point p{}` both need `struct Point` to already exist. (The methods' own
    // bodies are emitted here too; they reference only `self`, so they are
    // safe before the module-level functions and globals.)
    for (const auto& cn : classOrder) emitClass(cn, klasses[cn]);

    // Global variables must be declared *before* the functions that reference
    // them, or a function body reading a module-level name fails to compile.
    // Python resolves names at call time; C++ resolves them at definition site.
    bool anyGlobal = false;
    for (const auto& n : gorder) {
        auto f = globals.find(n);
        if (f == globals.end()) continue;
        if (funcs.count(n) || klasses.count(n) || modules.count(n)) continue;
        if (f->second.first && f->second.first->kind == EK::Call && f->second.first->a &&
            f->second.first->a->kind == EK::Name && returnsClosure(f->second.first->a->s)) {
            inPlaceNames.insert(n);  // defined inside main() instead
            continue;
        }
        anyGlobal = true;
        declareValue(n, f->second);
    }
    if (anyGlobal) blank();

    // Save the globals that are still default-initialised; emitFuncs() clears
    // defaultInitNames per function, and main()'s body should still be able to
    // drop a redundant `scores = {}` right after the global declaration.
    std::set<std::string> globalsStillDefault = defaultInitNames;
    emitFuncs();
    defaultInitNames = globalsStillDefault;

    line("int main() {");
    ind++;
    inFunction = true;
    push();
    // std::rand() is deterministic unless seeded. Detect random usage up front
    // (it can sit inside a loop/function/global, all of which emit *after* this
    // point) and seed once at the top of main(), so every run starts fresh --
    // matching Python, where each process starts random. A user's explicit
    // random.seed(...) still runs later and takes over.
    if (usesRandom || bodyUsesRandom(prog)) {
        usesRandom = true;
        need("ctime");
        line("std::srand(static_cast<unsigned>(std::time(nullptr)));");
    }
    for (const auto& s : prog) {
        if (s->kind == SK::FuncDef || s->kind == SK::ClassDef || s->kind == SK::Import ||
            s->kind == SK::FromImport)
            continue;
        emitStmt(s.get());
    }
    pop();
    inFunction = false;
    ind--;
    line("}");

    pop();  // global scope

    NativeResult r;
    r.ok = errors.empty();
    if (r.ok) {
        std::string helpers = reprHelpers();  // adds to `inc`, so before the includes
        std::string decls = reprHelperDecls();
        // Last line of defence: scan the emitted code for constructs that need a
        // header but whose need() call lives in a spot that only runs for some
        // inputs (e.g. a std::string element inside a std::vector). A bare "hi"
        // literal is a plain const char*, so it must NOT pull <string> in.
        std::string all = body.str() + decls + helpers;
        auto hasTok = [&](const char* tok) { return all.find(tok) != std::string::npos; };
        if (hasTok("std::string") || hasTok("std::to_string") || hasTok("std::stoll") ||
            hasTok("std::stod") || hasTok("std::stoi") || hasTok("std::getline") ||
            hasTok("std::ostringstream") || hasTok("std::istringstream") ||
            hasTok("std::stringstream"))
            inc.insert("string");
        std::ostringstream out;
        out << "// generated by p2cpp --native: direct C++, standard library only\n";
        for (const auto& h : inc) out << "#include <" << h << ">\n";
        out << "\n";
        out << decls;
        out << body.str();  // globals, functions and main() -- the program itself
        if (!helpers.empty()) out << "\n" << helpers;  // kept out of the way
        r.code = out.str();
    }
    r.warnings = warnings;
    r.errors = errors;
    return r;
}

}  // namespace

NativeResult generateNative(const std::vector<StmtP>& program, bool emitComments) {
    Nat n(program, emitComments);
    return n.run();
}
