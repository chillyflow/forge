def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()
    
    # Process all events
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:  # event['kind'] == 'retract'
            target = event['target']
            if target in cancelled:
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If target is not in postings, it's an unknown reference - no account created, but no error
    
    return totals
