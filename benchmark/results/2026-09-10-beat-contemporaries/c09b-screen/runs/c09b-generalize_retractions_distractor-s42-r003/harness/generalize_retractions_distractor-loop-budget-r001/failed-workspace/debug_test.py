def debug_balances(events):
    print("Processing events:", events)
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for i, event in enumerate(events):
        print(f"Event {i}: {event}")
        if event['id'] in seen:
            print(f"  Skipping duplicate event {event['id']}")
            continue
        seen.add(event['id'])
        print(f"  Added {event['id']} to seen")
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
            print(f"  Posted: {account} += {amount}, total = {totals[account]}")
        else:
            target = event['target']
            print(f"  Retracting target: {target}")
            if target in cancelled:
                print(f"  Target {target} already cancelled, skipping")
                continue
            cancelled.add(target)
            print(f"  Added {target} to cancelled")
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                print(f"  Cancelled: {account} -= {amount}, total = {totals[account]}")
            else:
                print(f"  No posting found for target {target}")
        print(f"  Current totals: {totals}")
    return totals

# Test the failing case
def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
result = debug_balances(events)
print("Final result:", result)