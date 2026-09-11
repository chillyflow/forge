def balances(events):
    # First pass: collect all cancellations
    cancelled = set()
    for event in events:
        if event['kind'] == 'retract':
            cancelled.add(event['target'])
    
    # Second pass: process postings and apply cancellations
    totals = {}
    processed_postings = set()
    
    for event in events:
        if event['id'] in processed_postings:
            continue
        processed_postings.add(event['id'])
        
        if event['kind'] == 'post':
            # Only process if not cancelled
            if event['id'] not in cancelled:
                account, amount = event['account'], event['amount']
                totals.setdefault(account, 0)
                totals[account] += amount
        # For retractions, they don't contribute to totals directly
    
    return totals
