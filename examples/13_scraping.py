# Web scraping: regex extraction, JSON, CSV — no network needed here.
import re
import json

html = '<html><head><title>商品列表</title></head><body>' \
       '<div class="item"><span class="name">苹果</span><span class="price">5.5</span></div>' \
       '<div class="item"><span class="name">香蕉</span><span class="price">3.2</span></div></body></html>'

# Pull the title out of its tag.
title = re.search("<title>(.*?)</title>", html).group(1)
print("标题:", title)

# Find every product name and price (a capture group returns the group).
names = re.findall('<span class="name">([^<]*)</span>', html)
prices = re.findall('<span class="price">([^<]*)</span>', html)
print("商品:", names)
print("价格:", prices)

# Strip every tag (re.sub).
text = re.sub("<[^>]*>", "", html)
print("纯文本:", text)

# Round-trip a JSON object (values are stored as strings here).
d = json.loads('{"site": "example", "count": 2}')
print("JSON:", json.dumps(d))
