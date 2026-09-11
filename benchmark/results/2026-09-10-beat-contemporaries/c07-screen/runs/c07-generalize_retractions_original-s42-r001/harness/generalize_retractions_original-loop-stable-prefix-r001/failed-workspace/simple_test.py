import sys
sys.path.insert(0, '.')

from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test case that's failing
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = balances(events)
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's also test the other cases to make sure they work
print("\n--- Testing other cases ---")

# Case 1: [p, retract('r', 'p')]
events1 = [p, retract('r', 'p')]
result1 = balances(events1)
print(f"Case 1 result: {result1}")

# Case 2: [retract('r', 'p'), p]  
events2 = [retract('r', 'p'), p]
result2 = balances(events2)
print(f"Case 2 result: {result2}")