def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
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
            # If this retraction is for a posting that already happened, cancel it
            if target in postings and target not in cancelled:
                account, amount = postings[target]
                totals[account] -= amount
                # Mark the posting as cancelled to prevent double processing
                cancelled.add(target)
            else:
                # If the posting hasn't arrived yet, or if it's already been cancelled, 
                # mark the retraction as cancelled to prevent double cancellation
                cancelled.add(target)
    return totals
