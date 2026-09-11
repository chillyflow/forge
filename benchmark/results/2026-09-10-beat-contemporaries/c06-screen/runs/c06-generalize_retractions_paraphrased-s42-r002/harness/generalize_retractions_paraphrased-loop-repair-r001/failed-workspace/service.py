def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting was already cancelled, don't process it
            if event['id'] in cancelled:
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # If target is already cancelled, we don't need to do anything
            if target in cancelled:
                continue
            # Mark target as cancelled (this handles the case where retract comes before posting)
            cancelled.add(target)
            # If target posting already exists, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
