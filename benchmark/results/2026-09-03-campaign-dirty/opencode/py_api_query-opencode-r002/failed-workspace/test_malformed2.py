from urllib.parse import unquote

# Test malformed percent escape handling
try:
    result = unquote('%ZZ')
    print('Result:', repr(result))
except Exception as e:
    print('Exception:', e)