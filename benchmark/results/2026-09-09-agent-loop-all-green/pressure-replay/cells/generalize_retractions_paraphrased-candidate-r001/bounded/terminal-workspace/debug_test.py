import unittest
from service import balances

def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

# Test the specific failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's also test the other cases
events2 = [retract('r', 'p'), p]
result2 = balances(events2)
print(f"Result2: {result2}")

events3 = [p, p, retract('r', 'p'), retract('s', 'p')]
result3 = balances(events3)
print(f"Result3: {result3}")