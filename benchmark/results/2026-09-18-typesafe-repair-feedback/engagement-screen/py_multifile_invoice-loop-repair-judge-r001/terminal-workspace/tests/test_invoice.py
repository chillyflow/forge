import unittest
from billing.invoice import total

class InvoiceTests(unittest.TestCase):
    def test_order_and_rounding(self):
        self.assertEqual(total(10.05, 0.10, 2.00), 11.05)
        self.assertEqual(total(1.26, 0, 0), 1.26)
