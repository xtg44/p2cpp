# 06_functions.py -- defaults, *args, nested functions, lambdas, recursion

def greet(name, greeting="Hello"):
    return greeting + ", " + name + "!"


print(greet("World"))
print(greet("World", "Hi"))


def add_all(*nums):
    total = 0
    for n in nums:
        total = total + n
    return total


print(add_all(1, 2, 3, 4))


def apply_twice(f, x):
    return f(f(x))


def double(x):
    return x * 2


print(apply_twice(double, 5))
print(apply_twice(lambda x: x + 3, 10))


def make_adder(n):
    def adder(x):
        return x + n
    return adder


add10 = make_adder(10)
print(add10(7))


def factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)


def is_even(n):
    if n == 0:
        return True
    return is_odd(n - 1)


def is_odd(n):
    if n == 0:
        return False
    return is_even(n - 1)


print(factorial(6))
print(is_even(10), is_odd(7))

nums = [4, 1, 3, 2]
print(sorted(nums))
print(sorted(nums, reverse=True))
print(sorted(["banana", "kiwi", "apple"], key=len))
print(list(map(double, nums)))
print([x * x for x in nums])
print(sum(x for x in nums))
