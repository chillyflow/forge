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
            # Mark the target posting as cancelled so if it arrives later, it won't be processed
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
