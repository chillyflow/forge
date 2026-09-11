def balances(events):
    seen = set()
    cancelled = set()
    postings = {}
    totals = {}
    
    for event in events:
        # Process each globally unique message once
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            # If this posting has been cancelled, skip it
            if event['id'] in cancelled:
                continue
                
            account = event['account']
            amount = event['amount']
            
            # Record this posting
            postings[event['id']] = (account, amount)
            
            # Update total for this account
            totals.setdefault(account, 0)
            totals[account] += amount
            
        else:  # event['kind'] == 'retract'
            target = event['target']
            
            # If target has already been cancelled, skip
            if target in cancelled:
                continue
                
            # Mark target as cancelled
            cancelled.add(target)
            
            # If the target posting exists, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                
    return totals
