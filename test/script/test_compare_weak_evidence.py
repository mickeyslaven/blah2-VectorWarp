import copy
import importlib.util
import json
import os
import subprocess
import unittest

SPEC=importlib.util.spec_from_file_location("weak", "script/compare-weak-evidence.py")
W=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(W)


class WeakComparisonTest(unittest.TestCase):
    def test_split_identity_and_qualification(self):
        row={"role":"development","pass_id":"p1","hardware_epoch":"e","first_sample":0,"end_sample":100}
        self.assertEqual(len(W.validate_cohort([row])),3)
        with self.assertRaisesRegex(ValueError,"whole-pass"):
            W.validate_cohort([row,dict(row,first_sample=100,end_sample=200)])
        with self.assertRaisesRegex(ValueError,"sample overlap"):
            W.validate_cohort([row,dict(row,pass_id="p2",first_sample=99,end_sample=200)])
        with self.assertRaisesRegex(ValueError,"qualification"):
            W.validate_cohort([dict(row,role="holdout")])
        with self.assertRaisesRegex(ValueError,"target-free"):
            W.validate_cohort([dict(row,role="negative_control",qualified=True,inspected_before_freeze=False)])

    def test_mixed_support_impulse_and_boundaries(self):
        def rows(depths):
            return [{"epoch":"e","sequence":i,"depth":d,"hit":True} for i,d in enumerate(depths)]
        self.assertEqual(list(W.disjoint_credits(rows([1,2,1,4,1,2,1,8]))),
                         [True,False,True,False,True,False,True,False])
        self.assertEqual(list(W.disjoint_credits(rows([2]*8))),[False,True,False,True,False,True,False,True])
        # Eight overlapping views of the same support never become eight votes.
        self.assertEqual(sum(W.disjoint_credits(rows([8]*16))),2)

    def test_gap_reset_duplicate_and_long_ring(self):
        rows=[{"epoch":"e","sequence":i,"depth":2,"hit":True} for i in range(300)]
        self.assertEqual(sum(W.disjoint_credits(rows)),150)
        rows[2]["sequence"]=10;rows[3]["sequence"]=11
        self.assertEqual(list(W.disjoint_credits(rows[:4])),[False,True,False,True])
        rows[3]["epoch"]="new"
        self.assertFalse(list(W.disjoint_credits(rows[:4]))[-1])
        with self.assertRaisesRegex(ValueError,"duplicate"):
            list(W.disjoint_credits([rows[0],rows[0]]))

    def test_fixed_baseline_retained_and_fractional_alignment(self):
        powers=[[[0,10,0]],[[10,0,0]]]
        fixed=W.motion_bank(powers,[1],-1,2,False)
        moving=W.motion_bank(powers,[1],-1,2,True)
        self.assertEqual(fixed[0]["power"][0][0],5)
        self.assertEqual(moving[0]["power"][0][0],10)
        self.assertEqual((moving[0]["first_frame"],moving[0]["ending_frame"]),(0,1))

    @unittest.skipUnless(os.environ.get("WEAK_BOOTSTRAP_EXE"),"compiled portable adapter not selected")
    def test_actual_cpp_bootstrap_scale_invariance_and_no_training(self):
        frame={"frame":0,"delay_km":list(range(33)),"doppler_hz":[0],
               "channels":[[[[float((i%3)+1),float(i%2)] for i in range(33)]] for _ in range(5)]}
        def run(f):
            p=subprocess.run([os.environ["WEAK_BOOTSTRAP_EXE"]],input=json.dumps(f)+"\n",text=True,
                             capture_output=True,timeout=10,check=True)
            return json.loads(p.stdout)
        a=run(frame);scaled=copy.deepcopy(frame)
        for channel in scaled["channels"]:
            for row in channel:
                for z in row:z[0]*=1e-8;z[1]*=1e-8
        b=run(scaled)
        for mode in ("diagonal","shrinkage075"):
            self.assertEqual(a[mode]["trained"],33)
            for x,y in zip(a[mode]["power"][0],b[mode]["power"][0]):self.assertAlmostEqual(x,y,places=9)
        frame["delay_km"]=[0]
        for channel in frame["channels"]:channel[0]=[[1,0]]
        self.assertEqual(run(frame)["diagonal"]["invalid"],1)


if __name__=="__main__":unittest.main()
