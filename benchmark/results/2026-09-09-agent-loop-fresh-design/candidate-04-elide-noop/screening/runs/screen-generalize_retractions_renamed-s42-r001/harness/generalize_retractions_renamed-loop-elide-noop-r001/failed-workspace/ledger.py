def positions_for(messages):
    entries, positions = {}, {}
    voided = set()
    # Track buckets that have been involved in operations (even if voided)
    involved_buckets = set()
    
    for message in messages:
        if message['kind'] == 'post':
            bucket, delta = message['bucket'], message['delta']
            # If this post was already retracted, skip it
            if message['key'] in voided:
                continue
            entries[message['key']] = (bucket, delta)
            positions.setdefault(bucket, 0)
            positions[bucket] += delta
            involved_buckets.add(bucket)
        else:  # message['kind'] == 'retract'
            reference = message['reference']
            # Mark this retraction as processed
            if reference in voided:
                continue
            voided.add(reference)
            # If the referenced post exists, subtract its delta
            if reference in entries:
                bucket, delta = entries[reference]
                positions[bucket] -= delta
                del entries[reference]
            else:
                # This retraction voids a post that hasn't arrived yet
                # Track the bucket that would be affected
                involved_buckets.add(message['bucket'])
    
    # Ensure all involved buckets appear in the result with their final values
    # (even if they end up with zero, they should appear)
    result = {}
    for bucket in involved_buckets:
        result[bucket] = positions.get(bucket, 0)
    
    return result
