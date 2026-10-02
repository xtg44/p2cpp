class Shape:
    def __init__(self, name, sides):
        self.name = name
        self.sides = sides
    def describe(self):
        return f"{self.name} has {self.sides} sides"

class Square(Shape):
    def __init__(self, side):
        self.side = side
    def area(self):
        return self.side * self.side

s = Shape("triangle", 3)
print(s.describe())
q = Square(4)
print(q.area())
print(q.side)

nums = list(range(10))
print([n for n in nums if n % 3 == 0])
print(sum(n * n for n in nums))
print(sorted({c for c in "hello"}))
print(tuple(n for n in nums if n > 6))
