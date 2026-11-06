-- Keep per-session PAPER P&L and position-history reads indexed as the
-- cockpit refreshes snapshots across the bot fleet.
BEGIN;
SET LOCAL lock_timeout='3s';
ALTER TABLE vp.orders ADD COLUMN IF NOT EXISTS session_id uuid;
COMMIT;

-- The P&L ledger joins current sessions to their filled orders, while recovery
-- accounting scans older orders for the same instance. This partial index
-- keeps both paths off a whole-table orders/fills scan.
CREATE INDEX CONCURRENTLY IF NOT EXISTS orders_paper_instance_session_idx
    ON vp.orders(instance,session_id,client_order_id)
    WHERE mode='paper' AND session_id IS NOT NULL;

-- Match the session partition and timestamp ordering used by the cockpit's
-- bounded equity-curve snapshot query.
CREATE INDEX CONCURRENTLY IF NOT EXISTS positions_history_instance_session_ts_idx
    ON vp.positions_history(instance,session_id,ts DESC)
    WHERE session_id IS NOT NULL;
