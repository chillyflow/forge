def balances(events):
    seen = set()
    cancelled = set()
    postings = {}
    totals = {}
    
    for event in events:
        # Skip duplicate events
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            # Process posting
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            # Process retraction
            target = event['target']
            # If this retraction has already been processed, skip it
            if target in cancelled:
                continue
            cancelled.add(target)
            # If the posting exists, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
