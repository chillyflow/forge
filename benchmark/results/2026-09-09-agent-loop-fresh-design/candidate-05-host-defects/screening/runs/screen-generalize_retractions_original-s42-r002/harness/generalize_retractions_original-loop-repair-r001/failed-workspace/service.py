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
            # A retraction should cancel its target exactly once, even if it arrives before the posting
            # Mark that this retraction has been processed for this target
            if target in cancelled:
                continue
            cancelled.add(target)
            # If the posting for this target already exists, cancel it now
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
