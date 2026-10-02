import math
print(math.sqrt(2))
print(round(math.pi, 4))
print(abs(-7), abs(-7.5))
print(int("42") + 1, float("3.5") * 2)
print("a-b-c".split("-"))
print("-".join(["x", "y"]))
t = "Hello, World"
print(t.upper(), t.lower())
print(t.replace("World", "there"))
print(t.startswith("Hello"), t.endswith("!"))
d = {"x": 1, "y": 2}
print(list(d.keys()), list(d.values()))
print("x" in d, "z" in d)
for k, v in d.items():
    print(f"{k}={v}")
