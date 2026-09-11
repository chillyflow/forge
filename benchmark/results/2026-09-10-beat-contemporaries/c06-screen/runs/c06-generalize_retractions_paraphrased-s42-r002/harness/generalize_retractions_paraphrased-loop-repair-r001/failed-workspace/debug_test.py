#!/usr/bin/env python3

# Reproduce the exact test case
from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
print("Events:", events)

result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")
print("Match:", result == {'cash': 0})

# Test the other cases too
print("\n--- Testing other cases ---")

# Case 2: [retract('r', 'p'), p]
events2 = [retract('r', 'p'), p]
print("Events2:", events2)
result2 = balances(events2)
print("Result2:", result2)
print("Expected: {'cash': 0}")
print("Match:", result2 == {'cash': 0})

# Case 3: [p, p, retract('r', 'p'), retract('s', 'p')]
events3 = [p, p, retract('r', 'p'), retract('s', 'p')]
print("Events3:", events3)
result3 = balances(events3)
print("Result3:", result3)
print("Expected: {'cash': 0}")
print("Match:", result3 == {'cash': 0})