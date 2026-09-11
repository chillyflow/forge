#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test case that's failing
p = post('p', 7)
events = [p, retract('r', 'p')]
print("Events:", events)

result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Test the other failing case
print("\n--- Testing [retract('r', 'p'), p] ---")
events2 = [retract('r', 'p'), p]
print("Events:", events2)
result2 = balances(events2)
print("Result:", result2)
print("Expected: {'cash': 0}")