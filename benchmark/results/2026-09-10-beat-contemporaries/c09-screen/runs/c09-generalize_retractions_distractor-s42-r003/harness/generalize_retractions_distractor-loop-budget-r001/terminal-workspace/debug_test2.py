import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test exactly what the failing test is doing
p = post('p', 7)

# First test case: [p, retract('r', 'p')]
events = [p, retract('r', 'p')]
print("=== Test case 1 ===")
print(f"Events: {events}")
result = balances(events)
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Second test case: [retract('r', 'p'), p]
events2 = [retract('r', 'p'), p]
print("\n=== Test case 2 ===")
print(f"Events: {events2}")
result2 = balances(events2)
print(f"Result: {result2}")
print(f"Expected: {{'cash': 0}}")

# Third test case: [p, p, retract('r', 'p'), retract('s', 'p')]
events3 = [p, p, retract('r', 'p'), retract('s', 'p')]
print("\n=== Test case 3 ===")
print(f"Events: {events3}")
result3 = balances(events3)
print(f"Result: {result3}")
print(f"Expected: {{'cash': 0}}")