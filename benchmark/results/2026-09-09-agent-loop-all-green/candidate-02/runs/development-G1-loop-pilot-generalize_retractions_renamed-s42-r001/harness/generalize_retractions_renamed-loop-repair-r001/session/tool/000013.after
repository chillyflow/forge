from ledger import positions_for

def post(i, delta): 
    return {'key': i, 'kind': 'post', 'bucket': 'cash', 'delta': delta}

def retract(i, reference): 
    return {'key': i, 'kind': 'retract', 'reference': reference}

# Test the three cases individually to understand what's failing
p = post('p', 7)

# Case 1: [p, retract('r', 'p')]
messages1 = [p, retract('r', 'p')]
result1 = positions_for(messages1)
print(f"Case 1 [p, retract('r', 'p')]: {result1}")

# Case 2: [retract('r', 'p'), p]
messages2 = [retract('r', 'p'), p]
result2 = positions_for(messages2)
print(f"Case 2 [retract('r', 'p'), p]: {result2}")

# Case 3: [p, p, retract('r', 'p'), retract('s', 'p')]
messages3 = [p, p, retract('r', 'p'), retract('s', 'p')]
result3 = positions_for(messages3)
print(f"Case 3 [p, p, retract('r', 'p'), retract('s', 'p')]: {result3}")