#!/usr/bin/env python3

# Recreate the exact test scenario
import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
print("Testing case 2: [retract('r', 'p'), post('p', 7)]")
result = balances([retract('r', 'p'), p])
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Also test case 1 for comparison
print("\nTesting case 1: [post('p', 7), retract('r', 'p)]")
result2 = balances([p, retract('r', 'p')])
print(f"Result: {result2}")
print(f"Expected: {{'cash': 0}}")