from urllib.parse import parse_qs

def parse_query(value):
    # Parse the query string while keeping blank values and handling repeated keys
    try:
        parsed = parse_qs(value, keep_blank_values=True, strict_parsing=True)
    except ValueError:
        # Re-raise ValueError for malformed percent escapes
        raise ValueError("Invalid percent-encoding")
    
    # Return the last value for repeated keys
    return {key: values[-1] for key, values in parsed.items()}
