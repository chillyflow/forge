def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    # Track retractions that reference postings not yet seen
    pending_retractions = {}
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals.setdefault(account, 0)
            totals[account] += amount
            # Check if this posting was referenced by a pending retraction
            if event['id'] in pending_retractions:
                # Cancel the posting
                for retraction_id in pending_retractions[event['id']]:
                    if retraction_id in cancelled:
                        continue
                    cancelled.add(retraction_id)
                    totals[account] -= amount
                del pending_retractions[event['id']]
        else:
            target = event['target']
            if target in cancelled:
                continue
            # If the target posting hasn't been seen yet, store this retraction
            if target not in postings:
                if target not in pending_retractions:
                    pending_retractions[target] = []
                pending_retractions[target].append(event['id'])
            else:
                # Target posting exists, so cancel it immediately
                cancelled.add(event['id'])
                account, amount = postings[target]
                totals[account] -= amount
    return totals
