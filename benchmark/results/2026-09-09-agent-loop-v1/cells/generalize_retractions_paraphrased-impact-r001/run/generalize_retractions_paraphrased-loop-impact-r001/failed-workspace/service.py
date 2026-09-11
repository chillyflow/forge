def balances(events):
    # Track all posts by ID
    posts = {}
    # Track which posts have been cancelled
    cancelled = set()
    
    # First pass: collect all posts and retracts
    for event in events:
        if event['kind'] == 'post':
            posts[event['id']] = event
        else:  # retract
            cancelled.add(event['target'])
    
    # Second pass: process only non-cancelled posts
    totals = {}
    for event in events:
        if event['kind'] == 'post' and event['id'] not in cancelled:
            account, amount = event['account'], event['amount']
            totals.setdefault(account, 0)
            totals[account] += amount
    
    return totals
