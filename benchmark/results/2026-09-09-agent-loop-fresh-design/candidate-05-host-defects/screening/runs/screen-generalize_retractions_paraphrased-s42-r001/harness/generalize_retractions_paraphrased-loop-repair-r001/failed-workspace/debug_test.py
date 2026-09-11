#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
print("Post event:", p)
print("Retract event:", retract('r', 'p'))

result = balances([p, retract('r', 'p')])
print("Result:", result)
print("Expected: {'cash': 0}")

# Also test the other failing case
print("\n--- Testing second case ---")
result2 = balances([retract('r', 'p'), p])
print("Result2:", result2)
print("Expected: {'cash': 0}")