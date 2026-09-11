#!/usr/bin/env python3

import sys
sys.path.insert(0, '.')

# Import the exact test setup
from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Replicate the exact test case that's failing
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]

print("Events:", events)
print("p object id:", id(p))

result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Let's also test the other cases to make sure they work
print("\n--- Testing other cases ---")

# Case 1: [p, retract('r', 'p')]
p1 = post('p1', 7)
r1 = retract('r1', 'p1')
events1 = [p1, r1]
result1 = balances(events1)
print("Case 1 [p, retract]:", result1)

# Case 2: [retract('r', 'p'), p]  
p2 = post('p2', 7)
r2 = retract('r2', 'p2')
events2 = [r2, p2]
result2 = balances(events2)
print("Case 2 [retract, p]:", result2)