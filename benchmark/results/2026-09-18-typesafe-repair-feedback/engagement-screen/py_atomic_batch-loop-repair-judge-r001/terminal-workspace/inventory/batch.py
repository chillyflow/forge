def apply_batch(inventory, adjustments):
    # First, validate all adjustments
    for sku, delta in adjustments:
        if sku not in inventory.quantities:
            return False
        new_quantity = inventory.quantities[sku] + delta
        if new_quantity < 0:
            return False
    
    # If all validations pass, apply all adjustments
    for sku, delta in adjustments:
        inventory.quantities[sku] += delta
    return True
