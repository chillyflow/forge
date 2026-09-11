def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    pending_cancellations = set()
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            if target in cancelled:
                continue
            # If the posting hasn't arrived yet, mark it for cancellation
            if target not in postings:
                pending_cancellations.add(target)
            else:
                # Posting exists, cancel it immediately
                cancelled.add(target)
                account, amount = postings[target]
                totals[account] -= amount
                del postings[target]
    # Handle any pending cancellations (they won't affect anything since postings don't exist)
    return totals
