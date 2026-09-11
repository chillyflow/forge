def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # If this posting ID is already cancelled, skip it
            if event['id'] in cancelled:
                # Even if we skip the posting, we should still track the account
                totals.setdefault(account, 0)
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # If the target is already in postings, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                # Remove the posting from postings dict to ensure credit contributes at most once
                del postings[target]
            else:
                # If the target is not in postings, mark it as cancelled for future postings
                cancelled.add(target)
    return totals
