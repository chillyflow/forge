import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Reproduce the exact failing test case
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print(f"Input events: {events}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")
print(f"Match: {result == {'cash': 0}}")

# Test all cases from the failing test
print("\nTesting all cases from test_order_and_duplicates:")
for i, events in enumerate([[p, retract('r', 'p')], [retract('r', 'p'), p],
                           [p, p, retract('r', 'p'), retract('s', 'p')]]):
    result = balances(events)
    print(f"Case {i+1}: {events}")
    print(f"  Result: {result}")
    print(f"  Expected: {{'cash': 0}}")
    print(f"  Match: {result == {'cash': 0}}")