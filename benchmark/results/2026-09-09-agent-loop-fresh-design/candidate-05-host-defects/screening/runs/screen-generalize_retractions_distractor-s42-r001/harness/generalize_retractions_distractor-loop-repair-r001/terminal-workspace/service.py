def balances(events):
    seen = set()
    postings, totals = {}, {}
    cancelled = set()  # Track postings that have been cancelled
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            # If this posting was already cancelled by a retraction that arrived earlier, skip it
            if event['id'] in cancelled:
                continue
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            if target in cancelled:
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
