import unittest
from service import balances
def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}
class BehaviorChecks(unittest.TestCase):
    def test_order_and_duplicates(self):
        p = post('p', 7)
        for events in [[p, retract('r', 'p')], [retract('r', 'p'), p],
                       [p, p, retract('r', 'p'), retract('s', 'p')]]:
            self.assertEqual(balances(events), {'cash': 0})
    def test_independent_postings(self):
        self.assertEqual(balances([post('a', 7), post('b', -2), retract('r', 'a')]), {'cash': -2})
        self.assertEqual(balances([retract('r', 'missing')]), {})
        self.assertEqual(balances([]), {})
