from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")
print("Match:", result == {'cash': 0})