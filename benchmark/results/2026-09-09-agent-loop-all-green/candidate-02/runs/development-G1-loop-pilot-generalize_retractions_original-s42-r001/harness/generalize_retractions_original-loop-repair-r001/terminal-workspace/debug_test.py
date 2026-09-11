#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
print("p object id:", id(p))
print("p object:", p)

events = [p, p, retract('r', 'p'), retract('s', 'p')]
print("Events:", events)

result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Test simpler cases
print("\n--- Test 1 ---")
result1 = balances([p, retract('r', 'p')])
print("Result 1:", result1)
print("Expected: {'cash': 0}")

print("\n--- Test 2 ---")
result2 = balances([retract('r', 'p'), p])
print("Result 2:", result2)
print("Expected: {'cash': 0}")