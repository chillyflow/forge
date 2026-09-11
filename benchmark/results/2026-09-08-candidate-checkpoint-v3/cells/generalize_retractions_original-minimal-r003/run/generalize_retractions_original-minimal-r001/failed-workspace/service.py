def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # If this posting has already been cancelled, skip
            if target in cancelled:
                continue
            # Mark this posting as cancelled (so it can only be cancelled once)
            cancelled.add(target)
            # If the posting exists, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If the posting doesn't exist, the retraction has no effect (as per requirements)
    return totals
