# 02_fizzbuzz.py -- loops, conditionals, functions


def fizzbuzz(n):
    for i in range(1, n + 1):
        if i % 15 == 0:
            print("FizzBuzz")
        elif i % 3 == 0:
            print("Fizz")
        elif i % 5 == 0:
            print("Buzz")
        else:
            print(i)


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def factorial(n, acc=1):
    if n <= 1:
        return acc
    return factorial(n - 1, acc * n)


def gcd(a, b):
    while b != 0:
        a, b = b, a % b
    return a


fizzbuzz(20)
print("----")
for k in range(10):
    print(k, fib(k))
print("----")
print("10! =", factorial(10))
print("gcd(1071, 462) =", gcd(1071, 462))
