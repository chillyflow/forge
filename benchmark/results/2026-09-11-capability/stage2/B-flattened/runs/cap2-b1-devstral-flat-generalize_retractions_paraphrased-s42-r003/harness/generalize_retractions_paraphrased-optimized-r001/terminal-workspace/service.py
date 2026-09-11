def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    
    # First pass: process all post events
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
    
    # Second pass: process all retract events
    seen.clear()
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'retract':
            target = event['target']
            if target in cancelled:
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    
    return totals
