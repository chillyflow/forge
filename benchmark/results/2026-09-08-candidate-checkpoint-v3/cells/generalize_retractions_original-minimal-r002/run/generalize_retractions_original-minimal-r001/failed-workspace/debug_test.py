import sys
sys.path.append('.')
from service import balances

def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

# Test case that's failing
p = post('p', 7)
events = [retract('r', 'p'), p]
result = balances(events)
print(f"Events: {events}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")