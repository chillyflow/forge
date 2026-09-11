from service import balances

def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

# Let's manually trace through what should happen
print("=== Manual trace ===")
p = post('p', 7)
print("p =", p)

events = [p, p, retract('r', 'p'), retract('s', 'p')]
print("events =", events)

# Simulate the algorithm step by step
seen = set()
cancelled = set()
postings = {}
totals = {}

for i, event in enumerate(events):
    print(f"\nStep {i+1}: {event}")
    if event['id'] in seen:
        print(f"  -> Duplicate event, skipping")
        continue
    seen.add(event['id'])
    print(f"  -> Added to seen")
    
    if event['kind'] == 'post':
        account, amount = event['account'], event['amount']
        postings[event['id']] = (account, amount)
        totals.setdefault(account, 0)
        totals[account] += amount
        print(f"  -> Posting: {account} +{amount} = {totals[account]}")
    else:
        target = event['target']
        print(f"  -> Retract target: {target}")
        if target in cancelled:
            print(f"  -> Target already cancelled, skipping")
            continue
        cancelled.add(target)
        print(f"  -> Added target to cancelled")
        if target in postings:
            account, amount = postings[target]
            totals[account] -= amount
            print(f"  -> Retracted: {account} -{amount} = {totals[account]}")
        else:
            print(f"  -> No posting found for target {target}")

print(f"\nFinal totals: {totals}")

# Now run the actual function
result = balances(events)
print(f"Actual result: {result}")