import sys
sys.path.insert(0, '.')
from ledger import positions_for

def post(i, delta): return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}
def retract(i, reference): return {'key': i, 'kind': 'retract', 'reference': reference}

# Test the failing case
p = post('p', 7)
messages = [p, retract('r', 'p')]
result = positions_for(messages)
print(f"Input: {messages}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")