from urllib.parse import parse_qs, unquote
import urllib.parse

# Test various cases
test_cases = [
    'a=1&a=2&empty=',
    'bad=%ZZ',
    'normal=value',
    'empty=',
    '%20=space'
]

for case in test_cases:
    print(f'Testing: {case}')
    try:
        result = parse_qs(case, keep_blank_values=True)
        print(f'  Result: {result}')
        
        # Try to manually unquote each value to detect malformed escapes
        for key, values in result.items():
            for value in values:
                try:
                    unquoted = unquote(value)
                    print(f'    Unquoted "{value}": "{unquoted}"')
                except Exception as e:
                    print(f'    Error unquoting "{value}": {e}')
    except Exception as e:
        print(f'  Parse error: {e}')
    print()