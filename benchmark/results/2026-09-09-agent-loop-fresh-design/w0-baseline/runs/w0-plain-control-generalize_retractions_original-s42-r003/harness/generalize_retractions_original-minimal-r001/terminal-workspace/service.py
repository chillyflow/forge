def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting was already marked for cancellation, skip it
            if event['id'] in cancelled:
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # A retraction cancels its referenced posting exactly once
            if target not in cancelled:
                # Mark the target as cancelled to prevent double cancellation
                cancelled.add(target)
                # If the target posting has already been processed, cancel it
                if target in postings:
                    account, amount = postings[target]
                    totals[account] -= amount
                    # Remove the posting from postings dict to ensure it's only cancelled once
                    del postings[target]
    
    return totals
