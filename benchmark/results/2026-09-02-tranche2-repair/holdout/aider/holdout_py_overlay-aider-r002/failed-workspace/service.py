from copy import deepcopy

def overlay(base, changes):
    result = deepcopy(base)
    for key, value in changes.items():
        if value is None:
            result.pop(key, None)
        elif isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = overlay(result[key], value)
        else:
            result[key] = value
    return result
