from urllib.parse import parse_qs
import re

def parse_query(value):
    # Check for malformed percent-encoding: % followed by non-hex characters
    if re.search(r'%[^\w%]', value):
        # More specific check: % followed by non-hex characters (not part of valid hex)
        if re.search(r'%[^0-9A-Fa-f]', value):
            raise ValueError("Invalid percent-encoding")
    
    # Also check for incomplete percent-encoding (single % at end)
    if re.search(r'%[^0-9A-Fa-f]$', value):
        raise ValueError("Invalid percent-encoding")
    
    parsed = parse_qs(value, keep_blank_values=True)
    return {key: values[-1] for key, values in parsed.items()}
