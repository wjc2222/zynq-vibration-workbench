"""HOST synthetic engineering numeric fixture only; NOT measured vibration data.

Excites each of the 13 existing CLI order bands at --rotation-hz 25. Each
frequency is an exact 8192-point/5kHz analysis bin. The complete 641-tap history
and 8192 decimated outputs require 82551 raw 50kHz frames. No code/tolerance/model
changes, no mechanical fault signal, no training or physical accuracy claim.
"""
from pathlib import Path
import hashlib,json,math,struct
here=Path(__file__).resolve().parent
fs=50000;frames=82551;rotation=25;analysis_rate=5000;points=8192
orders=[.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10]
bins=[int(math.floor(rotation*o*points/analysis_rate+.5)) for o in orders]
amplitudes=[1200+40*i for i in range(13)]
scales=[1,.8,.6];dc=[100,-150,75]
tones=[]
for i,(order,k,amp) in enumerate(zip(orders,bins,amplitudes)):
    first=max(4,math.ceil(rotation*order*points/analysis_rate-1.5))
    last=math.floor(rotation*order*points/analysis_rate+1.5)
    assert first<=k<=last
    tones.append({'order':order,'bin':k,'frequency_hz':k*analysis_rate/points,
                  'amplitude_CH1_ADC':amp,'band_first_bin':first,'band_last_bin':last,
                  'phase_radians':-math.pi*i*(i-1)/13})
raw=bytearray(frames*6);minimum=[32767]*3;maximum=[-32768]*3;sum_sq=[0]*3
for n in range(frames):
    values=[]
    for ch in range(3):
        value=round(dc[ch]+scales[ch]*sum(t['amplitude_CH1_ADC']*math.sin(2*math.pi*t['frequency_hz']*n/fs+t['phase_radians']+.23*ch*(i+1)) for i,t in enumerate(tones)))
        assert -32768<value<32767
        values.append(value);minimum[ch]=min(minimum[ch],value);maximum[ch]=max(maximum[ch],value);sum_sq[ch]+=value*value
    struct.pack_into('<hhh',raw,n*6,*values)
path=here/'SYNTHETIC-numeric-multisine-50k-82551-s16le3.bin'
path.write_bytes(raw)
report={'schema':'ax7z020.v7.synthetic-numeric-fixture.v1','synthetic':True,
 'scope':'Engineering numeric comparison only; NOT measured machine/sensor data, training data, model validation or fault accuracy',
 'sample_rate_hz':fs,'frames':frames,'channels':3,'layout':'chronological frame-interleaved CH1 CH2 CH3 signed int16 little endian, no header',
 'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'rotation_hz':rotation,'tones':tones,
 'amplitude_scales':scales,'DC_ADC':dc,'observed_minimum_ADC':minimum,'observed_maximum_ADC':maximum,
 'raw_rms_ADC':[math.sqrt(x/frames) for x in sum_sq],
 'conservative_peak_bounds_ADC':[abs(dc[ch])+scales[ch]*sum(amplitudes)+.5 for ch in range(3)],
 'raw_ADC_endpoints':0,'CLI_noise_floor_or_tolerances_changed':False,
 'coherence_proof':'f_k=k*5000/8192; decimation10 gives 8192-sample analysis integer bin; full FIR history prevents startup transient',
 'band_coverage_proof':'Each nearest integer analysis bin is explicitly verified inside its unchanged CLI +/-1.5-bin order band; reference bins4..819 contain all13 tones',
 'suggested_command':'vibration-accel-cli --backend hardware --fs 50000 --raw /tmp/v7-new-board-20261103/SYNTHETIC-numeric-multisine-50k-82551-s16le3.bin --rotation-hz 25 --compare',
 'hardware_tested_by_generator':False}
(here/'SYNTHETIC-numeric-multisine-proof.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'path':str(path),'bytes':len(raw),'sha256':report['sha256'],'min':minimum,'max':maximum,'bins':bins},indent=2))
