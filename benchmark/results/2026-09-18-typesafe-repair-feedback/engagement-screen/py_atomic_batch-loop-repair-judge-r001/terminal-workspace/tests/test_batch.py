import unittest
from inventory.model import Inventory
from inventory.batch import apply_batch

class BatchTests(unittest.TestCase):
    def test_atomicity(self):
        stock=Inventory({"a":3,"b":1}); self.assertTrue(apply_batch(stock,[("a",-2),("b",2)])); self.assertEqual(stock.quantities,{"a":1,"b":3})
        stock=Inventory({"a":3,"b":1}); self.assertFalse(apply_batch(stock,[("a",-2),("b",-4)])); self.assertEqual(stock.quantities,{"a":3,"b":1})
        self.assertFalse(apply_batch(stock,[("missing",1)])); self.assertEqual(stock.quantities,{"a":3,"b":1})
