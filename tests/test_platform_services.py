import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
from cockpit_analytics import build_analytics,live_position_marks
from cockpit_platform import PlatformStore,run_research


class LedgerAnalyticsTests(unittest.TestCase):
    def fill(self,ident,instance,session,direction,price,shares,fee,ts):
        return dict(id=ident,client_order_id=str(ident),instance=instance,session_id=session,
                    mode='paper',condition_id='same-market',token_id='yes',asset='BTC',
                    direction=direction,price=price,size_shares=shares,fee_usd=fee,ts=ts,
                    source='strategy' if direction=='buy' else 'settle',execution_mode='maker')

    def test_partial_exit_fees_and_instance_scope(self):
        rows=[self.fill(1,'a','one','buy',.5,10,.2,1),
              self.fill(2,'b','one','buy',.8,10,.1,2),
              self.fill(3,'a','one','sell',.7,5,.1,3),
              self.fill(4,'b','one','sell',0,10,0,4)]
        result=build_analytics(rows,0,5)
        by_instance={r['instance']:r for r in result['instances']}
        self.assertAlmostEqual(by_instance['a']['net'],.8)
        self.assertAlmostEqual(by_instance['b']['net'],-8.1)
        self.assertEqual(result['summary']['trades'],2)
        self.assertEqual(result['excluded_unmatched_exits'],0)

    def test_paper_session_boundary_excludes_unmatched_exit(self):
        rows=[self.fill(1,'a','one','buy',.5,10,0,1),
              self.fill(2,'a','two','sell',1,10,0,2)]
        result=build_analytics(rows,0,5)
        self.assertEqual(result['summary']['net'],0)
        self.assertEqual(result['excluded_unmatched_exits'],1)

    def test_exit_window_retains_earlier_entry_cost(self):
        rows=[self.fill(1,'a','one','buy',.7,10,.1,1),self.fill(2,'a','one','sell',1,10,0,9)]
        result=build_analytics(rows,8,10)
        self.assertAlmostEqual(result['summary']['net'],2.9)


class PositionMarkTests(unittest.TestCase):
    def board(self):
        return {'snapshot_age_s':2,'stale':False,'cards':[{'period':'CURRENT','condition_id':'market','window_close':20000,'book_age_s':3,'polymarket_up_bid':.4,'polymarket_up_price':.5,'polymarket_down_bid':.5,'polymarket_down_price':.6}]}

    def test_exact_market_side_executable_bid(self):
        marks=live_position_marks(self.board(),10000)
        self.assertEqual(marks[('market','UP')]['bid'],.4)
        self.assertEqual(marks[('market','DOWN')]['bid'],.5)
        self.assertNotIn(('another-market','UP'),marks)

    def test_closed_or_stale_books_are_not_live_marks(self):
        board=self.board();board['cards'][0]['book_age_s']=16
        self.assertEqual(live_position_marks(board,10000),{})
        self.assertEqual(live_position_marks(self.board(),20000),{})
        board=self.board();board['stale']=True
        self.assertEqual(live_position_marks(board,10000),{})

    def test_crossed_book_is_not_an_executable_mark(self):
        board=self.board();board['cards'][0]['polymarket_up_bid']=.9
        self.assertNotIn(('market','UP'),live_position_marks(board,10000))


class DurableWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.store=PlatformStore(self.temp.name)

    def test_incident_dedup_ack_recovery_and_reopen(self):
        observed=[{'fingerprint':'feed:cb','severity':'HIGH','title':'Coinbase stale','evidence':{'age':15}}]
        incident=self.store.observe_incidents(observed)[0]
        self.store.acknowledge(incident['id'],'operator')
        again=self.store.observe_incidents(observed)[0]
        self.assertEqual(again['id'],incident['id'])
        self.assertEqual(again['state'],'ACKNOWLEDGED')
        self.assertEqual(again['occurrences'],2)
        self.assertEqual(self.store.observe_incidents([])[0]['state'],'RESOLVED')
        self.assertEqual(self.store.observe_incidents(observed)[0]['state'],'OPEN')
        reopened=PlatformStore(self.temp.name)
        self.assertEqual(reopened.incidents()[0]['id'],incident['id'])
        self.assertEqual(len(reopened.events()),4)

    def test_config_snapshot_is_content_addressed_and_audit_is_chained(self):
        a=self.store.config('cb',{'strategy.hot.max_size_usd':10})
        b=self.store.config('cb',{'strategy.hot.max_size_usd':20},'operator','increase')
        self.assertNotEqual(a,b)
        self.assertEqual(a,self.store.config('cb',{'strategy.hot.max_size_usd':10}))
        rows=self.store.events()
        self.assertEqual(rows[0]['previous_hash'],rows[1]['hash'])
        self.assertEqual(len(self.store.configs('cb')),2)

    def test_research_rejects_nonfinite_inputs(self):
        class App:
            @staticmethod
            def fleet_instances():return ['cb']
        for value in ['NaN','Infinity','-Infinity']:
            with self.assertRaises(ValueError):
                run_research(App(),self.store,'operator',{'kind':'execution_cost','extra_cost_cents':value})
        self.assertEqual(self.store.runs(),[])


if __name__=='__main__':unittest.main()
