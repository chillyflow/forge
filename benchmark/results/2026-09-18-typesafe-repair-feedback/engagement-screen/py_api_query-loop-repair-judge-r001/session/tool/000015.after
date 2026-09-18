from urllib.parse import parse_qs

def parse_query(value):
    # Parse the query string with keep_blank_values=True to keep empty values
    parsed = parse_qs(value, keep_blank_values=True)
    
    # Check for malformed percent escapes by attempting to parse each value
    for key, values in parsed.items():
        for val in values:
            # Check if the value contains malformed percent escapes
            # A malformed percent escape is one that starts with % but isn't followed by two hex digits
            if '%' in val:
                # Find all percent escape patterns
                import re
                # Pattern to match % followed by non-hex characters or incomplete hex
                percent_matches = re.findall(r'%[0-9a-fA-F]?[0-9a-fA-F]?', val)
                for match in percent_matches:
                    if match != '%' and len(match) == 3:  # %XX pattern - valid
                        continue
                    elif match != '%' and len(match) == 2:  # %X pattern - invalid
                        raise ValueError("Invalid percent escape")
                    elif match == '%':  # % alone - invalid
                        raise ValueError("Invalid percent escape")
    
    # Return the last value for repeated keys
    result = {}
    for key, values in parsed.items():
        # Use the last value for repeated keys
        result[key] = values[-1]
    
    return result
