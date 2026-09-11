from service import balances

def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

# Test the exact failing case
p = post('p', 7)
events = [retract('r', 'p'), p]
print(f"Events: {events}")
result = balances(events)
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Let's trace what should happen:
# 1. Process retract('r', 'p') - marks 'p' as cancelled, no posting with id 'p' yet, so nothing happens to totals
# 2. Process post('p', 7) - creates posting, adds to totals, but since 'p' is in cancelled, it should not be added to totals
print("\nDetailed trace:")
print("1. Process retract('r', 'p'):")
print("   - event id 'r' not seen, so process")
print("   - event kind is 'retract'")
print("   - target is 'p'")
print("   - 'p' not in cancelled, so add to cancelled")
print("   - 'p' not in postings, so no effect on totals")
print("2. Process post('p', 7):")
print("   - event id 'p' not seen, so process")
print("   - event kind is 'post'")
print("   - target 'p' is in cancelled set, so skip adding to totals")