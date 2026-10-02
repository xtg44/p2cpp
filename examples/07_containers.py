# 07_containers.py -- sets, tuples, comprehensions, nested data

s = {3, 1, 2, 3, 1}
print("set      :", sorted(s))
print("len      :", len(s))
s.add(5)
s.discard(1)
print("after    :", sorted(s))
print("contains :", 3 in s, 99 in s)

a = {1, 2, 3}
b = {3, 4, 5}
print("union    :", sorted(a | b))
print("inter    :", sorted(a & b))
print("diff     :", sorted(a - b))

t = (1, "two", 3.0)
print("tuple    :", t)
x, y, z = t
print("unpack   :", x, y, z)

nested = [[1, 2], [3, 4], [5, 6]]
flat = [n for row in nested for n in row]
print("flat     :", flat)
print("sum all  :", sum(flat))

sq = {n: n * n for n in range(1, 6)}
print("sq dict  :", sq)
print("evens    :", [n for n in range(10) if n % 2 == 0])
print("matrix   :", [[i * j for j in range(3)] for i in range(3)])

words = ["apple", "banana", "cherry", "avocado"]
by_first = {}
for w in words:
    k = w[0]
    if k not in by_first:
        by_first[k] = []
    by_first[k].append(w)
print("grouped  :", by_first)

# setdefault-style accumulation
counts = {}
for c in "abracadabra":
    counts[c] = counts.get(c, 0) + 1
print("counts   :", counts)

pairs = [("a", 1), ("b", 2)]
d2 = {}
for key, val in pairs:
    d2[key] = val
print("d2       :", d2)
