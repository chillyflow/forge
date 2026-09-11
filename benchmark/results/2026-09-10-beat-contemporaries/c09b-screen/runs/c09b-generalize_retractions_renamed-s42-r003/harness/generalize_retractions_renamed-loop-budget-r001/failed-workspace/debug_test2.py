#!/usr/bin/env python3

import sys
sys.path.insert(0, '.')

from ledger import positions_for
from test_ledger import post, retract

def test_case():
    p = post('p', 7)
    messages = [p, p, retract('r', 'p'), retract('s', 'p')]
    
    print("Test case: [p, p, retract('r', 'p'), retract('s', 'p')]")
    print(f"p = {p}")
    print(f"messages = {messages}")
    
    result = positions_for(messages)
    print(f"Result: {result}")
    print(f"Expected: {{'cash': 0}}")
    print(f"Match: {result == {'cash': 0}}")

if __name__ == "__main__":
    test_case()