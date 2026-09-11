#!/usr/bin/env python3

import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount):
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target):
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = balances(events)
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's also test the other cases to make sure they work
events2 = [p, retract('r', 'p')]
result2 = balances(events2)
print(f"Case 2 result: {result2}")
print(f"Case 2 expected: {{'cash': 0}}")

events3 = [retract('r', 'p'), p]
result3 = balances(events3)
print(f"Case 3 result: {result3}")
print(f"Case 3 expected: {{'cash': 0}}")