import unittest
from ledger import positions_for
def post(i, delta): return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}
def retract(i, reference): return {'key': i, 'kind': 'retract', 'reference': reference}
class JournalCases(unittest.TestCase):
    def test_order_and_duplicates(self):
        p = post('p', 7)
        for messages in [[p, retract('r', 'p')], [retract('r', 'p'), p],
                       [p, p, retract('r', 'p'), retract('s', 'p')]]:
            self.assertEqual(positions_for(messages), {'cash': 0})
    def test_independent_postings(self):
        self.assertEqual(positions_for([post('a', 7), post('b', -2), retract('r', 'a')]), {'cash': -2})
        self.assertEqual(positions_for([retract('r', 'missing')]), {})
        self.assertEqual(positions_for([]), {})
