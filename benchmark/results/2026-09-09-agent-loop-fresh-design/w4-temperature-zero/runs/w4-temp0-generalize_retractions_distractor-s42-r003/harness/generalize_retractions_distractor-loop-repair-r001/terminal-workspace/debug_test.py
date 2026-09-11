from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
print("Events:", events)
result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Let's trace what should happen:
print("\nStep by step:")
print("1. p (post 'p', 7) -> postings['p'] = ('cash', 7), totals['cash'] = 7")
print("2. p (post 'p', 7) -> duplicate, ignored")
print("3. retract('r', 'p') -> target 'p' in postings, totals['cash'] = 0")
print("4. retract('s', 'p') -> target 'p' in cancelled, ignored")