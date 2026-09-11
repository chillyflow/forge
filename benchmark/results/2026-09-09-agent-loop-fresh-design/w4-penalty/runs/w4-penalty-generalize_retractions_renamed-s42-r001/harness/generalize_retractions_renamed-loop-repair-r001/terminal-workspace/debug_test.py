#!/usr/bin/env python3

from ledger import positions_for

def post(i, delta): 
    return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}

def retract(i, reference): 
    return {'key': i, 'kind': 'retract', 'reference': reference}

# Test case 1: [p, retract('r', 'p')] where p = post('p', 7)
p = post('p', 7)
messages1 = [p, retract('r', 'p')]
result1 = positions_for(messages1)
print(f"Test 1 - Messages: {messages1}")
print(f"Result: {result1}")
print(f"Expected: {{'cash': 0}}")
print()

# Test case 2: [retract('r', 'p'), p] where p = post('p', 7) 
messages2 = [retract('r', 'p'), p]
result2 = positions_for(messages2)
print(f"Test 2 - Messages: {messages2}")
print(f"Result: {result2}")
print(f"Expected: {{'cash': 0}}")
print()

# Test case 3: [p, p, retract('r', 'p'), retract('s', 'p')] where p = post('p', 7)
messages3 = [p, p, retract('r', 'p'), retract('s', 'p')]
result3 = positions_for(messages3)
print(f"Test 3 - Messages: {messages3}")
print(f"Result: {result3}")
print(f"Expected: {{'cash': 0}}")