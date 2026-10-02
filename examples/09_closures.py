# defaults, closures, comprehensions
def make(prefix=">>"):
    def log(msg, level="INFO"):
        return f"[{level}] {prefix} {msg}"
    return log

lg = make("!")
print(lg("hello"))
print(lg("warn", level="WARN"))

def stats(xs):
    return {"n": len(xs), "total": sum(xs), "doubled": sum(xs) * 2}

print(stats([3, 1, 4, 1, 5]))
squares = {x: x ** 2 for x in range(1, 5)}
print(squares)
print(sorted(squares.items()))
names = ["bob", "al", "carol"]
print(sorted(names, key=len))
print(max(names, key=len))
