import sys
sys.path.insert(0, '.')

# Import the actual test functions
from test_service import balances, post, retract

# Test the specific failing case
p = post('p', 7)
result = balances([p, retract('r', 'p')])
print("Result:", result)
print("Expected: {'cash': 0}")

# Test the specific case that's failing
print("\nTesting the exact case from test:")
try:
    p = post('p', 7)
    result = balances([p, retract('r', 'p')])
    print(f"Result: {result}")
    print(f"Expected: {{'cash': 0}}")
    print(f"Match: {result == {'cash': 0}}")
except Exception as e:
    print(f"Error: {e}")