from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
r = retract('r', 'p')

print("Testing [retract('r', 'p'), post('p', 7)]")
result = balances([r, p])
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

print("\nTesting [post('p', 7), retract('r', 'p')]")
result2 = balances([p, r])
print(f"Result: {result2}")
print(f"Expected: {{'cash': 0}}")