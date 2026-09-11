def balances(events):
    seen = set()
    cancelled = set()
    postings = {}
    totals = {}
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting has been cancelled by a retraction, skip it
            if event['id'] in cancelled:
                # Even if cancelled, ensure the account exists in totals
                totals.setdefault(account, 0)
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:  # event['kind'] == 'retract'
            target = event['target']
            # If the target has already been cancelled, skip this retraction
            if target in cancelled:
                continue
            cancelled.add(target)
            # If the posting has already arrived, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
