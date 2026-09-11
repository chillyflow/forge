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
            # If this posting was already cancelled, skip it
            if event['id'] in cancelled:
                continue
                
            account = event['account']
            amount = event['amount']
            
            # Store the posting
            postings[event['id']] = (account, amount)
            
            # Add to total
            totals.setdefault(account, 0)
            totals[account] += amount
            
        else:  # event['kind'] == 'retract'
            target = event['target']
            
            # If this retract was already processed, skip it
            if target in cancelled:
                continue
                
            # Mark this posting as cancelled
            cancelled.add(target)
            
            # If the posting exists, subtract its amount from the total
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                
    return totals
