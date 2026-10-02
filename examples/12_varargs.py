# 12_varargs.py -- *args, callables as values, higher-order functions

def add_all(*nums):
    total = 0
    for n in nums:
        total = total + n
    return total


def describe(*args, **kwargs):
    return (len(args), len(kwargs))


def apply_twice(f, x):
    return f(f(x))


def compose(f, g):
    return lambda x: f(g(x))


def fact(n):
    if n < 2:
        return 1
    return n * fact(n - 1)


inc = lambda x: x + 1
dbl = lambda x: x * 2

print("add_all  :", add_all(1, 2, 3, 4))
print("add_all  :", add_all())
print("describe :", describe(1, 2, 3))
print("twice    :", apply_twice(lambda v: v * 3, 2))
print("inc/dbl  :", inc(10), dbl(10))
print("compose  :", compose(inc, dbl)(5))
print("fact     :", fact(6))
print("map      :", list(map(dbl, [1, 2, 3])))
print("filter   :", list(filter(lambda v: v % 2 == 0, range(10))))
print("sum gen  :", sum(dbl(n) for n in range(5)))
