#!/usr/bin/env python3

from service import balances
from test_service import post, retract

# Recreate the exact failing test case
def test_exact_case():
    p = post('p', 7)
    events = [p, retract('r', 'p')]
    
    print("Event 1:", p)
    print("Event 2:", retract('r', 'p'))
    print("Events list:", events)
    
    result = balances(events)
    print("Result:", result)
    print("Expected: {'cash': 0}")
    
    # Let's trace step by step
    seen, cancelled = set(), set()
    postings, totals = {}, {}
    
    print("\n--- Tracing execution ---")
    for i, event in enumerate(events):
        print(f"\nEvent {i+1}: {event}")
        print(f"  event['id'] = {event['id']}")
        print(f"  event['kind'] = {event['kind']}")
        
        if event['id'] in seen:
            print("  Already seen, skipping")
            continue
            
        seen.add(event['id'])
        print(f"  Added to seen: {seen}")
        
        if event['kind'] == 'post':
            account, amount = event['account'], event['amount']
            postings[event['id']] = (account, amount)
            print(f"  Added posting: postings[{event['id']}] = ({account}, {amount})")
            print(f"  postings = {postings}")
            
            # Check if this posting has been cancelled by a retraction that arrived earlier
            if event['id'] in cancelled:
                print(f"  Event {event['id']} is in cancelled, skipping")
                continue
                
            totals.setdefault(account, 0)
            totals[account] += amount
            print(f"  Added to totals: totals[{account}] += {amount}")
            print(f"  totals = {totals}")
            
        else:  # retract
            target = event['target']
            print(f"  Target = {target}")
            if target in cancelled:
                print(f"  Target {target} already in cancelled, skipping")
                continue
            cancelled.add(target)
            print(f"  Added target {target} to cancelled: {cancelled}")
            
            if target in postings:
                account, amount = postings[target]
                totals[account] -= amount
                print(f"  Subtracted from totals: totals[{account}] -= {amount}")
                print(f"  totals = {totals}")
            else:
                print(f"  No posting found for target {target}")

    print(f"\nFinal totals: {totals}")

if __name__ == "__main__":
    test_exact_case()