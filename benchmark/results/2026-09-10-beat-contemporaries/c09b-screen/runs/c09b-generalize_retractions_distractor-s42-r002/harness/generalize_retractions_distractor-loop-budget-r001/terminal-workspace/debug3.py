# Let me directly replicate the exact failing test
import sys
sys.path.insert(0, '.')

# Import the exact same functions as the test
from test_service import post, retract

# Replicate the exact failing test logic
p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]

print("Event structure:")
for i, event in enumerate(events):
    print(f"  {i}: {event}")

print("\nExpected result: {'cash': 0}")

# Import and run the actual function
from service import balances
result = balances(events)
print(f"\nActual result: {result}")

# Test the individual cases from the test
print("\n--- Testing individual cases ---")
print("Case 1: [p, retract('r', 'p')]")
result1 = balances([p, retract('r', 'p')])
print(f"Result: {result1}")

print("\nCase 2: [retract('r', 'p'), p]")
result2 = balances([retract('r', 'p'), p])
print(f"Result: {result2}")

print("\nCase 3: [p, p, retract('r', 'p'), retract('s', 'p')]")
result3 = balances([p, p, retract('r', 'p'), retract('s', 'p')])
print(f"Result: {result3}")