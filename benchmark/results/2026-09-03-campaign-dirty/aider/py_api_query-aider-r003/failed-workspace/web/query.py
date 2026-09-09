from urllib.parse import parse_qs, unquote

def parse_query(value):
    try:
        parsed = parse_qs(value, keep_blank_values=True)
        return {key: values[-1] for key, values in parsed.items()}
    except Exception:
        raise ValueError("Malformed percent escape")
