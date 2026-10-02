# 08_control.py -- while, try/except, break/continue, nested loops

n = 0
while n < 5:
    n = n + 1
print("while    :", n)

total = 0
i = 0
while True:
    i = i + 1
    if i > 10:
        break
    if i % 2 == 1:
        continue
    total = total + i
print("continue :", total)

for a in range(1, 4):
    for b in range(1, 4):
        if a * b == 6:
            print("found    :", a, b)

found = -1
for v in [4, 8, 15, 16]:
    if v % 5 == 0:
        found = v
        break
else:
    found = 0
print("loop else:", found)

try:
    x = 10 // 0
    print("never")
except ZeroDivisionError as e:
    print("caught   :", e)

try:
    data = [1, 2, 3]
    print(data[10])
except IndexError:
    print("index err")

def safe_div(a, b):
    if b == 0:
        raise ValueError("cannot divide by zero")
    return a / b


try:
    print(safe_div(10, 2))
    print(safe_div(1, 0))
except ValueError as e:
    print("value err:", e)
finally:
    print("done")

print("assert   :", max(3, 7), min(3, 7))
assert n == 5
print("assert ok")

for idx, ch in enumerate("abc"):
    print(f"  {idx} -> {ch}")

for name, age in zip(["amy", "bob"], [30, 25]):
    print(f"  {name} is {age}")
