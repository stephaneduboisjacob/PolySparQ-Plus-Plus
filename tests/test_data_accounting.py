import copy
import math
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from accounting import replay_fills
from refresh_candles import closed_rows
import prediction_board as board


class LedgerTests(unittest.TestCase):
    def fill(self, direction, price, shares, fee=0, **kw):
        return dict(mode='paper',instance='cb',session_id='a',condition_id='c',
                    token_id='up',client_order_id=str(time.monotonic_ns()),direction=direction,
                    price=price,size_shares=shares,fee_usd=fee,**kw)

    def test_partial_exit_reentry_allocates_remaining_cost(self):
        fills = [self.fill('buy',.2,10,.1),self.fill('sell',.4,5,.05),
                 self.fill('buy',.6,5,.1),self.fill('sell',.5,10,.1)]
        r = replay_fills(fills)
        self.assertEqual(r['positions'], [])
        self.assertAlmostEqual(r['exits'][0]['realized_pnl'], .9)
        self.assertAlmostEqual(r['exits'][1]['cost_basis'], 4.15)
        self.assertAlmostEqual(sum(e['realized_pnl'] for e in r['exits']),
                               r['sell_value']-r['buy_value']-r['fees'])

    def test_other_side_bot_session_cannot_subsidize_exit(self):
        base = self.fill('buy',.2,10,.1)
        unrelated = [{**base,'token_id':'down','price':.8},
                     {**base,'instance':'latency','price':.7},
                     {**base,'session_id':'b','price':.9}]
        r = replay_fills([base,*unrelated,self.fill('sell',1,10)])
        self.assertAlmostEqual(r['exits'][0]['realized_pnl'],7.9)
        self.assertEqual(len(r['positions']),3)

    def test_fee_loss_and_unmatched_sells_are_not_wins(self):
        r = replay_fills([self.fill('buy',.5,10,.1),self.fill('sell',.51,10,.1)])
        self.assertAlmostEqual(r['exits'][0]['realized_pnl'],-.1)
        r = replay_fills([self.fill('sell',1,10)])
        self.assertFalse(r['exits'][0]['matched'])
        self.assertTrue(r['errors'])

    def test_live_position_survives_process_session_change(self):
        buy = {**self.fill('buy',.4,10,.1),'mode':'live'}
        sell = {**self.fill('sell',1,10),'mode':'live','session_id':'new-process'}
        r = replay_fills([buy,sell])
        self.assertEqual(r['positions'],[])
        self.assertTrue(r['exits'][0]['matched'])
        self.assertAlmostEqual(r['exits'][0]['realized_pnl'],5.9)


class DataTests(unittest.TestCase):
    def test_only_closed_valid_candles(self):
        raw = [[60000,'100','102','99','101','5',119999],
               [120000,'101','102','100','102','2',179999]]
        self.assertEqual(len(closed_rows('BTCUSDT',raw,120000)),1)
        raw[0][2] = '98'
        with self.assertRaises(ValueError):
            closed_rows('BTCUSDT',raw,120000)

    def test_books_and_missing_fee_fail_closed(self):
        self.assertEqual(board.fee_parameters({}),(-1,1))
        self.assertEqual(board.fee_parameters({'feesEnabled':False}),(0,1))
        self.assertEqual(board.fee_parameters({'feesEnabled':True,'feeSchedule':{'rate':.07}}),(.07,1))
        self.assertEqual(board.fee_parameters({'feeSchedule':{'rate':float('nan')}}),(-1,1))
        crossed = {'bids':[{'price':'.8','size':'10'}], 'asks':[{'price':'.7','size':'10'}]}
        self.assertEqual(board.book_top(crossed),(None,None))
        self.assertEqual(board.book_top({'bids':[{'price':'.5','size':'nan'},
                        {'price':'.6','size':'bad'}]}),(None,None))

    def test_outcome_requires_matching_resolved_complementary_prices(self):
        m = dict(conditionId='c',closed=True,umaResolutionStatus='resolved',
                 outcomes='["Up","Down"]',outcomePrices='["1","0"]')
        self.assertEqual(board.resolved_outcome(m,'c'),1)
        self.assertIsNone(board.resolved_outcome(m,'other'))
        self.assertIsNone(board.resolved_outcome({**m,'closed':False},'c'))
        self.assertIsNone(board.resolved_outcome({**m,'outcomePrices':'["1","1"]'},'c'))

    def test_journal_queries_closed_markets_and_scores(self):
        with tempfile.TemporaryDirectory() as root:
            os.mkdir(root+'/logs')
            now = int(time.time())
            card = dict(model_status='ready',condition_id='c',slug='slug',seconds_left=150,
                        window_close=(now-300)*1000,up_probability=.8,polymarket_up=.7)
            calls = []
            def public(url):
                calls.append(url)
                return [dict(conditionId='c',closed=True,umaResolutionStatus='resolved',
                             outcomes=['Up','Down'],outcomePrices=[1,0])]
            r = board._journal(root,[card],{},public)
            self.assertIn('closed=true',calls[0])
            self.assertEqual(r['resolved_markets'],1)
            self.assertAlmostEqual(r['brier'],.04)
            self.assertAlmostEqual(r['market_brier'],.09)
            db=sqlite3.connect(root+'/logs/prediction_quality.sqlite3')
            row=db.execute('SELECT p,shadow_p,diagnostics_json FROM forecasts WHERE cid=?',('c',)).fetchone()
            self.assertEqual(row[0],.8)
            self.assertEqual(row[1],.8)
            self.assertIn('prediction_basis',row[2])
            db.close()

    def test_market_midpoint_is_primary_and_quote_range_is_bounded(self):
        req={'up_bid':.53,'up_ask':.55,'down_bid':.45,'down_ask':.47}
        market=board._market_probability(req)
        quote=board._market_probability_range(req)
        self.assertAlmostEqual(market,.54)
        self.assertLessEqual(quote[0],market)
        self.assertGreaterEqual(quote[1],market)
        self.assertEqual(board._primary_probability(.71,.61,.82,market,quote),
                         (market,quote[0],quote[1],'polymarket_midpoint'))
        self.assertEqual(board._primary_probability(.71,.61,.82,None,None),
                         (.71,.61,.82,'experimental_settlement_ensemble'))

    def test_lead_lag_alert_requires_fresh_aligned_edge_and_books(self):
        lead=[{'source':'Coinbase','move_bps':8.0,'score':.7,'age_s':1.0}]
        args=('condition',True,.50,.61,'UP',.03,2.0,3.0,lead,1234)
        candidate=board._lead_lag_alert_candidate(*args)
        self.assertEqual(candidate['side'],'UP')
        self.assertEqual(candidate['model_market_gap_pp'],11.0)
        self.assertIsNone(board._lead_lag_alert_candidate(
            *args[:6],8.0,args[7],args[8],args[9]))
        self.assertIsNone(board._lead_lag_alert_candidate(
            args[0],args[1],args[2],args[3],args[4],.01,*args[6:]))
        self.assertIsNone(board._lead_lag_alert_candidate(
            *args[:8],[],args[9]))

    def test_snapshot_invalidation_at_rollover_and_stale_refresh(self):
        card = dict(window_open=100000,window_close=400000,model_status='ready',
                    direction='UP',model_edge=.1,conservative_edge=.08,opportunity={'level':'alert'},
                    alert_candidate={'id':'c:UP'},reference_age_s=2,book_age_s=1)
        card['next_window'] = {**card,'provisional':True,'window_open':400000,'window_close':700000}
        original = dict(generated_at=390000,inputs_at=390000,cards=[card])
        r = board.age_board(original,401000)
        self.assertTrue(r['stale'])
        self.assertEqual(r['cards'][0]['direction'],'WAIT')
        self.assertEqual(r['cards'][0]['next_window']['model_status'],'awaiting_open_reference')
        self.assertIsNone(r['cards'][0]['opportunity'])
        self.assertIsNone(r['cards'][0]['alert_candidate'])
        self.assertEqual(original['cards'][0]['direction'],'UP')

    def test_slow_producer_does_not_block_readers(self):
        entered, release = threading.Event(),threading.Event()
        def build(*args,**kw):
            entered.set();release.wait(3)
            return {'cards':[],'generated_at':int(time.time()*1000),'errors':[]}
        service = board.PredictionService(None,None,'/nonexistent',lambda:{},interval=.1)
        with patch.object(board,'build_board',build),patch.object(board,'resolve_pending'):
            try:
                service.start()
                self.assertTrue(entered.wait(1))
                t = time.monotonic()
                for _ in range(100):
                    self.assertTrue(service.snapshot()['warming_up'])
                self.assertLess(time.monotonic()-t,.1)
            finally:
                service.stop();release.set()
                for thread in service._threads:
                    thread.join(2)

    def test_prediction_refresh_interval_accepts_only_supported_rates(self):
        service = board.PredictionService(None, None, '/nonexistent', lambda: {}, interval=5)
        service.set_interval(2)
        self.assertEqual(service.interval, 2)
        self.assertTrue(service._refresh_now.is_set())
        service._refresh_now.clear()
        service.set_interval(3)
        self.assertEqual(service.interval, 2)
        self.assertFalse(service._refresh_now.is_set())


if __name__ == '__main__':
    unittest.main()
