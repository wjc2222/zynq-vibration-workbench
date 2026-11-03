"""Host-only installed AMD bit-accurate model diagnostic; no tolerance tuning.

Reconstructs exact fixed-point FIR/mean/Hann inputs from a raw window when
provided. Reports absolute FFT errors against double on those same integer
inputs, plus a declared independent vector sweep. No real-machine accuracy.
"""
from pathlib import Path
import argparse, ctypes as ct, hashlib, json, os, struct
import numpy as np

class Generics(ct.Structure):
    _fields_=[(k,ct.c_int) for k in ('nfft','arch','has_nfft','float_pt','input_width','twiddle_width','scaling','bfp','rounding','nssr','inverse')]
class Inputs(ct.Structure):
    _fields_=[('nfft',ct.c_int),('re',ct.POINTER(ct.c_double)),('re_size',ct.c_int),('im',ct.POINTER(ct.c_double)),('im_size',ct.c_int),('scaling',ct.POINTER(ct.c_int)),('scaling_size',ct.c_int),('direction',ct.c_int)]
class Outputs(ct.Structure):
    _fields_=[('re',ct.POINTER(ct.c_double)),('re_size',ct.c_int),('im',ct.POINTER(ct.c_double)),('im_size',ct.c_int),('blk_exp',ct.c_int),('overflow',ct.c_int)]

def rnd(x,s):
    q=x>>s;r=x-(q<<s);half=1<<(s-1)
    return q+((r>half)|((r==half)&((q&1)!=0)))

class Model:
    def __init__(self,directory):
        self.path=next(directory.rglob('libIp_xfft_v9_1_bitacc_cmodel.dll'))
        self.search=os.add_dll_directory(str(self.path.parent.resolve()))
        self.deps=[ct.CDLL(str(self.path.parent/n)) for n in ('libgmp.dll','libmpfr.dll','libIp_floating_point_v7_1_bitacc_cmodel.dll')]
        self.lib=ct.CDLL(str(self.path));self.lib.xilinx_ip_xfft_v9_1_create_state.argtypes=[Generics]
        self.lib.xilinx_ip_xfft_v9_1_create_state.restype=ct.c_void_p
        self.lib.xilinx_ip_xfft_v9_1_bitacc_simulate.argtypes=[ct.c_void_p,Inputs,ct.POINTER(Outputs)]
        self.lib.xilinx_ip_xfft_v9_1_bitacc_simulate.restype=ct.c_int
        self.lib.xilinx_ip_xfft_v9_1_destroy_state.argtypes=[ct.c_void_p]
        self.state=self.lib.xilinx_ip_xfft_v9_1_create_state(Generics(13,4,0,0,24,24,1,1,1,1,0))
        if not self.state:raise RuntimeError('Cmodel state failed')
    def run(self,y):
        ptr=lambda a:a.ctypes.data_as(ct.POINTER(ct.c_double))
        re=np.ascontiguousarray(y,dtype=np.float64)/2**23;im=np.zeros(8192);ore=np.empty(8192);oim=np.empty(8192)
        sch=(ct.c_int*13)(*([0]*13));out=Outputs(ptr(ore),8192,ptr(oim),8192,0,0)
        code=self.lib.xilinx_ip_xfft_v9_1_bitacc_simulate(self.state,Inputs(13,ptr(re),8192,ptr(im),8192,sch,13,1),ct.byref(out))
        if code or out.overflow:raise RuntimeError(f'Cmodel status={code} overflow={out.overflow}')
        return np.rint(ore*2**23).astype(np.int64),np.rint(oim*2**23).astype(np.int64),out.blk_exp
    def close(self):self.lib.xilinx_ip_xfft_v9_1_destroy_state(self.state)

def metrics(model,y,energy,label):
    re,im,exp=model.run(y);ideal=np.fft.fft(y.astype(np.float64))
    restored=(re.astype(np.float64)+1j*im.astype(np.float64))*2.**exp
    diff=restored-ideal
    # One input integer = 2^(pre_shift=1)/128 ADC code. Same as production.
    unit=1/64;den=8192*energy;weight=np.full(4097,2.);weight[[0,-1]]=1
    pa=(re[:4097].astype(np.float64)**2+im[:4097].astype(np.float64)**2)*2.**(2*exp)*unit**2*weight/den
    pb=np.abs(ideal[:4097])**2*unit**2*weight/den
    rms_error=np.abs(np.sqrt(pa)-np.sqrt(pb));peak=float(pb.max())
    complex_error_rms=np.abs(diff[:4097])*unit*np.sqrt(weight/den)
    eligible=pb>=max(1e-10,peak*1e-6)
    relative=np.zeros_like(pb);relative[eligible]=rms_error[eligible]/np.sqrt(pb[eligible])
    worst=int(np.argmax(relative))
    summary=dict(label=label,input_peak_integer=int(np.abs(y).max()),bfp_exponent=int(exp),
        model_vs_double_max_complex_error_output_lsb=float(np.abs(diff).max()/2**exp),
        model_vs_double_complex_l2_relative=float(np.linalg.norm(diff)/max(np.linalg.norm(ideal),1e-100)),
        max_bin_rms_absolute_error_adc=float(rms_error.max()),max_bin_complex_absolute_error_adc_rms=float(complex_error_rms.max()),
        all_bins_complex_error_rms_adc=float(np.linalg.norm(complex_error_rms)),
        reference_band_complex_error_rms_adc=float(np.linalg.norm(complex_error_rms[4:820])),
        reference_band_double_rms_adc=float(np.sqrt(pb[4:820].sum())),
        reference_band_model_rms_adc=float(np.sqrt(pa[4:820].sum())),
        spectrum_rms_relative=float(np.linalg.norm(np.sqrt(pa)-np.sqrt(pb))/max(np.sqrt(pb.sum()),1e-100)),
        total_rms_relative=float(abs(np.sqrt(pa.sum())-np.sqrt(pb.sum()))/max(np.sqrt(pb.sum()),1e-100)),
        old_relative_peak_based_strong_bins=int(eligible.sum()),old_max_strong_bin_rms_relative=float(relative.max()),
        old_worst_bin=worst,old_worst_bin_reference_rms_adc=float(np.sqrt(pb[worst])),old_worst_bin_absolute_error_adc=float(rms_error[worst]),
        reference_peak_power_adc2=peak)
    return summary,pa,pb,re,im

def main():
    p=argparse.ArgumentParser();p.add_argument('--model-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--raw',type=Path);p.add_argument('--fs',type=int,default=50000);p.add_argument('--sweep',action='store_true')
    p.add_argument('--hardware-power-ch3',type=Path)
    p.add_argument('--driver-result',type=Path,help='197272-byte fixed UAPI result from dump-driver-result')
    a=p.parse_args()
    if not a.raw and not a.sweep:raise ValueError('raw input or independent sweep required')
    if a.driver_result and not a.raw:raise ValueError('driver result requires the matching raw input')
    if a.output.exists():raise ValueError('new output directory required')
    a.output.mkdir(parents=True);root=Path(__file__).resolve().parents[1]
    hann=np.array([int(v,16) for v in (root/'data/hann_8192.mem').read_text().split()],dtype=np.int64)
    energy=float(np.sum((hann/2**23)**2));model=Model(a.model_dir);summaries=[]
    wire=None
    if a.driver_result:
        wire=a.driver_result.read_bytes()
        if len(wire)!=197272:raise ValueError('driver result size mismatch')
        header=struct.unpack_from('<10IQ4I',wire)
        if header[:5]!=(1,0x36414e41,0x60001,a.fs,8192) or header[9]:raise ValueError('driver result identity/error mismatch')
    try:
        if a.raw:
            taps={50000:641,25000:321,10000:129,5000:65}[a.fs];d=a.fs//5000
            raw=np.fromfile(a.raw,dtype='<i2').astype(np.int64).reshape(-1,3)
            if len(raw)!=taps+8191*d:raise ValueError('raw frame count mismatch')
            h=np.array([int(v,16) for v in (root/f'data/fir_{a.fs}.mem').read_text().split()],dtype=np.int64);h[h>=2**23]-=2**24
            for ch in range(3):
                rounded=rnd(np.convolve(raw[:,ch],h,'valid')[::d],16)
                sat_count=int(np.count_nonzero((rounded < -8388608)|(rounded > 8388607)))
                filtered=np.clip(rounded,-8388608,8388607)
                mean=int(rnd(np.array([filtered.sum()],dtype=np.int64),13)[0]);y=rnd((filtered-mean)*hann,24)
                summary,pa,pb,re,im=metrics(model,y,energy,f'raw_CH{ch+1}')
                summary['raw_window_sha256']=hashlib.sha256(a.raw.read_bytes()).hexdigest()
                summary['lowband_ac_rms_adc']=float(np.std(filtered/128))
                summaries.append(summary);np.savez(a.output/f'raw_CH{ch+1}.npz',filtered_q7=filtered,fft_input=y,re=re,im=im,model_power=pa,double_power=pb)
                if wire:
                    off=64+ch*65736;stats=struct.unpack_from('<qQiiIIQQ',wire,off)
                    low=np.frombuffer(wire,dtype='<i4',count=8192,offset=off+48)
                    hw_power=np.frombuffer(wire,dtype='<u8',count=4097,offset=off+32816)
                    hw_bands=np.frombuffer(wire,dtype='<u8',count=18,offset=off+65592)
                    model_power=(re[:4097]**2+im[:4097]**2).astype(np.uint64)
                    clips=int(np.count_nonzero((raw[:,ch]==-32768)|(raw[:,ch]==32767)))
                    expected_stats=(int(filtered.sum()),int(np.sum(filtered*filtered)),int(filtered.min()),int(filtered.max()),summary['bfp_exponent'],1,clips,sat_count)
                    weighted=model_power.copy();weighted[1:-1]*=2
                    expected_bands=np.zeros(18,dtype=np.uint64)
                    expected_bands[13]=np.sum(weighted[4:820]);expected_bands[14]=np.sum(weighted[:3277]);expected_bands[17]=expected_bands[13]
                    summary.update(hardware_driver_result_sha256=hashlib.sha256(wire).hexdigest(),
                        hardware_lowband_exact=bool(np.array_equal(low,filtered)),
                        hardware_stats_and_scale_exact=stats==expected_stats,
                        hardware_power_words_exact=bool(np.array_equal(hw_power,model_power)),
                        hardware_power_words_checked=4097,
                        hardware_power_max_integer_error=int(np.max(np.abs(hw_power.astype(np.int64)-model_power.astype(np.int64)))),
                        hardware_fixed_and_invalid_order_bands_exact=bool(np.array_equal(hw_bands,expected_bands)),
                        hardware_band_words_checked=18)
                    if header[7]!=0:raise ValueError('diagnostic currently expects no order descriptors')
                if ch==2 and a.hardware_power_ch3:
                    hardware=np.fromfile(a.hardware_power_ch3,dtype='<u8');expected=re[:4097]**2+im[:4097]**2
                    if hardware.shape!=(4097,):raise ValueError('hardware CH3 power shape mismatch')
                    summary['hardware_power_words_exact']=bool(np.array_equal(hardware,expected))
                    summary['hardware_power_max_integer_error']=int(np.max(np.abs(hardware.astype(np.int64)-expected)))
        if a.sweep:
            # Declared a priori independent domain, not fitted to the measured window.
            n=np.arange(8192);rng=np.random.default_rng(20261031)
            for amplitude in (1,2,4,8,16,32,64,128,256,512,1024,4096,16384,65536,262144,1048576,4194304):
                for kind in ('coherent41','offbin1638p4','impulse','white','alternating','dc'):
                    if kind=='coherent41':x=np.rint(amplitude*np.sin(2*np.pi*41*n/8192))
                    elif kind=='offbin1638p4':x=np.rint(amplitude*np.sin(2*np.pi*1638.4*n/8192))
                    elif kind=='impulse':x=np.zeros(8192);x[1471]=amplitude
                    elif kind=='white':x=rng.integers(-amplitude,amplitude+1,8192)
                    elif kind=='alternating':x=np.where(n%2,-amplitude,amplitude)
                    else:x=np.full(8192,amplitude)
                    # The FFT integer input itself is the independent stimulus.
                    summary,*_=metrics(model,np.asarray(x,dtype=np.int64),energy,f'{kind}_input_lsb{amplitude}')
                    summaries.append(summary)
        result=dict(schema='ax7z020.v6.fft-quantization-diagnostic.v1',model_sha256=hashlib.sha256(model.path.read_bytes()).hexdigest(),
                    model_generics=dict(nfft=13,arch=4,input_width=24,twiddle_width=24,bfp=1,rounding=1),
                    method='installed bit-accurate model against double FFT of identical quantized integer input',
                    tolerance_changed=False,empirical_diagnostic_not_universal_error_bound=True,
                    official_rounding_documentation='https://docs.amd.com/r/en-US/pg109-xfft/Rounding-Implementation',
                    machine_classification_accuracy_assessed=False,
                    hardware_exact_against_vendor_model=(all(x.get('hardware_lowband_exact') and x.get('hardware_stats_and_scale_exact') and x.get('hardware_power_words_exact') and x.get('hardware_fixed_and_invalid_order_bands_exact') for x in summaries) if wire else None),
                    reports=summaries)
        (a.output/'report.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(dict(reports=len(summaries),output=str(a.output),max_complex_error_output_lsb=max(x['model_vs_double_max_complex_error_output_lsb'] for x in summaries))))
        if wire and not result['hardware_exact_against_vendor_model']:raise SystemExit(4)
    finally:model.close()

if __name__=='__main__':main()
