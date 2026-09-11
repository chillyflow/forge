import sys
sys.path.insert(0, '.')

from ledger import positions_for
from test_ledger import post, retract

def debug_test():
    p = post('p', 7)
    print("Testing case 1: [p, retract('r', 'p')]")
    print("p =", p)
    print("retract('r', 'p') =", retract('r', 'p'))
    result1 = positions_for([p, retract('r', 'p')])
    print("Result:", result1)
    print("Expected: {'cash': 0}")
    print()
    
    print("Testing case 2: [retract('r', 'p'), p]")
    result2 = positions_for([retract('r', 'p'), p])
    print("Result:", result2)
    print("Expected: {'cash': 0}")
    print()
    
    print("Testing case 3: [p, p, retract('r', 'p'), retract('s', 'p')]")
    result3 = positions_for([p, p, retract('r', 'p'), retract('s', 'p')])
    print("Result:", result3)
    print("Expected: {'cash': 0}")
    print()

if __name__ == "__main__":
    debug_test()