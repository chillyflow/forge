from .money import money

def total(subtotal, discount_rate, shipping):
    discounted_subtotal = subtotal * (1 - discount_rate)
    return money(discounted_subtotal + shipping)
