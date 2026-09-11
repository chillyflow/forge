#!/usr/bin/env python3

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print(f"Events: {events}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's also test the other cases
print("\n--- Testing all cases ---")
for i, events in enumerate([[p, retract('r', 'p')], [retract('r', 'p'), p],
                           [p, p, retract('r', 'p'), retract('s', 'p')]]):
    result = balances(events)
    print(f"Case {i+1}: {events} -> {result}")