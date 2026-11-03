"""Independent integer FIR fixtures. No FFT model or machine evidence."""
from pathlib import Path
import argparse, hashlib, json
import numpy as np

def generate(data, output):
    output.mkdir(parents=True, exist_ok=True)
    coeff = np.array([int(line, 16) for line in (data/'fir_all.mem').read_text().split()], dtype=np.int64)
    coeff[coeff >= 2**23] -= 2**24
    manifest = []
    kinds = ('full', 'dc', 'step', 'alternating', 'impulse', 'noise', 'adversarial_saturation')
    for profile, (fs,taps,d) in enumerate(((50000,641,10),(25000,321,5),(10000,129,2),(5000,65,1))):
        count = taps + 8191*d
        h = coeff[profile*1024:profile*1024+taps]
        if taps % 2 != 1 or not np.array_equal(h, h[::-1]):
            raise ValueError(f'Profile {profile} is not an odd symmetric FIR; pre-add evaluator is invalid')
        for case, kind in enumerate(kinds):
            raw = np.zeros((count,3),dtype=np.int64)
            if kind == 'full': raw[:] = (32767,-32768,32767)
            elif kind == 'dc': raw[:] = (1234,-2345,100)
            elif kind == 'step': raw[count//2:] = (30000,-28000,12000)
            elif kind == 'alternating':
                raw[:] = np.where((np.arange(count)%2)[:,None],(-32768,32767,-20000),(32767,-32768,20000))
            elif kind == 'impulse':
                raw[[0,taps-1,count//2,count-1]] = ((32767,-32768,12000),(-12000,18000,-32768),(32767,24000,-12000),(-32768,32767,20000))
            elif kind == 'noise': raw[:] = np.random.default_rng(20261030+profile).integers(-32768,32768,size=(count,3))
            else:
                # Align maximal signed16 input with the reversed coefficient
                # signs at the first complete output. L1 gain >2 exercises
                # signed24-Q7 clamp and the arithmetic-saturation counter.
                aligned=np.where(h[::-1]>=0,32767,-32768)
                raw[:taps,0]=aligned;raw[:taps,1]=-aligned-1;raw[:taps,2]=aligned
            result=np.zeros((8192,3),dtype=np.int64);sat=[]
            for ch in range(3):
                accum=np.convolve(raw[:,ch],h,mode='valid')[::d]
                assert len(accum)==8192
                q=accum>>16;remainder=accum-(q<<16)
                q=q+((remainder>32768)|((remainder==32768)&((q&1)!=0)))
                sat.append(int(np.count_nonzero((q < -8388608)|(q > 8388607))))
                result[:,ch]=np.clip(q,-8388608,8388607)
            base=f'fir_unit_{profile}_{case}'
            packed_raw=raw[:,0]&65535 | ((raw[:,1]&65535)<<16) | ((raw[:,2]&65535)<<32)
            (output/(base+'_input.mem')).write_text(''.join(f'{int(x):012x}\n' for x in packed_raw),encoding='ascii')
            packed_result=[sum((int(v)&0xffffff)<<(24*ch) for ch,v in enumerate(frame)) for frame in result]
            (output/(base+'_expected.mem')).write_text(''.join(f'{x:018x}\n' for x in packed_result),encoding='ascii')
            stats=[]
            for ch in range(3):
                x=result[:,ch]
                stats.extend((int(x.min()),int(x.max()),int(x.sum()),int((x*x).sum()),int(np.count_nonzero((raw[:,ch]==-32768)|(raw[:,ch]==32767))),sat[ch]))
            (output/(base+'_stats.mem')).write_text(''.join(f'{x&((1<<64)-1):016x}\n' for x in stats),encoding='ascii')
            manifest.append(dict(profile=profile,fs=fs,taps=taps,decimation=d,kind=kind,raw_frames=count,
                                 reference='integer64 convolution, nearest ties-even Q23 to Q7, signed24 clamp',
                                 saturation_counts=sat,
                                 raw_sha256=hashlib.sha256(raw.astype('<i2').tobytes()).hexdigest()))
    (output/'fir_unit_manifest.json').write_text(json.dumps(dict(scope='FIR only; FFT is stubbed and unverified',cases=manifest),indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('data',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();generate(args.data,args.output)
