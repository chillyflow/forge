#!/usr/bin/env python3

# Direct test to isolate the issue
import sys
sys.path.insert(0, '.')

# Copy the exact functions from test_service.py
def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Import the function we're testing
from service import balances

# Test case that's failing
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print(f"Test case [p, retract('r', 'p')]:")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")
print(f"Match: {result == {'cash': 0}}")

# Test other cases
events2 = [retract('r', 'p'), p]
result2 = balances(events2)
print(f"\nTest case [retract('r', 'p'), p]:")
print(f"Result: {result2}")
print(f"Expected: {{'cash': 0}}")
print(f"Match: {result2 == {'cash': 0}}")

# Test case with duplicates
events3 = [p, p, retract('r', 'p'), retract('s', 'p')]
result3 = balances(events3)
print(f"\nTest case [p, p, retract('r', 'p'), retract('s', 'p')]:")
print(f"Result: {result3}")
print(f"Expected: {{'cash': 0}}")
print(f"Match: {result3 == {'cash': 0}}")