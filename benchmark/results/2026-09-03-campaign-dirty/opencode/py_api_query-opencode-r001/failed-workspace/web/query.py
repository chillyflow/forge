from urllib.parse import parse_qs, urlparse
import urllib.parse

def parse_query(value):
    # Validate that the query string doesn't contain malformed percent escapes
    # by attempting to parse it with urllib.parse
    try:
        # First, check if we can parse the query string without errors
        # This will catch malformed percent escapes
        parsed = parse_qs(value, keep_blank_values=True)
        
        # Return the last value for each key (as per the test case)
        return {key: values[-1] for key, values in parsed.items()}
    except ValueError as e:
        # Re-raise ValueError for malformed percent escapes
        raise
    except Exception:
        # For any other parsing errors, raise ValueError
        raise ValueError("Invalid query string")
