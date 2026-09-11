def balances(events):
    seen = set()
    cancelled = set()
    postings = {}
    totals = {}
    
    # First pass: process all events to build up our data structures
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
        else:
            target = event['target']
            cancelled.add(target)
    
    # Second pass: process all events and apply cancellations properly
    seen = set()  # Reset seen for second pass
    totals = {}   # Reset totals
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # Only process if not cancelled
            if event['id'] not in cancelled:
                totals.setdefault(account, 0)
                totals[account] += amount
        else:
            # For retracts, we already marked the target as cancelled in the first pass
            # No need to do anything else here
            pass
    
    return totals
