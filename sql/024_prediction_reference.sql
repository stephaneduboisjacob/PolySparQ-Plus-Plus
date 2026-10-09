-- Reference metadata is deliberately unknown for legacy rows; do not infer
-- a historical market's settlement rule from its duration or today's rules.
ALTER TABLE vp.pm_markets ADD COLUMN IF NOT EXISTS settlement_window_s integer NOT NULL DEFAULT -1;
