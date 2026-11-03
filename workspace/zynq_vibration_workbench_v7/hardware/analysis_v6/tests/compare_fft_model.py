"""Installed AMD C model vs actual RTL AXIS captures, all complex codes/bins."""
import ctypes as ct, os,sys,json,zipfile,hashlib
from pathlib import Path
import numpy as np
class Generics(ct.Structure):
    _fields_=[(k,ct.c_int) for k in ('nfft','arch','has_nfft','float_pt','input_width','twiddle_width','scaling','bfp','rounding','nssr','inverse')]
class Inputs(ct.Structure):
    _fields_=[('nfft',ct.c_int),('re',ct.POINTER(ct.c_double)),('re_size',ct.c_int),('im',ct.POINTER(ct.c_double)),('im_size',ct.c_int),('scaling',ct.POINTER(ct.c_int)),('scaling_size',ct.c_int),('direction',ct.c_int)]
class Outputs(ct.Structure):
    _fields_=[('re',ct.POINTER(ct.c_double)),('re_size',ct.c_int),('im',ct.POINTER(ct.c_double)),('im_size',ct.c_int),('blk_exp',ct.c_int),('overflow',ct.c_int)]
work=Path(sys.argv[1]);vectors=Path(sys.argv[2]);out=Path(sys.argv[3]);out.mkdir(parents=True,exist_ok=True)
profile_count=int(sys.argv[4]) if len(sys.argv)>4 else 4
prefix=sys.argv[5] if len(sys.argv)>5 else 'fft_'
dll=vectors.parent/'cmodel_runtime';dll.mkdir(exist_ok=True)
with zipfile.ZipFile('D:/vivado/2025.2/Vivado/data/ip/xilinx/xfft_v9_1/cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip') as z:z.extractall(dll)
libpath=next(dll.rglob('libIp_xfft_v9_1_bitacc_cmodel.dll'));dllpath=libpath.parent
search=os.add_dll_directory(str(dllpath.resolve()))
deps=[ct.CDLL(str(dllpath/n)) for n in ('libgmp.dll','libmpfr.dll','libIp_floating_point_v7_1_bitacc_cmodel.dll')]
lib=ct.CDLL(str(libpath));lib.xilinx_ip_xfft_v9_1_create_state.argtypes=[Generics];lib.xilinx_ip_xfft_v9_1_create_state.restype=ct.c_void_p
lib.xilinx_ip_xfft_v9_1_bitacc_simulate.argtypes=[ct.c_void_p,Inputs,ct.POINTER(Outputs)];lib.xilinx_ip_xfft_v9_1_bitacc_simulate.restype=ct.c_int
lib.xilinx_ip_xfft_v9_1_destroy_state.argtypes=[ct.c_void_p]
state=lib.xilinx_ip_xfft_v9_1_create_state(Generics(13,4,0,0,24,24,1,1,1,1,0))
assert state
ptr=lambda a:a.ctypes.data_as(ct.POINTER(ct.c_double))
reports=[];capture_files=[]
root=Path(__file__).resolve().parents[1]
hann=np.array([int(v,16) for v in (root/'data/hann_8192.mem').read_text().splitlines()],dtype=np.int64)
def rnd(x,s):
    q=x>>s;r=x-(q<<s);h=1<<(s-1);return q+((r>h)|((r==h)&((q&1)!=0)))
try:
 for p in range(profile_count):
  ins=np.loadtxt(work/f'{prefix}input_{p}.txt',dtype=np.int64);outs=np.loadtxt(work/f'{prefix}output_{p}.txt',dtype=np.int64)
  for kind in ['input','output']:
   file=work/f'{prefix}{kind}_{p}.txt';capture_files.append(dict(file=file.name,sha256=hashlib.sha256(file.read_bytes()).hexdigest(),bytes=file.stat().st_size))
  samples=np.load(vectors/f'vectors_{p}.npz')['filtered']
  stats=json.loads((vectors/f'stats_{p}.json').read_text());rtlstats=np.loadtxt(work/(f'{prefix}stats_{p}.txt' if prefix!='fft_' else f'rtl_stats_{p}.txt'),dtype=np.int64)
  for c in range(3):
   if prefix!='fft_unit_':assert list(rtlstats[c,1:6])==[stats[c][k] for k in ('sum','sumsq','min','max','clip')],('stats',p,c)
   else:assert list(rtlstats[c,[1,3,4,5]])==[stats[c][k] for k in ('sum','min','max','clip')],('preloaded input identity',p,c)
   a=ins[ins[:,0]==c];o=outs[outs[:,0]==c];assert len(a)==len(o)==8192
   assert np.array_equal(a[:,1],np.arange(8192));assert len(np.unique(o[:,1]))==8192
   mean=int(rnd(np.array([stats[c]['sum']],dtype=np.int64),13)[0]);expected=rnd((samples[:,c]-mean)*hann,24)
   assert np.array_equal(expected,a[:,2]),('Hann/mean/input',p,c)
   re=np.ascontiguousarray(a[:,2]/2**23);im=np.zeros(8192);ore=np.empty(8192);oim=np.empty(8192);sch=(ct.c_int*13)(*([0]*13))
   result=Outputs(ptr(ore),8192,ptr(oim),8192,0,0)
   status=lib.xilinx_ip_xfft_v9_1_bitacc_simulate(state,Inputs(13,ptr(re),8192,ptr(im),8192,sch,13,1),ct.byref(result));assert status==0
   expected_re=np.rint(ore*2**23).astype(np.int64);expected_im=np.rint(oim*2**23).astype(np.int64)
   assert np.all(o[:,4]==result.blk_exp),('BFP exponent',p,c)
   got=np.empty((8192,2),dtype=np.int64);got[o[:,1]]=o[:,2:4]
   assert np.array_equal(got[:,0],expected_re) and np.array_equal(got[:,1],expected_im),('complex bit accurate',p,c,np.max(abs(got[:,0]-expected_re)))
   if prefix=='fft_unit_':
    power=np.loadtxt(work/f'{prefix}powers_{p}.txt',dtype=np.uint64);bands=np.loadtxt(work/f'{prefix}bands_{p}.txt',dtype=np.uint64)
    actual_power=power[power[:,0]==c];actual_power=actual_power[np.argsort(actual_power[:,1]),2]
    refpower=expected_re[:4097]**2+expected_im[:4097]**2
    assert np.array_equal(actual_power,refpower),('power RAM',p,c)
    weighted=refpower.copy();weighted[1:-1]*=2
    reference=sum(int(v) for v in weighted[4:820]);order=sum(int(v) for v in weighted[100:201]);full=sum(int(v) for v in weighted[:3277])
    expected_bands=[order]+[0]*12+[reference,full,order,0,reference-order]
    assert list(bands[bands[:,0]==c,2])==expected_bands,('band sums/union',p,c)
   restored=(got[:,0]+1j*got[:,1])*2.**(result.blk_exp+1)/128
   ideal=np.fft.fft((samples[:,c]/128-np.mean(samples[:,c]/128))*(hann/2**23))
   peak_actual=int(np.argmax(abs(restored[1:4097]))+1);peak_reference=int(np.argmax(abs(ideal[1:4097]))+1)
   sl=slice(max(1,peak_reference-2),min(4097,peak_reference+3))
   band_relative_error=float(abs(np.linalg.norm(restored[sl])-np.linalg.norm(ideal[sl]))/np.linalg.norm(ideal[sl]))
   assert abs(peak_actual-peak_reference)<=1 and band_relative_error<=.01,('dominant band target',p,c)
   reports.append(dict(profile=p,axis=c,complex_codes_checked=8192,bit_exact=True,exponent=result.blk_exp,relative_spectrum_l2=float(np.linalg.norm(restored-ideal)/np.linalg.norm(ideal)),dominant_peak_bin_actual=peak_actual,dominant_peak_bin_double=peak_reference,dominant_five_bin_rms_relative_error=band_relative_error))
finally:lib.xilinx_ip_xfft_v9_1_destroy_state(state)
summary=dict(status='passed' if profile_count==4 else 'partial_profiles_passed',method='actual XFFT RTL, preloaded FIR result RAM' if prefix=='fft_unit_' else 'whole raw-to-FFT RTL',FIR_profiles=0 if prefix=='fft_unit_' else profile_count,FIR_sample_checks=0 if prefix=='fft_unit_' else 8192*3*profile_count,stats_exact=None if prefix=='fft_unit_' else True,stats_verification='separate FIR-unit tests' if prefix=='fft_unit_' else 'full raw-to-FFT RTL',hann_input_exact=True,fft_complex_checks=8192*3*profile_count,power_and_bands_exact=True if prefix=='fft_unit_' else None,RTL_vs_installed_AMD_model=reports,board_verified=False,capture_files=capture_files,vivado='2025.2',ip_vlnv='xilinx.com:ip:xfft:9.1',ip_revision=15,installed_vendor_model_sha256=hashlib.sha256(libpath.read_bytes()).hexdigest(),rtl_sha256=hashlib.sha256((root/'rtl/detection_accel.sv').read_bytes()).hexdigest(),fir_rom_sha256=hashlib.sha256((root/'data/fir_all.mem').read_bytes()).hexdigest(),hann_rom_sha256=hashlib.sha256((root/'data/hann_8192.mem').read_bytes()).hexdigest())
report_name=('fft_numeric_verification.json' if prefix=='fft_unit_' else 'numeric_verification.json') if profile_count==4 else f'numeric_verification_profiles_{profile_count}.json'
(out/report_name).write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
