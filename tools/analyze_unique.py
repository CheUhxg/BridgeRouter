v4_text = []
with open('v4.20', 'r') as fp:
    for line in fp:
        v4_text.append(line.split(' ', 1)[0].strip())  # 分割第一个空格前的部分

v5_text = []
with open('v5.15', 'r') as fp:
    for line in fp:
        v5_text.append(line.split(' ', 1)[0].strip())  # 分割第一个空格前的部分

v6_text = []
with open('v6.6', 'r') as fp:
    for line in fp:
        v6_text.append(line.split(' ', 1)[0].strip())  # 分割第一个空格前的部分

merge = []
# v4 unique
num = 0
merge = list(set(v5_text + v6_text))
merge = sorted(merge)
for line in v4_text:
    if line not in merge:
        print(line.strip())
        num += 1
print(f"v4: {num}\n")

# v5 unique
num = 0
merge = list(set(v4_text + v6_text))
merge = sorted(merge)
for line in v5_text:
    if line not in merge:
        print(line.strip())
        num += 1
print(f"v5: {num}\n")

# v6 unique
num = 0
merge = list(set(v5_text + v4_text))
merge = sorted(merge)
for line in v6_text:
    if line not in merge:
        print(line.strip())
        num += 1
print(f"v6: {num}\n")