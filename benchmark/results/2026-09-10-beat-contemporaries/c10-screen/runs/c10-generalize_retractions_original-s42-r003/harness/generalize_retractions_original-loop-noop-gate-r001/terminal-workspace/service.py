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
            # Only add to total if not cancelled
            if event['id'] not in cancelled:
                totals.setdefault(account, 0)
                totals[account] += amount
            # Ensure account is in totals even if posting is cancelled
            else:
                totals.setdefault(account, 0)
        else:
            target = event['target']
            if target in cancelled:
                continue
            cancelled.add(target)
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    return totals
