"""Frozen v6 FIR/Hann coefficients; numerical evidence is not sensor evidence."""
from pathlib import Path
import hashlib, json
import numpy as np
from scipy.signal import remez, freqz

root=Path(__file__).resolve().parents[1]
out=root/'data';out.mkdir(exist_ok=True)
manifest={'coefficient_fraction_bits':23,'output_fraction_bits':7,'rounding':'nearest_ties_even','fir_sha256_encoding':'signed24_twos_complement_hex6_lowercase_LF','profiles':[]}
header=['#pragma once','#include <cstdint>','namespace v6_coeff {']
mem=[]
for fs,taps,d in [(50000,641,10),(25000,321,5),(10000,129,2),(5000,65,1)]:
    stop=2400 if fs==5000 else 2500
    h=remez(taps,[0,2000,stop,fs/2],[1,0],weight=[1,10],fs=fs,maxiter=100)
    q=np.rint(h*2**23).astype(np.int64)
    f,H=freqz(q/2**23,worN=1048576,fs=fs)
    passdb=20*np.log10(np.maximum(abs(H[f<=2000]),1e-20))
    attenuation=-20*np.log10(max(abs(H[f>=stop])))
    ripple=max(passdb)-min(passdb)
    assert ripple<=.1 and attenuation>=80,(fs,ripple,attenuation)
    text=''.join(f'{int(v)&0xffffff:06x}\n' for v in q)
    (out/f'fir_{fs}.mem').write_text(text,encoding='ascii')
    mem.extend(list(q)+[0]*(1024-taps))
    header.append(f'inline constexpr int32_t fir_{fs}[{taps}] = {{'+','.join(map(str,q))+'};')
    edge_response=sum(q/2**23*np.exp(-2j*np.pi*2000*np.arange(taps)/fs))
    manifest['profiles'].append(dict(fs=fs,taps=taps,decimation=d,first_output_raw_index=taps-1,required_raw_frames=taps+8191*d,group_delay_raw_samples=(taps-1)//2,coefficient_sum=int(sum(q)),ripple_db=float(ripple),passband_min_gain_db=float(min(passdb)),passband_max_gain_db=float(max(passdb)),gain_at_2000_hz_db=float(20*np.log10(abs(edge_response))),stopband_attenuation_db=float(attenuation),sha256=hashlib.sha256(text.encode()).hexdigest(),file_sha256=hashlib.sha256((out/f'fir_{fs}.mem').read_bytes()).hexdigest()))
(out/'fir_all.mem').write_text(''.join(f'{int(v)&0xffffff:06x}\n' for v in mem),encoding='ascii')
manifest['fir_all_file_sha256']=hashlib.sha256((out/'fir_all.mem').read_bytes()).hexdigest()
w=np.rint((.5-.5*np.cos(2*np.pi*np.arange(8192)/8192))*2**23).astype(np.int64)
(out/'hann_8192.mem').write_text(''.join(f'{int(v):06x}\n' for v in w),encoding='ascii')
header.append('inline constexpr const char* fir_sha256[4] = {'+','.join('"'+p['sha256']+'"' for p in manifest['profiles'])+'};')
header.append('inline constexpr uint32_t hann_8192[8192] = {'+','.join(map(str,w))+'};')
header.append('}')
(out/'analysis_coefficients.hpp').write_text('\n'.join(header)+'\n',encoding='ascii')
manifest['hann']={'fraction_bits':23,'storage':'unsigned24','sum':int(sum(w)),'sum_squares':int(sum(int(v)**2 for v in w)),'sha256_encoding':'literal_file_bytes','sha256':hashlib.sha256((out/'hann_8192.mem').read_bytes()).hexdigest()}
(out/'coefficients.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='ascii')
print(json.dumps(manifest,indent=2))
