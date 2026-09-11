#!/usr/bin/env python3

# Let's trace through the exact execution of the failing case
def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    print(f"Processing events: {events}")
    for i, event in enumerate(events):
        print(f"Event {i}: {event}")
        if event['id'] in seen:
            print(f"  -> Skipping (already seen)")
            continue
        seen.add(event['id'])
        print(f"  -> Added to seen")
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            print(f"  -> Posting: account={account}, amount={amount}")
            # If this posting has been cancelled already, skip it
            if event['id'] in cancelled:
                print(f"  -> Skipping (cancelled)")
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
            print(f"  -> Added to postings and totals: {totals}")
        else:
            target = event['target']
            print(f"  -> Retract targeting: {target}")
            if target in cancelled:
                print(f"  -> Skipping (already cancelled)")
                continue
            cancelled.add(target)
            print(f"  -> Added to cancelled set: {cancelled}")
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                print(f"  -> Subtracted from totals: {totals}")
            else:
                print(f"  -> Target not in postings, no adjustment")
    print(f"Final result: {totals}")
    return totals

# Test case that's failing
def post(i, amount): 
    return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}

def retract(i, target): 
    return {'id': i, 'kind': 'retract', 'target': target}

p = post('p', 7)
result = balances([retract('r', 'p'), p])
print(f"Final result: {result}")