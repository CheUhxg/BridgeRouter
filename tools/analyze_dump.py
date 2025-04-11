import sys
import json

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_dump.py <json_path>")
        return

    with open(sys.argv[1], 'r') as f:
        data = json.load(f)

    for struct, info in data.items():
        label = []
        if info.get("bridge", False):
            label.append("B")
        if info.get("router", False):
            label.append("R")
        print(f"{struct} {''.join(label)}")

if __name__ == "__main__":
    main()