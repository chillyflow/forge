import unittest
from service import balances

def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

class DebugTest(unittest.TestCase):
    def test_specific_case(self):
        p = post('p', 7)
        events = [p, p, retract('r', 'p'), retract('s', 'p')]
        result = balances(events)
        print("Result:", result)
        self.assertEqual(result, {'cash': 0})

if __name__ == '__main__':
    unittest.main()