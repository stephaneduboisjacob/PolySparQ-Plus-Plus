-- Migration imports created valid per-year primary keys without attaching them
-- to the parent index. The invalid parent makes ON CONFLICT(symbol,ts) fail.
-- Reuse the existing unique indexes; no archive rewrite or index rebuild.
BEGIN;
SET LOCAL lock_timeout = '3s';
DO $$
DECLARE child record;
BEGIN
    FOR child IN
        SELECT ns.nspname, idx.relname
        FROM pg_inherits part
        JOIN pg_index ix ON ix.indrelid=part.inhrelid
        JOIN pg_class idx ON idx.oid=ix.indexrelid
        JOIN pg_namespace ns ON ns.oid=idx.relnamespace
        WHERE part.inhparent='vp.candles_1m'::regclass AND ix.indisprimary
          AND NOT EXISTS (SELECT 1 FROM pg_inherits attached
                          WHERE attached.inhrelid=ix.indexrelid)
    LOOP
        EXECUTE format('ALTER INDEX vp.candles_1m_pkey ATTACH PARTITION %I.%I',
                       child.nspname,child.relname);
    END LOOP;
    IF NOT (SELECT indisvalid FROM pg_index WHERE indexrelid='vp.candles_1m_pkey'::regclass) THEN
        RAISE EXCEPTION 'Candle parent primary key remains invalid';
    END IF;
END $$;
COMMIT;
