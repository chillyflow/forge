def balances(events):
    seen, cancelled = set(), set()
    postings, retractions = {}, {}
    all_accounts = set()
    
    # First pass: collect all postings and retractions, and track all accounts
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            all_accounts.add(account)
        else:
            target = event['target']
            retractions[target] = event['id']
    
    # Second pass: apply retractions to cancel postings
    for target, retraction_id in retractions.items():
        if target in postings and target not in cancelled:
            cancelled.add(target)
    
    # Third pass: calculate final totals for all accounts that had postings
    totals = {}
    for account in all_accounts:
        totals[account] = 0
    
    for posting_id, (account, amount) in postings.items():
        if posting_id not in cancelled:
            totals[account] += amount
    
    return totals
