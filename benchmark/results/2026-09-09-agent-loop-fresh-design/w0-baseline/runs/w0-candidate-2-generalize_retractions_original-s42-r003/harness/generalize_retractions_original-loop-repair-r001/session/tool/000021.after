def balances(events):
    # Track all events we've seen (idempotency)
    seen = set()
    
    # Track postings by their event ID
    postings = {}
    
    # Track total balances by account
    totals = {}
    
    # Track which postings have been cancelled
    cancelled = set()
    
    for event in events:
        # Skip if we've already processed this event ID (idempotency)
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            # Process posting
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            # Only add to total if this posting hasn't been cancelled
            if event['id'] not in cancelled:
                totals[account] += amount
        else:
            # Process retraction
            target = event['target']
            # If this target has already been cancelled, skip
            if target in cancelled:
                continue
            # Mark this target as cancelled
            cancelled.add(target)
            # If we have the posting, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    
    return totals
