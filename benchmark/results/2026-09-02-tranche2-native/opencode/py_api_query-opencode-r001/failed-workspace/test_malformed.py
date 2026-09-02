from urllib.parse import parse_qs, unquote

# Test malformed percent escape
try:
    result = parse_qs('bad=%ZZ', keep_blank_values=True)
    print('Result:', result)
    
    # Try to unquote the problematic value
    try:
        unquoted = unquote('%ZZ')
        print('Unquoted:', unquoted)
    except Exception as e:
        print('Unquote error:', e)
        
except Exception as e:
    print('Parse error:', e)