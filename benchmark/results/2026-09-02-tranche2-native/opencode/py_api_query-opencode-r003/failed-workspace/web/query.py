from urllib.parse import parse_qs, urlparse
import urllib.parse

def parse_query(value):
    # Parse the query string, keeping blank values and handling repeated keys
    try:
        parsed = parse_qs(value, keep_blank_values=True)
    except ValueError:
        # Re-raise ValueError for malformed percent escapes
        raise ValueError("Malformed percent escape")
    
    # Return the last value for repeated keys
    return {key: values[-1] for key, values in parsed.items()}
