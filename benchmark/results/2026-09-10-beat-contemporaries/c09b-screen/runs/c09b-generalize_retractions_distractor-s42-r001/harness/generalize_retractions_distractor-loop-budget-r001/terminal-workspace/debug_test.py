from service import balances

def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = balances(events)
print("Result:", result)

# Test simpler cases
events2 = [p, retract('r', 'p')]
result2 = balances(events2)
print("Result2:", result2)

events3 = [retract('r', 'p'), p]
result3 = balances(events3)
print("Result3:", result3)