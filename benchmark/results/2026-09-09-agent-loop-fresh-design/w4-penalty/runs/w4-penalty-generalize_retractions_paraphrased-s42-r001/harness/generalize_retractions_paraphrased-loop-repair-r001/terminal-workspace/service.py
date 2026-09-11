def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting ID was already cancelled by a retract event, skip it
            if event['id'] in cancelled:
                # Even if cancelled, we still need to ensure the account exists
                totals.setdefault(account, 0)
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # If this retract references an already cancelled posting, ignore it
            if target in cancelled:
                continue
            cancelled.add(target)
            # If the target posting exists, cancel it by subtracting its amount
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
