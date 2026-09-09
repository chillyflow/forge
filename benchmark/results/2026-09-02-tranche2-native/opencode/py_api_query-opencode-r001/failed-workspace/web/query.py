from urllib.parse import parse_qs, unquote
import urllib.parse

def parse_query(value):
    # Parse the query string while keeping blank values and handling repeated keys
    try:
        # Use parse_qs with keep_blank_values=True to preserve empty values
        parsed = parse_qs(value, keep_blank_values=True)
        
        # Validate that all values are properly percent-encoded
        for key, values in parsed.items():
            for val in values:
                # Try to unquote the value to check for malformed percent escapes
                try:
                    unquote(val)
                except Exception:
                    raise ValueError("Invalid query string")
        
        # For each key, return the last value (in case of repeated keys)
        result = {}
        for key, values in parsed.items():
            if values:  # Only add if there are values
                result[key] = values[-1]  # Get the last value
        
        return result
    except ValueError:
        # Re-raise ValueError for malformed percent escapes
        raise
    except Exception:
        # Handle any other parsing errors by raising ValueError
        raise ValueError("Invalid query string")
