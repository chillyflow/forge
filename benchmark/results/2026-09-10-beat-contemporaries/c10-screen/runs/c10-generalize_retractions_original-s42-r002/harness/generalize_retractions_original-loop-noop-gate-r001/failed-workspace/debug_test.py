def debug_balances(events):
    print("=== DEBUG: Processing events ===")
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for i, event in enumerate(events):
        print(f"Event {i}: {event}")
        if event['id'] in seen:
            print(f"  Skipping duplicate event {event['id']}")
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
            print(f"  Added posting {event['id']}: {account} +{amount}, total = {totals[account]}")
        else:
            target = event['target']
            print(f"  Processing retraction for target {target}")
            print(f"  Cancelled set before: {cancelled}")
            if target in cancelled:
                print(f"  Target {target} already cancelled, skipping")
                continue
            cancelled.add(target)
            print(f"  Added {target} to cancelled set")
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                print(f"  Cancelled posting {target}: {account} -{amount}, total = {totals[account]}")
                del postings[target]
            else:
                print(f"  Target {target} not found in postings")
        print(f"  Current state - postings: {postings}, totals: {totals}, cancelled: {cancelled}")
    return totals

# Test the failing case
def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

p = post('p', 7)
events = [p, p, retract('r', 'p'), retract('s', 'p')]
print("Testing case:", events)
result = debug_balances(events)
print("Final result:", result)
print("Expected: {'cash': 0}")