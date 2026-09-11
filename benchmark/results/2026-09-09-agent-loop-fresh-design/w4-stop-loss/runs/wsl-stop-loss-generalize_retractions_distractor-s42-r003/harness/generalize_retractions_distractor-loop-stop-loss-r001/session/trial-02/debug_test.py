from service import balances
from test_service import post, retract

def test_debug():
    p = post('p', 7)
    events = [p, p, retract('r', 'p'), retract('s', 'p')]
    result = balances(events)
    print("Events:", events)
    print("Result:", result)
    print("Expected: {'cash': 0}")

if __name__ == "__main__":
    test_debug()