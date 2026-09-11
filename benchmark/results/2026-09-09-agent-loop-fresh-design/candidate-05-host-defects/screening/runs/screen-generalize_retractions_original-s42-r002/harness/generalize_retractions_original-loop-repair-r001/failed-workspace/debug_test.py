from service import balances

# Test the specific failing case
p = {'id': 'p', 'kind': 'post', 'account': 'cash', 'amount': 7}
r = {'id': 'r', 'kind': 'retract', 'target': 'p'}

events = [p, r]
result = balances(events)
print(f"Events: {events}")
print(f"Result: {result}")
print(f"Expected: {{'cash': 0}}")

# Test the reverse order
events2 = [r, p]
result2 = balances(events2)
print(f"Events2: {events2}")
print(f"Result2: {result2}")
print(f"Expected: {{'cash': 0}}")