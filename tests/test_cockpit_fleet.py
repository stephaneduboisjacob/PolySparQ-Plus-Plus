import json
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'scripts'))
import cockpit
from cockpit_fleet import action, EXECUTION_DEFAULTS

class FleetControlsTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.config=self.root/'config';self.config.mkdir()
        (self.root/'PolySparQ++_CAPABILITIES.json').write_bytes((Path(__file__).resolve().parent.parent/'PolySparQ++_CAPABILITIES.json').read_bytes())
        self.path=self.config/'polypulse-base.toml'
        self.path.write_text('[runtime]\nmode="paper"\n[strategy.hot]\nautopilot_on_start=true\nper_click_size_usd=5.0\n[risk]\nmax_order_usd=5.0\n[wallet]\nstarting_balance_usd=100.0\nprivate_key="DO_NOT_EXPOSE"\n[data_collection]\nfeatures_enabled=true\n')
        for name,value in [('ROOT',str(self.root)),('CONFIG_DIR',str(self.config)),('_platform',None)]:
            p=patch.object(cockpit,name,value);p.start();self.addCleanup(p.stop)
        for name,value in [('fleet_instances',lambda:['base']),('instance_running',lambda n:False),('_supervisor_running',lambda n:False),('current_mode',lambda n:'paper'),('accounting_since',lambda *a:None),('reconstructed_positions',lambda *a:{'positions':[]}),('q',lambda *a,**k:{'n':0})]:
            p=patch.object(cockpit,name,value);p.start();self.addCleanup(p.stop)
        self.app=cockpit._CockpitServices()

    def call(self,**body):return action(self.app,body,'test-operator')

    def test_create_is_paper_stopped_and_preserves_private_config(self):
        r=self.call(operation='create',instance='new-maker',template='base',execution_mode='maker')
        cfg=tomllib.loads((self.config/'polypulse-new-maker.toml').read_text())
        self.assertEqual(cfg['runtime']['mode'],'paper')
        self.assertFalse(cfg['strategy']['hot']['autopilot_on_start'])
        self.assertFalse(cfg['data_collection']['features_enabled'])
        self.assertEqual(cfg['strategy']['hot']['maker_poll_ms'],500)
        self.assertNotIn('DO_NOT_EXPOSE',json.dumps(r)+json.dumps(cockpit.platform_store().events()))
        with self.assertRaises(ValueError):self.call(operation='create',instance='new-maker',template='base')
        self.assertEqual((self.config/'polypulse-new-maker.toml').stat().st_mode&0o777,0o600)

    def test_properties_need_flat_stopped_and_explicit_live(self):
        with self.assertRaises(ValueError):self.call(operation='properties',instance='base',changes={'mode':'live'},confirmed=True)
        with patch.object(cockpit,'instance_running',return_value=True):
            with self.assertRaises(ValueError):self.call(operation='properties',instance='base',changes={'mode':'paper'})
        with patch.object(cockpit,'reconstructed_positions',return_value={'positions':[{'shares':1}]}):
            with self.assertRaises(ValueError):self.call(operation='properties',instance='base',changes={'paper_balance':1000},confirmed=True)

    def test_paper_balance_atomic_and_audited(self):
        import hashlib
        r=self.call(operation='properties',instance='base',changes={'paper_balance':1000},confirmed=True,expected_hash=hashlib.sha256(self.path.read_bytes()).hexdigest()[:12])
        self.assertTrue(r['ok']);self.assertEqual(tomllib.loads(self.path.read_text())['wallet']['starting_balance_usd'],1000)
        self.assertEqual(cockpit.platform_store().events()[0]['type'],'fleet.properties')

    def test_missing_maker_controls_are_editable_defaults(self):
        k=cockpit.api_v3_knobs('base')
        row=next(r for g in k['groups'] for r in g['rows'] if r['bare']=='maker_poll_ms')
        self.assertEqual(row['value'],500);self.assertTrue(row['active'])
        code,r=cockpit.api_v3_config_write({'instance':'base','changes':[{'key':'strategy.hot.maker_poll_ms','value':250}]})
        self.assertEqual(code,200,r);self.assertEqual(tomllib.loads(self.path.read_text())['strategy']['hot']['maker_poll_ms'],250)

    def test_recycler_requires_paper_and_explicit_exit_policy(self):
        code,r=cockpit.api_v3_config_write({'instance':'base','changes':[{'key':'strategy.hot.recycler_mode','value':'sniper'}]})
        self.assertEqual(code,422,r)
        code,r=cockpit.api_v3_config_write({'instance':'base','changes':[
            {'key':'strategy.hot.recycler_mode','value':'sniper'},
            {'key':'strategy.hot.recycler_allow_taker_exit','value':True}]})
        self.assertEqual(code,200,r)
        import hashlib
        with self.assertRaisesRegex(ValueError,'PAPER'):
            self.call(operation='properties',instance='base',changes={'mode':'live'},confirmed=True,
                confirm_live=True,expected_hash=hashlib.sha256(self.path.read_bytes()).hexdigest()[:12])
        before=self.path.read_bytes()
        code,r=cockpit.api_v3_config_write({'instance':'base','changes':[{'key':'strategy.hot.recycler_target_price','value':.3}]})
        self.assertEqual(code,422,r);self.assertEqual(before,self.path.read_bytes())

    def test_stopped_service_is_not_running_due_to_recent_log(self):
        from types import SimpleNamespace
        # Use the original function, bypassing this fixture's lifecycle stub.
        import importlib.util
        spec=importlib.util.spec_from_file_location('status_test',Path(cockpit.__file__))
        real=importlib.util.module_from_spec(spec);spec.loader.exec_module(real)
        with patch.object(real.subprocess,'run',return_value=SimpleNamespace(stdout='inactive')), patch.object(real.os,'stat',side_effect=AssertionError('Must not inspect a stopped service log')):
            self.assertFalse(real.instance_running('base'))

    def test_checkpoint_readiness_is_populated_before_gate(self):
        text=(Path(__file__).resolve().parent.parent/'src/main.cpp').read_text()
        populate=text.index('populate_reference_forecasts(row,m,oracle_buf,cb_buf,candle_store,now_ms)')
        checkpoint=text.index('// ── WPRED: window-fraction checkpoint predictor')
        self.assertLess(populate,checkpoint)

if __name__=='__main__':unittest.main()
