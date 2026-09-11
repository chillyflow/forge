from service import balances

# Test the failing case
result1 = balances([{'id': 'r', 'kind': 'retract', 'target': 'p'}, {'id': 'p', 'kind': 'post', 'account': 'cash', 'amount': 7}])
print(f"Case 1 result: {result1}")
print(f"Expected: {{'cash': 0}}")

# Test the working case  
result2 = balances([{'id': 'p', 'kind': 'post', 'account': 'cash', 'amount': 7}, {'id': 'r', 'kind': 'retract', 'target': 'p'}])
print(f"Case 2 result: {result2}")
print(f"Expected: {{'cash': 0}}")

# Test the third case from the test
p = {'id': 'p', 'kind': 'post', 'account': 'cash', 'amount': 7}
r = {'id': 'r', 'kind': 'retract', 'target': 'p'}
result3 = balances([p, p, r, {'id': 's', 'kind': 'retract', 'target': 'p'}])
print(f"Case 3 result: {result3}")
print(f"Expected: {{'cash': 0}}")