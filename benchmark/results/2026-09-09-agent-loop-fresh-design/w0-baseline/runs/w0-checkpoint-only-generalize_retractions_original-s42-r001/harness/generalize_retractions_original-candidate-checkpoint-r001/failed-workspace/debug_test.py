#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
result = balances([p, retract('r', 'p')])
print(f"Result for [post('p', 7), retract('r', 'p')]: {result}")
print(f"Expected: {{'cash': 0}}")

# Test reverse order
result2 = balances([retract('r', 'p'), p])
print(f"Result for [retract('r', 'p'), post('p', 7)]: {result2}")
print(f"Expected: {{'cash': 0}}")

# Test the third case
result3 = balances([p, p, retract('r', 'p'), retract('s', 'p')])
print(f"Result for [post('p', 7), post('p', 7), retract('r', 'p'), retract('s', 'p')]: {result3}")
print(f"Expected: {{'cash': 0}}")