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
            if target in cancelled:
                continue
            # If the target posting hasn't arrived yet, ignore this retraction permanently
            if target not in postings:
                cancelled.add(target)
                continue
            cancelled.add(target)
            account, amount = postings[target]
            totals[account] -= amount
    return totals
