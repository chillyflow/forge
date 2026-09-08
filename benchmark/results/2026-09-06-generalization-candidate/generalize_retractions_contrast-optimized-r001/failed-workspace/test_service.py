import unittest
from service import balances
def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}
class Retractions(unittest.TestCase):
    def test_arrival_order(self):
        p = post('p', 7)
        self.assertEqual(balances([retract('early', 'p'), p]), {'cash': 7})
        self.assertEqual(balances([retract('early', 'p'), p, retract('later', 'p')]), {'cash': 0})
        self.assertEqual(balances([p, p, retract('r', 'p'), retract('s', 'p')]), {'cash': 0})
        self.assertEqual(balances([retract('same', 'p'), p, retract('same', 'p')]), {'cash': 7})
        self.assertEqual(balances([retract('r', 'missing')]), {})
        self.assertEqual(balances([]), {})
