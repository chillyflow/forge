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

# Test the reverse case
events2 = [retract('r', 'p'), p]
result2 = balances(events2)
print(f"Events2: {events2}")
print(f"Result2: {result2}")
print(f"Expected2: {{'cash': 0}}")