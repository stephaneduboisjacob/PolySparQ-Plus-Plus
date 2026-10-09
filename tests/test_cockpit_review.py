"""The loss panel must not count partial exits as independent trades."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
from cockpit_review import performance_review, prediction_calibration
import cockpit


class LossReviewTests(unittest.TestCase):
    def fill(self, ident, bot, session, market, direction, price, shares, ts, fee=0):
        return dict(id=ident,client_order_id=str(ident),mode='paper',instance=bot,
                    session_id=session,condition_id=market,token_id='yes',
                    side_label='UP',direction=direction,price=price,
                    size_shares=shares,fee_usd=fee,ts=ts,asset='BTC',
                    horizon_seconds=300,source='strategy' if direction=='buy' else 'settle',
                    execution_mode='maker' if bot=='a' else 'taker')

    def test_partial_exits_one_complete_exposure_and_real_fees(self):
        fills=[self.fill(1,'a','s1','m','buy',.5,10,1,.1),
               self.fill(2,'a','s1','m','sell',1,4,2,.02),
               self.fill(3,'a','s1','m','sell',1,6,3,.03)]
        r=performance_review(fills,0,4)
        self.assertEqual(r['summary']['samples'],1)
        self.assertEqual(r['summary']['wins'],1)
        self.assertAlmostEqual(r['summary']['net'],4.85)
        self.assertAlmostEqual(r['summary']['fees'],.15)
        self.assertEqual(r['recent'][0]['exit_orders'],2)

    def test_shared_market_losses_and_unmatched_exclusions(self):
        fills=[self.fill(1,'a','s1','m','buy',.5,10,1),
               self.fill(2,'a','s1','m','sell',0,10,2),
               self.fill(3,'b','s2','m','buy',.4,10,1),
               self.fill(4,'b','s2','m','sell',0,10,2),
               self.fill(5,'b','s2','other','buy',.3,10,1)]
        r=performance_review(fills,0,3)
        self.assertEqual(r['summary']['samples'],2)
        self.assertEqual(r['summary']['unique_markets'],1)
        self.assertEqual(r['loss_clusters'][0]['instances'],['a','b'])
        self.assertEqual(r['excluded']['open_or_missing_exit'],1)

    def test_cohort_split_uses_entry_time_not_exit_time(self):
        fills=[self.fill(1,'a','s1','old','buy',.5,10,1),
               self.fill(2,'a','s1','old','sell',0,10,9),
               self.fill(3,'a','s1','new','buy',.5,10,8),
               self.fill(4,'a','s1','new','sell',1,10,10)]
        c=performance_review(fills,0,11,7)['cohorts']
        self.assertEqual(c['before']['samples'],1)
        self.assertEqual(c['after']['samples'],1)
        self.assertEqual(c['before']['net'],-5)
        self.assertEqual(c['after']['net'],5)

    def test_api_scope_and_mode_binding(self):
        rows=[self.fill(1,'a','s1','m','buy',.5,10,1),
              self.fill(2,'a','s1','m','sell',0,10,2)]
        with patch.object(cockpit,'q',return_value=rows) as query,patch.object(cockpit,'_review_cache',{}):
            r=cockpit.api_v4_loss_review(1,'a','paper')
        self.assertEqual(r['scope'],'a')
        self.assertEqual(query.call_args.args[1],('a','a','paper','paper'))
        with self.assertRaises(ValueError):cockpit.api_v4_loss_review(1,mode='not-a-mode')

    def test_calibration_uses_exact_order_and_selected_side(self):
        candidates=[{'order_id':'one','instance':'p','asset':'BTC','horizon_seconds':300,
                     'side':'DOWN','outcome':1,'condition_id':'c1'},
                    {'order_id':'two','instance':'p','asset':'BTC','horizon_seconds':300,
                     'side':'UP','outcome':0,'condition_id':'c2'}]
        payloads=[{'client_order_id':'one','fair_up':.2},
                  {'client_order_id':'two','fair_up':.7},
                  {'client_order_id':'unrelated','fair_up':.999}]
        result=prediction_calibration(candidates,payloads)['overall']
        self.assertEqual(result['samples'],2)
        self.assertAlmostEqual(result['mean_predicted'],.75)
        self.assertAlmostEqual(result['realized_win_rate'],.5)
        self.assertAlmostEqual(result['brier_score'],(.2**2+.7**2)/2)


if __name__=='__main__':unittest.main()
