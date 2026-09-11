def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    pending_retractions = set()  # Track postings that have been canceled by retractions
    
    for message in messages:
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            key = message['key']
            # If this posting was canceled by a retraction, don't add it
            if key in pending_retractions:
                # Even though we skip adding it, we should still initialize the bucket to 0
                positions[bucket] = positions.get(bucket, 0)
                continue
            entries[key] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            if reference in voided:
                continue
            voided.add(reference)
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
            else:
                # This retraction references a posting that doesn't exist yet
                # Mark that posting as canceled (will be ignored when it arrives)
                pending_retractions.add(reference)
    
    return positions
