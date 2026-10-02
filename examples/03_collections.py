# 03_collections.py -- lists, dicts, sets, comprehensions
import math

xs = [5, 3, 9, 1, 7, 3]
print("list      :", xs)
print("len       :", len(xs))
print("sorted    :", sorted(xs))
print("reversed  :", sorted(xs, reverse=True))
print("sum       :", sum(xs))
print("min/max   :", min(xs), max(xs))
print("first/last:", xs[0], xs[-1])
print("slice     :", xs[1:4])
print("neg slice :", xs[-3:])
print("dup       :", xs * 2)

# list 方法与原地操作
xs.append(11)
xs.sort()
print("appended  :", xs)
xs.reverse()
print("reversed  :", xs)
xs.remove(3)
print("removed   :", xs)
print("index of 1:", xs.index(1))
print("count of 3:", xs.count(3))

# 推导式
squares = [x * x for x in range(10)]
evens = [x for x in range(20) if x % 2 == 0]
print("squares   :", squares)
print("evens     :", evens)

# 字典
ages = {"alice": 30, "bob": 25, "carol": 35}
print("dict      :", ages)
print("keys      :", sorted(ages.keys()))
print("values    :", sorted(ages.values()))
print("get       :", ages.get("dave", 0))
for k, v in ages.items():
    print(f"  {k} -> {v}")

# 用字典统计词频
words = "the quick brown fox jumps over the lazy dog the fox".split()
freq = {}
for w in words:
    if w in freq:
        freq[w] = freq[w] + 1
    else:
        freq[w] = 1
print("freq      :", freq)
print("len(freq) :", len(freq))

# 字符串
joined = ", ".join(["a", "b", "c"])
print("join      :", joined)
print("split     :", joined.split(", "))
print("replace   :", joined.replace("a", "A"))

# math 模块
print("sqrt(2)   :", math.sqrt(2))
print("pi        :", math.pi)
print("floor     :", math.floor(3.7))
print("ceil      :", math.ceil(3.2))
