from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print("Test case [p, retract('r', 'p')]:", result)
print("Expected: {'cash': 0}")
print("Actual:", result)

# Test the specific failing case
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = balances(events)
print("\nTest case [p, p, retract('r', 'p'), retract('s', 'p')]:", result)
print("Expected: {'cash': 0}")
print("Actual:", result)