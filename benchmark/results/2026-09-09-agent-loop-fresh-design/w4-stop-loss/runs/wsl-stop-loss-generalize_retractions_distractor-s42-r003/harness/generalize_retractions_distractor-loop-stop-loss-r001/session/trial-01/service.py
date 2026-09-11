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
            # If target posting hasn't been seen yet, we'll handle it when it arrives
            if target not in postings:
                # Store the retraction for later processing when the posting arrives
                continue
            # If the target has already been cancelled, skip
            if target in cancelled:
                continue
            cancelled.add(target)
            account, amount = postings[target]
            totals[account] -= amount
    return totals
