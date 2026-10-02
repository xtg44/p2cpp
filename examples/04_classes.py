# 04_classes.py -- 面向对象


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def length_sq(self):
        return self.x * self.x + self.y * self.y

    def scale(self, k):
        return Point(self.x * k, self.y * k)

    def show(self):
        print(f"Point({self.x}, {self.y})")


class Counter:
    def __init__(self, start=0):
        self.value = start
        self.history = []

    def inc(self, step=1):
        self.value = self.value + step
        self.history.append(self.value)
        return self.value

    def total(self):
        return sum(self.history)


p = Point(3, 4)
p.show()
print("length_sq =", p.length_sq())

q = p.scale(2)
q.show()
print("q.length_sq =", q.length_sq())


def dist_sq(a, b):
    dx = a.x - b.x
    dy = a.y - b.y
    return dx * dx + dy * dy


print("dist_sq(p, q) =", dist_sq(p, q))

c = Counter()
for i in range(5):
    c.inc(i)
print("value   =", c.value)
print("history =", c.history)
print("total   =", c.total())

c2 = Counter(100)
c2.inc()
print("c2.value =", c2.value)
