import sys

def parse_braces(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    lines = content.splitlines()
    stack = []
    
    in_block_comment = False
    in_line_comment = False
    in_string = False
    escape = False

    for line_idx, line in enumerate(lines):
        line_num = line_idx + 1
        i = 0
        while i < len(line):
            char = line[i]
            
            if in_block_comment:
                if char == '*' and i + 1 < len(line) and line[i+1] == '/':
                    in_block_comment = False
                    i += 2
                    continue
                i += 1
                continue
                
            if in_line_comment:
                break
                
            if in_string:
                if escape:
                    escape = False
                elif char == '\\':
                    escape = True
                elif char == '"':
                    in_string = False
                i += 1
                continue
                
            if char == '/' and i + 1 < len(line):
                if line[i+1] == '/':
                    in_line_comment = True
                    break
                elif line[i+1] == '*':
                    in_block_comment = True
                    i += 2
                    continue
                    
            if char == '"':
                in_string = True
                escape = False
                i += 1
                continue
                
            if char == '{':
                stack.append(('{', line_num, line.strip()))
                if 800 <= line_num <= 1220:
                    print(f"PUSH {{ at line {line_num}: {line.strip()[:40]}")
            elif char == '}':
                if not stack:
                    print(f"Extra closing brace '}}' at line {line_num}: {line.strip()}")
                else:
                    popped = stack.pop()
                    if 800 <= line_num <= 1220:
                        print(f"POP matching {{ from line {popped[1]} at line {line_num}")
                    
            i += 1
            
        in_line_comment = False

    print(f"Parsing complete. Unclosed braces remaining in stack: {len(stack)}")
    for item in stack:
        print(f"  Unclosed '{{' opened at line {item[1]}: {item[2]}")

if __name__ == '__main__':
    parse_braces(r"c:\Users\elias\Desktop\GITHUB\Token-Talks\dailyrem\discord_client.cpp")

