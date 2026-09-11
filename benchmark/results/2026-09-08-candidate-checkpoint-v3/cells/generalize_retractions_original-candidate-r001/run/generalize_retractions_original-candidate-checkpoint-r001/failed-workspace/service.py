def balances(events):
    seen = set()
    postings = {}
    totals = {}
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
        else:
            target = event['target']
            # If the target posting hasn't arrived yet, remember this retraction
            if target not in postings:
                if target not in pending_retractions:
                    pending_retractions[target] = []
                pending_retractions[target].append(event['id'])
            else:
                # Target posting exists, cancel it
                account, amount = postings[target]
                totals[account] -= amount
                # Remove the posting from our records
                del postings[target]
                # If there were pending retractions for this target, they are now processed
                if target in pending_retractions:
                    del pending_retractions[target]
    return totals
