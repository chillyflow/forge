from urllib.parse import parse_qs
import re

def parse_query(value):
    # Check for malformed percent-encoding
    if value is not None:
        # Find all percent-encoded sequences
        percent_matches = re.findall(r'%[0-9a-fA-F]{2}', value)
        for match in percent_matches:
            # Check if the sequence is valid (two hex digits)
            if not re.match(r'%[0-9a-fA-F]{2}$', match):
                raise ValueError("Invalid percent-encoding")
    
    parsed = parse_qs(value, keep_blank_values=True)
    return {key: values[-1] for key, values in parsed.items()}
