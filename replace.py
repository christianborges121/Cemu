import os
import re

directories = [
    'src/streaming',
    'src/input',
    'src/gui',
    'src/Cafe'
]

targets = ['StreamingCapture', 'VideoStreamServer', 'DiscoveryServer']

for directory in directories:
    for root, _, files in os.walk(directory):
        for file in files:
            if file.endswith('.cpp') or file.endswith('.h'):
                filepath = os.path.join(root, file)
                with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                    content = f.read()
                
                original = content
                for target in targets:
                    content = content.replace(f'{target}::GetInstance()', f'{target}::instance()')
                
                if content != original:
                    with open(filepath, 'w', encoding='utf-8') as f:
                        f.write(content)
                    print(f"Updated {filepath}")
