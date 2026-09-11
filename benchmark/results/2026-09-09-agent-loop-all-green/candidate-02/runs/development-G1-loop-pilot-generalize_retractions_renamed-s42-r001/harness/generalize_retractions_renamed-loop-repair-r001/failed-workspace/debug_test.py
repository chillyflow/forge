from ledger import positions_for

def post(i, delta): 
    return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}

def retract(i, reference): 
    return {'key': i, 'kind': 'retract', 'reference': reference}

# Test case that's failing
p = post('p', 7)
messages = [p, p, retract('r', 'p'), retract('s', 'p')]
result = positions_for(messages)
print(f"Result: {result}")

# Let's also test the simpler case
messages2 = [p, retract('r', 'p')]
result2 = positions_for(messages2)
print(f"Result2: {result2}")