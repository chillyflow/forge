def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    
    for message in messages:
        if message['kind'] == 'post':
            # If this post has been voided by a retraction, skip it
            if message['key'] in voided:
                continue
            bucket, delta = message['bucket'], message['delta']
            # If this post already exists, we don't add it again (idempotent)
            if message['key'] not in entries:
                entries[message['key']] = (bucket, delta)
                positions.setdefault(bucket, 0)
                positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            # A retraction cancels its referenced posting exactly once, even if it arrives before that posting
            # So we mark the reference as voided to prevent future posts from being added
            # If the referenced post exists, cancel it
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
    
    return positions
