#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = balances(events)
print(f"Input events: {events}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")
print(f"Match: {result == {'cash': 0}}")