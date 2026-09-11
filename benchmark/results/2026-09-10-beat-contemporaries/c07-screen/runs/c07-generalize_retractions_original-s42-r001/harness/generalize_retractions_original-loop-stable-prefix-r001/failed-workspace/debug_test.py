def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    print("Processing events:", events)
    for i, event in enumerate(events):
        print(f"Event {i}: {event}")
        if event['id'] in seen:
            print(f"  Skipping duplicate ID {event['id']}")
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
            print(f"  Added posting: {account} + {amount}, total: {totals[account]}")
        else:
            target = event['target']
            print(f"  Processing retraction targeting {target}")
            if target in cancelled:
                print(f"  Target {target} already cancelled, skipping")
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                print(f"  Cancelled posting: {account} - {amount}, total: {totals[account]}")
                del postings[target]
            else:
                print(f"  No posting found for target {target}, just marking as cancelled")
    print("Final totals:", totals)
    return totals

# Test the failing case
def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}
def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}

p = post('p', 7)
result = balances([p, p, retract('r', 'p'), retract('s', 'p')])
print("Result:", result)