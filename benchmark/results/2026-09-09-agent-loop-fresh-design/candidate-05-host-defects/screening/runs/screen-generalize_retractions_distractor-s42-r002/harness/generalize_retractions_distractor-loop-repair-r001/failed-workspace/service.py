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
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # If this retraction has already been processed, skip it
            if target in cancelled:
                continue
            cancelled.add(target)
            # If the posting has already arrived, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
            # If posting hasn't arrived yet, we still need to track the account
            # because the retraction will cancel a posting that would have involved this account
    return totals
