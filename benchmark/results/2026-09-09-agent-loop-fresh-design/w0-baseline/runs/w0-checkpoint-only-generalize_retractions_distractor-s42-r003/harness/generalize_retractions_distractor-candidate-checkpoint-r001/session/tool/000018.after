def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()  # postings that have been cancelled
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting was cancelled by a previous retraction, don't add to total
            if event['id'] in cancelled:
                continue
            postings[event['id']] = (account, amount)
            totals[account] = totals.get(account, 0) + amount
        else:
            target = event['target']
            # Record that this posting is cancelled (if it exists)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                cancelled.add(target)
            else:
                # If target posting doesn't exist yet, mark it for cancellation when it arrives
                cancelled.add(target)
                # Make sure account is in totals even if no posting will happen
                account = event.get('account', 'cash')  # This is wrong approach
                # Actually, let's handle this properly - for retractions we don't know the account
                # but we still need to ensure accounts are tracked
    return totals
