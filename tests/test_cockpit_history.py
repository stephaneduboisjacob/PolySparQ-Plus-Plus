"""Durable history pagination and scope regression tests; no live DB writes."""
import base64
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import cockpit
from cockpit_platform import transactions_view


class TransactionHistoryTests(unittest.TestCase):
    def test_history_preserves_microseconds_and_scope_in_cursor(self):
        class App:
            calls=[]
            def q(self,sql,args=(),one=False):
                self.calls.append((sql,args))
                if one:return {'fills':3,'purchased':1,'proceeds':2,'fees':0}
                return [{'id':3-i,'ts':1000,'cursor_ts':f'2026-09-20 00:00:00.00000{3-i}+00'} for i in range(3)]
        app=App()
        first=transactions_view(app,{'instance':['retired-bot'],'days':['0'],'limit':['2']})
        cursor=json.loads(base64.urlsafe_b64decode(first['next_cursor']))
        self.assertEqual(cursor,{'ts':'2026-09-20 00:00:00.000002+00','id':2})
        self.assertNotIn('cursor_ts',first['transactions'][0])
        self.assertEqual(app.calls[0][1],('retired-bot','retired-bot','',''))
        self.assertNotIn('now() -',app.calls[0][0])
        transactions_view(app,{'cursor':[first['next_cursor']],'limit':['2']})
        self.assertEqual(app.calls[-1][1][-3].microsecond,2)
        self.assertIn('(f.ts,f.id)<',app.calls[-1][0])

    def test_invalid_history_cursor_and_mode_are_explicit(self):
        class App:
            def q(self,*args,**kwargs):return {}
        for cursor in ['!','e30=','W10=',base64.urlsafe_b64encode(b'{"ts":"2026-09-20","id":1}').decode()]:
            with self.assertRaises(ValueError):transactions_view(App(),{'cursor':[cursor]})
        with self.assertRaises(ValueError):transactions_view(App(),{'mode':['bogus']})

    def test_analytics_cache_does_not_mix_instances(self):
        rows=[dict(id=1,ts=1,client_order_id='a1',mode='paper',instance='a',session_id='s',condition_id='c',token_id='t',direction='buy',price=.5,size_shares=10,fee_usd=0),
              dict(id=2,ts=2,client_order_id='a2',mode='paper',instance='a',session_id='s',condition_id='c',token_id='t',direction='sell',price=1,size_shares=10,fee_usd=0),
              dict(id=3,ts=1,client_order_id='b1',mode='paper',instance='b',session_id='s',condition_id='c',token_id='t',direction='buy',price=.5,size_shares=10,fee_usd=0),
              dict(id=4,ts=2,client_order_id='b2',mode='paper',instance='b',session_id='s',condition_id='c',token_id='t',direction='sell',price=0,size_shares=10,fee_usd=0)]
        with patch.object(cockpit,'q',return_value=rows),patch.object(cockpit,'_analytics_cache',{}):
            self.assertEqual(cockpit.api_v2_analytics(0,'a')['summary']['net'],5)
            self.assertEqual(cockpit.api_v2_analytics(0,'b')['summary']['net'],-5)
            self.assertEqual(cockpit.api_v2_analytics(0)['summary']['net'],0)
            self.assertEqual(cockpit.api_v2_analytics(0,'a')['summary']['net'],5)

    def test_closed_exits_pagination_is_stable_at_equal_timestamps(self):
        data={'exits':[dict(id=i,ts=100,realized_pnl=1,fees=0,cost_basis=1,shares=1,entry_cost=1) for i in [1,2,3]],'server_ts':101,'method':'matched','excluded_unmatched_exits':0}
        with patch.object(cockpit,'api_v2_analytics',return_value=data):
            a=cockpit.api_v4_platform('closed_positions',{'limit':['2']})
            b=cockpit.api_v4_platform('closed_positions',{'limit':['2'],'cursor':[a['next_cursor']]})
        self.assertEqual([x['id'] for x in a['exits']],[3,2])
        self.assertEqual([x['id'] for x in b['exits']],[1])
        self.assertEqual(b['totals']['trades'],3)

    def test_scoped_maker_and_latency_queries_bind_all_parameters(self):
        def query(sql,args=(),one=False):
            self.assertEqual(sql.count('%s'),len(args or ()))
            return []
        with patch.object(cockpit,'q',side_effect=query):
            self.assertEqual(cockpit.api_v3_maker_fleet('cb')['quotes'],[])
            self.assertEqual(cockpit.api_v3_latency()['latency'],[])

    def test_live_start_requires_explicit_confirmation(self):
        with patch.object(cockpit,'fleet_instances',return_value=['cb']),patch.object(cockpit,'current_mode',return_value='live'):
            self.assertEqual(cockpit.api_v3_fleet_control({'action':'start','instance':'cb'})[0],422)
            self.assertEqual(cockpit.api_v3_fleet_control({'action':'start','instance':'unknown'})[0],400)


if __name__=='__main__':unittest.main()
