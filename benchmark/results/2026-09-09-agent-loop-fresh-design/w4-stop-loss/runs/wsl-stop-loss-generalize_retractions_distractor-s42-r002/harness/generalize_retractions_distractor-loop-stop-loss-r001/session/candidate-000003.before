def balances(events):
    seen, cancelled = set(), set()
    postings, totals = {}, {}
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
            # Process any pending retractions for this posting
            if event['id'] in pending_retractions:
                for retraction_id in pending_retractions[event['id']]:
                    cancelled.add(event['id'])
                    totals[account] -= amount
                del pending_retractions[event['id']]
        else:
            target = event['target']
            # If target posting hasn't been seen yet, store this retraction for later
            if target not in postings:
                if target not in pending_retractions:
                    pending_retractions[target] = []
                pending_retractions[target].append(event['id'])
            # If the target has already been cancelled, skip
            elif target in cancelled:
                continue
            else:
                cancelled.add(target)
                account, amount = postings[target]
                totals[account] -= amount
    return totals
