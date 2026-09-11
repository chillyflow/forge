def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()  # Track which posting IDs have been cancelled
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            # Only add to total if not cancelled
            if event['id'] not in cancelled:
                totals.setdefault(account, 0)
                totals[account] += amount
            else:
                # Even if cancelled, we need to ensure the account is tracked
                totals.setdefault(account, 0)
        else:
            target = event['target']
            if target in cancelled:
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
