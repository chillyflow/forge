from urllib.parse import parse_qs
import re

def parse_query(value):
    # Check for malformed percent-encoding
    if value is not None:
        # Find all percent-encoded sequences
        percent_matches = re.findall(r'%[0-9A-Fa-f]{2}', value)
        for match in percent_matches:
            # Check if the sequence is valid (should be exactly 3 characters: % + 2 hex digits)
            if len(match) != 3:
                raise ValueError("Invalid percent-encoding")
    
    parsed = parse_qs(value, keep_blank_values=True)
    return {key: values[-1] for key, values in parsed.items()}
