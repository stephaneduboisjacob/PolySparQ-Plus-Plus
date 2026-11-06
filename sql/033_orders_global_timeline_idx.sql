-- Keep the cockpit's cross-instance latest-order view out of a full table sort.
-- Build concurrently so bot order writers can continue during setup_db.sh.
-- PostgreSQL does not allow CREATE INDEX CONCURRENTLY inside a transaction.
CREATE INDEX CONCURRENTLY IF NOT EXISTS orders_ts_created_idx
    ON vp.orders (ts_created DESC);
