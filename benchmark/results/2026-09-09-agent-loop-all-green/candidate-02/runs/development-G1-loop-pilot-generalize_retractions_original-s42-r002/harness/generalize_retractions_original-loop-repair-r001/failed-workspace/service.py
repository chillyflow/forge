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
            account = event['account']
            amount = event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # Skip if this retraction has already been processed
            if target in cancelled:
                continue
            # If we have the posting, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                cancelled.add(target)
            else:
                # If posting hasn't arrived yet, mark retraction as cancelled
                cancelled.add(target)
    return totals
