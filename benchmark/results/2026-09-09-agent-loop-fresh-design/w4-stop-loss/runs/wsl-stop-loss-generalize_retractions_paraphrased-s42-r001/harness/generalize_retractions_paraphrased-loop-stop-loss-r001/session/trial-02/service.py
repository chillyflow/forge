def balances(events):
    # Track all events by ID to ensure each is processed once
    seen = set()
    # Track cancelled postings (void notices)
    cancelled = set()
    # Track postings by ID to know what to cancel
    postings = {}
    # Track final balances
    totals = {}
    
    for event in events:
        # Process each globally unique message once
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            # Store the posting
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            # Add to total
            totals.setdefault(account, 0)
            totals[account] += amount
        else:  # event['kind'] == 'retract'
            target = event['target']
            # If this retract is already cancelled, skip it
            if target in cancelled:
                continue
            # Mark this target as cancelled
            cancelled.add(target)
            # If the target posting exists, remove its amount from the balance
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                # Remove the posting from postings dict to ensure it contributes at most once
                del postings[target]
    
    return totals
