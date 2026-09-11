#!/usr/bin/env python3

import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the specific failing case from the test suite
p = post('p', 7)
print("p =", p)

# Test case 1: [p, retract('r', 'p')] - This should work
result1 = balances([p, retract('r', 'p')])
print("Case 1 [p, retract('r', 'p')]:", result1)

# Test case 2: [retract('r', 'p'), p] - This is the failing case
result2 = balances([retract('r', 'p'), p])
print("Case 2 [retract('r', 'p'), p]:", result2)

# Test case 3: [p, p, retract('r', 'p'), retract('s', 'p')]
result3 = balances([p, p, retract('r', 'p'), retract('s', 'p')])
print("Case 3 [p, p, retract('r', 'p'), retract('s', 'p')]:", result3)