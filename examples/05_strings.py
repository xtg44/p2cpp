# 05_strings.py -- string methods, slicing, f-string format specs

s = "Hello, World"
print(s.upper())
print(s.lower())
print(s.replace("World", "Python"))
print(s.split(", "))
print(len(s), s[0], s[-1])
print(s[0:5], s[7:], s[:5])
print(s[::-1])
print("  padded  ".strip() + "|")
print(s.startswith("Hello"), s.endswith("World"))
print(s.find("World"), s.count("l"))
print("-".join(["a", "b", "c"]))
print(s.title(), s.capitalize())

name = "Ada"
score = 92.5
print(f"{name} scored {score:.2f}")
print(f"{name:>10}|")
print(f"{name:<10}|")
print(f"{name:^10}|")
print(f"{255:x} {255:o} {5:b}")
print(f"{1234567:,}")
print(f"{0.5:.1%}")
print(f"{42:05d}")

# numbers <-> strings
print(int("123"), float("3.5"), str(7), str(True))
print(abs(-5), round(3.14159, 2), min(3, 1, 2), max([3, 1, 2]))
print(chr(65), ord("A"))
