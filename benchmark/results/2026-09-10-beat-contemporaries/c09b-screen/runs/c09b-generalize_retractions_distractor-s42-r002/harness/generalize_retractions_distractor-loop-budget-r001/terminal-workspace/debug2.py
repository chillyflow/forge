import unittest
from service import balances
from test_service import post

# Replicate the exact test case
p = post('p', 7)
events = [p, p, {'id': 'r', 'kind': 'retract', 'target': 'p'}, {'id': 's', 'kind': 'retract', 'target': 'p'}]

print("Events:", events)
result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Test each individual case from the test
print("\nTesting first case [p, retract('r', 'p')]:")
result1 = balances([p, {'id': 'r', 'kind': 'retract', 'target': 'p'}])
print("Result:", result1)

print("\nTesting second case [retract('r', 'p'), p]:")
result2 = balances([{'id': 'r', 'kind': 'retract', 'target': 'p'}, p])
print("Result:", result2)

print("\nTesting third case [p, p, retract('r', 'p'), retract('s', 'p')]:")
result3 = balances([p, p, {'id': 'r', 'kind': 'retract', 'target': 'p'}, {'id': 's', 'kind': 'retract', 'target': 'p'}])
print("Result:", result3)