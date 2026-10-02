# 01_basics.py -- variables, arithmetic, strings, conditionals
name = "World"
count = 3
pi = 3.14159
flag = True

print("Hello,", name, "!")
print("count =", count, " pi =", pi, " flag =", flag)

# Python 的整数除法与地板除语义
print("7 / 2  =", 7 / 2)
print("7 // 2 =", 7 // 2)
print("-7 // 2 =", -7 // 2)
print("-7 % 2  =", -7 % 2)
print("2 ** 10 =", 2 ** 10)

if count > 5:
    print("big")
elif count == 3:
    print("three")
else:
    print("small")

s = "  Hello, Python  "
print("[" + s.strip() + "]")
print(s.strip().upper())
print(s.strip().lower())
print("length =", len(s.strip()))
print("comma at", s.find(","))

for i in range(3):
    print(f"i = {i}, square = {i * i}, half = {i / 2:.3f}")
