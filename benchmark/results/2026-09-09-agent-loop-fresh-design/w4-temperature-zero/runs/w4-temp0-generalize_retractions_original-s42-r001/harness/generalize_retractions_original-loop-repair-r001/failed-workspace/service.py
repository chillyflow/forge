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
            if event['id'] not in cancelled:
                totals.setdefault(account, 0)
                totals[account] += amount
        else:
            target = event['target']
            if target in cancelled:
                continue
            if target in postings:
                cancelled.add(target)
                account, amount = postings[target]
                totals[account] -= amount
            else:
                # If target is not in postings, mark it as cancelled (unmatched retraction)
                # Even unmatched retractions should result in zero balance for the account
                # We need to track which accounts are involved in retractions
                cancelled.add(target)
                # For unmatched retractions, we still need to ensure account appears in totals
                # But we don't know the account from the retraction, so we'll track it differently
                # Actually, let's just not do anything for unmatched retractions - they have no balance effect
                pass
    return totals
