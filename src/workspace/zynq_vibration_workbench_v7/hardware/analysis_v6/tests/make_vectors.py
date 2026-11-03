from pathlib import Path
import json,sys
import numpy as np
root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
cfg=json.loads((root/'data/coefficients.json').read_text())
def round_even(v,shift):
    q=v>>shift;r=v-(q<<shift);half=1<<(shift-1)
    return q+((r>half)|((r==half)&((q&1)!=0)))
for profile,p in enumerate(cfg['profiles']):
    n=p['required_raw_frames'];t=np.arange(n)/p['fs'];rng=np.random.default_rng(21+profile)
    raw=np.column_stack([np.rint(10000*np.sin(2*np.pi*83.0078125*t)+500),np.rint(26000*np.sin(2*np.pi*713.9*t)-1200),rng.integers(-20000,20001,n)]).astype(np.int64)
    raw[:5]=[[32767,-32768,0],[-32768,32767,32767],[0,0,-32768],[12,-12,3],[-3,4,-5]]
    q=np.array([int(v,16)-(1<<24) if int(v,16)&(1<<23) else int(v,16) for v in (root/f'data/fir_{p["fs"]}.mem').read_text().splitlines()],dtype=np.int64)
    filtered=np.column_stack([round_even(np.convolve(raw[:,c],q,'valid')[::p['decimation']],16) for c in range(3)])
    assert filtered.shape==(8192,3)
    (out/f'input_{profile}.mem').write_text(''.join(f'{((int(row[2])&65535)<<32)|((int(row[1])&65535)<<16)|(int(row[0])&65535):012x}\n' for row in raw),encoding='ascii')
    (out/f'expected_{profile}.mem').write_text(''.join(f'{((int(row[2])&0xffffff)<<48)|((int(row[1])&0xffffff)<<24)|(int(row[0])&0xffffff):018x}\n' for row in filtered),encoding='ascii')
    stats=[dict(sum=int(sum(filtered[:,c])),sumsq=int(sum(int(x)**2 for x in filtered[:,c])),min=int(min(filtered[:,c])),max=int(max(filtered[:,c])),clip=int(np.sum((raw[:,c]==32767)|(raw[:,c]==-32768)))) for c in range(3)]
    (out/f'stats_{profile}.json').write_text(json.dumps(stats,indent=2)+'\n')
    np.savez(out/f'vectors_{profile}.npz',raw=raw,filtered=filtered)
print('VECTORS_READY profiles=4')
