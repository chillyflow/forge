def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting was already cancelled by a retraction, skip it
            if event['id'] in cancelled:
                continue
            postings[event['id']] = (account, amount)
            totals[account] = totals.get(account, 0) + amount
        else:
            target = event['target']
            # If target posting was already cancelled, ignore this retraction
            if target in cancelled:
                continue
            # Mark the target as cancelled
            cancelled.add(target)
            # If the target posting exists, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If target posting doesn't exist, the retraction is unmatched and has no effect
    return totals
