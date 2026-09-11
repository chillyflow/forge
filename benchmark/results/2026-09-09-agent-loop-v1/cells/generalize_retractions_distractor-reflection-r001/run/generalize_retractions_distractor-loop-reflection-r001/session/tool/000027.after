def balances(events):
    seen = set()
    postings = {}
    totals = {}
    cancelled = set()
    retraction_queue = {}
    
    for event in events:
        if event['id'] in seen:
            continue
        seen.add(event['id'])
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            totals[account] = totals.get(account, 0) + amount
            # Process any queued retractions for this posting
            if event['id'] in retraction_queue:
                for retraction in retraction_queue[event['id']]:
                    cancelled.add(retraction['id'])
                    account, amount = postings[event['id']]
                    totals[account] -= amount
                del retraction_queue[event['id']]
        else:
            target = event['target']
            # Check if this retraction has already cancelled a posting
            if target in cancelled:
                continue
            # Check if the target posting has already been processed
            if target in postings:
                account, amount = postings[target]
                cancelled.add(event['id'])
                totals[account] -= amount
            else:
                # Queue this retraction for when the target posting arrives
                if target not in retraction_queue:
                    retraction_queue[target] = []
                retraction_queue[target].append(event)
    return totals
