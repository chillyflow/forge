from urllib.parse import parse_qs

def parse_query(value):
    try:
        parsed = parse_qs(value, keep_blank_values=True)
        return {key: values[-1] for key, values in parsed.items()}
    except ValueError:
        raise ValueError("Invalid percent-encoding")
