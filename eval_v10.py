"""Score the held-out set with a .tflite and write held_out_v10_scored.csv.

Used 2026-09-19 to derive the v10 thresholds in app_hazard_classifier.h
(HAZ_LOGIT_THRESH_Q / HAZ_PROB_THRESH_Q). The v10 model itself is not in this
repository; see docs/PROVENANCE.md and model/README.md.

usage: python3 eval_v10.py <model.tflite>
"""
import sys
import numpy as np, pandas as pd
from ai_edge_litert.interpreter import Interpreter

if len(sys.argv) != 2:
    sys.exit(__doc__)
MODEL = sys.argv[1]
interp = Interpreter(model_path=MODEL)
interp.allocate_tensors()
inp = interp.get_input_details()[0]
out = interp.get_output_details()[0]
print("input:", inp['dtype'], inp['shape'], inp.get('quantization'))
print("output:", out['dtype'], out['shape'], out.get('quantization'))

FEAT = ["d0","d1","d2","d3","d4","d5","d6","d7","d8","d9","v","a","ax","ay","az","amb","spad"]
held = pd.read_csv("held_out_test_vectors_WITH_ANGLE_20260918.csv")
print("held-out rows:", len(held))

in_scale, in_zp = inp['quantization']
out_scale, out_zp = out['quantization']

probs = []
for _, row in held.iterrows():
    x = row[FEAT].values.astype(np.float32)
    if inp['dtype'] == np.int8:
        xq = np.round(x/in_scale + in_zp).astype(np.int8)
    else:
        xq = x.astype(inp['dtype'])
    interp.set_tensor(inp['index'], xq.reshape(inp['shape']))
    interp.invoke()
    y = interp.get_tensor(out['index'])[0]
    if out['dtype'] == np.int8:
        yf = (y.astype(np.float32) - out_zp) * out_scale
    else:
        yf = y.astype(np.float32)
    probs.append(float(yf[0]) if yf.shape else float(yf))

held['prob'] = probs
held.to_csv("held_out_v10_scored.csv", index=False)
print("min/max prob:", held.prob.min(), held.prob.max())
