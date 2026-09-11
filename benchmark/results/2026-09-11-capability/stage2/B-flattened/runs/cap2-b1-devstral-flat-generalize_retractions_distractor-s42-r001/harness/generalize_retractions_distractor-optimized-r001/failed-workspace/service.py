def balances(events):
    seen = set()
    cancelled = set()
    totals = {}
    
    # First pass: collect all postings and retractions
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            totals.setdefault(account, 0)
        else:
            cancelled.add(event['target'])
    
    # Second pass: calculate totals, applying only non-cancelled postings
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            if event['id'] not in cancelled:
                account, amount = event['account'], event['amount']
                totals[account] += amount
    
    return totals
