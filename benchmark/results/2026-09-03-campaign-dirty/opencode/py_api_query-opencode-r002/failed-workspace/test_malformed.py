from urllib.parse import parse_qs, unquote

# Test malformed percent escape handling
try:
    result = parse_qs('bad=%ZZ', keep_blank_values=True)
    print('Result:', result)
    
    # Try to unquote the problematic value
    for key, values in result.items():
        for value in values:
            print(f'Unquoting {value}:')
            try:
                unquoted = unquote(value)
                print(f'  Success: {unquoted}')
            except Exception as e:
                print(f'  Error: {e}')
except Exception as e:
    print('Parse error:', e)