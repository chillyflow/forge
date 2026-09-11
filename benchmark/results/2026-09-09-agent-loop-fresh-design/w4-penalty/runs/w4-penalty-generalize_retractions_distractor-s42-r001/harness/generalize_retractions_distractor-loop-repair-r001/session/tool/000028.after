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
            # If the posting has already been cancelled, ignore this retraction
            if target in cancelled:
                continue
            # Mark this posting as cancelled (even if it hasn't been posted yet)
            cancelled.add(target)
            # If there's a corresponding posting, cancel it and adjust balance
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
