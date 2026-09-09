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
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If the target posting hasn't arrived yet, we still mark it as cancelled
            # but don't modify totals since there's no posting to cancel
    return totals
