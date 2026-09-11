def balances(events):
    postings = {}
    totals = {}
    cancelled = set()
    
    # Process all events to track postings and cancellations
    for event in events:
        if event['kind'] == 'post':
            postings[event['id']] = (event['account'], event['amount'])
        else:  # event['kind'] == 'retract'
            cancelled.add(event['target'])
    
    # Process all postings, but only add to totals if not cancelled
    for event_id, (account, amount) in postings.items():
        totals.setdefault(account, 0)
        if event_id not in cancelled:
            totals[account] += amount
    
    # Remove accounts that have zero balance
    return {acc: bal for acc, bal in totals.items() if bal != 0}
