from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Test the failing case
p = post('p', 7)
events = [p, retract('r', 'p')]
print("Events:", events)
result = balances(events)
print("Result:", result)
print("Expected: {'cash': 0}")

# Let's trace what should happen:
print("\nStep by step:")
print("1. Post event: id='p', account='cash', amount=7")
print("   - postings['p'] = ('cash', 7)")
print("   - totals['cash'] = 0 + 7 = 7")
print("2. Retract event: id='r', target='p'")
print("   - target 'p' in postings: True")
print("   - account='cash', amount=7")
print("   - totals['cash'] = 7 - 7 = 0")