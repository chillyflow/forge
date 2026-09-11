#!/usr/bin/env python3

from ledger import positions_for

def post(i, delta): 
    return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}

def retract(i, reference): 
    return {'key': i, 'kind': 'retract', 'reference': reference}

# Test the failing case
p = post('p', 7)
result = positions_for([p, retract('r', 'p')])
print(f"Result for [post('p', 7), retract('r', 'p')]: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's also test the other cases
result2 = positions_for([retract('r', 'p'), p])
print(f"Result for [retract('r', 'p'), post('p', 7)]: {result2}")
print(f"Expected: {{'cash': 0}}")

result3 = positions_for([p, p, retract('r', 'p'), retract('s', 'p')])
print(f"Result for [post('p', 7), post('p', 7), retract('r', 'p'), retract('s', 'p')]: {result3}")
print(f"Expected: {{'cash': 0}}")