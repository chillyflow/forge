from urllib.parse import parse_qs

def parse_query(value):
    parsed = parse_qs(value, keep_blank_values=True)
    return {key: values[-1] for key, values in parsed.items()}
