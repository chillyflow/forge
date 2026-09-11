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
            # Only add to total if not cancelled
            if event['id'] not in cancelled:
                totals[account] += amount
        else:
            target = event['target']
            # Mark target as cancelled
            cancelled.add(target)
            # If target was already posted, subtract it from total
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If target was not posted, no account is created (unknown reference)
    return totals
