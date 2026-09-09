from urllib.parse import parse_qs
import re

def parse_query(value):
    # First check for invalid percent-encoding
    if value:
        # Find all percent-encoded sequences
        percent_matches = re.findall(r'%[0-9a-fA-F]{2}', value)
        for match in percent_matches:
            # Check if the sequence is valid (should be 2 hex digits)
            if not re.match(r'%[0-9a-fA-F]{2}', match):
                raise ValueError("Invalid percent-encoding")
    
    parsed = parse_qs(value, keep_blank_values=True)
    return {key: values[-1] for key, values in parsed.items()}
