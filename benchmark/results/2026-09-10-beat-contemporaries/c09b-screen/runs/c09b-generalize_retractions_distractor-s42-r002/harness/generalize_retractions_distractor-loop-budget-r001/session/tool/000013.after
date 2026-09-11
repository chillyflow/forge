from service import balances
from test_service import post

# Test the failing case
p = post('p', 7)
events = [p, p, {'id': 'r', 'kind': 'retract', 'target': 'p'}, {'id': 's', 'kind': 'retract', 'target': 'p'}]
result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Let's also test the other cases from the test
print("\nTesting [p, retract('r', 'p')]:")
result2 = balances([p, {'id': 'r', 'kind': 'retract', 'target': 'p'}])
print("Result:", result2)

print("\nTesting [retract('r', 'p'), p]:")
result3 = balances([{'id': 'r', 'kind': 'retract', 'target': 'p'}, p])
print("Result:", result3)