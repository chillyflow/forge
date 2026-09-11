def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            # If this posting was already marked as cancelled, don't add to total
            if event['id'] not in cancelled:
                totals.setdefault(account, 0)
                totals[account] += amount
            # Ensure account is initialized even if posting is cancelled
            else:
                totals.setdefault(account, 0)
        else:
            target = event['target']
            if target in cancelled:
                continue
            # Mark the target posting as cancelled
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
