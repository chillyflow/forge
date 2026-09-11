def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    # Track accounts that have been involved in operations
    involved_accounts = set()
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            # Track that this account was involved in an operation
            involved_accounts.add(account)
            # If this posting was already cancelled by an earlier retraction, skip it
            if event['id'] in cancelled:
                continue
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
        else:
            target = event['target']
            # Mark the target as cancelled (even if it hasn't arrived yet)
            cancelled.add(target)
            # If the posting for this target has already arrived, cancel it
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
    # Ensure all accounts that were involved in operations appear in the result
    # even if their final balance is zero
    for account in involved_accounts:
        if account not in totals:
            totals[account] = 0
    return totals
