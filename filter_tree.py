import re

def filter_tree(input_file: str, output_file: str):
    pattern = re.compile(r'^((?:[|│]   |    )*)(\+---|\\---|├───|└───)?(.*)$')
    skip_depth = None
    with open(input_file, 'r', encoding='utf-16') as fin, \
         open(output_file, 'w', encoding='utf-8') as fout:
        for line in fin:
            original_line = line
            line_stripped = line.rstrip('\n')
            
            if not line_stripped.strip():
                if skip_depth is None:
                    fout.write(original_line)
                continue
                
            match = pattern.match(line_stripped)
            if not match:
                if skip_depth is None:
                    fout.write(original_line)
                continue
                
            branch = match.group(2)
            name = match.group(3)
            
            current_depth = len(line_stripped) - len(name)
            
            if skip_depth is not None:
                if current_depth > skip_depth:
                    continue
                else:
                    skip_depth = None
            
            if branch is not None and name:
                if name.startswith('.') or name.startswith('__'):
                    skip_depth = current_depth
                    continue
            
            fout.write(original_line)

if __name__ == '__main__':
    filter_tree('tree.txt', 'tree_clean.txt')
