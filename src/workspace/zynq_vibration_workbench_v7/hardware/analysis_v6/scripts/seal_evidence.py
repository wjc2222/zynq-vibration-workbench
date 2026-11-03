"""Seal current PL artifacts and unchanged capture source hashes. No deployment."""
from pathlib import Path
import hashlib,json,shutil,re,datetime,sys
src=Path(__file__).resolve().parents[1]
candidate=src.parents[1]
build=Path(sys.argv[1])
out=candidate/'artifacts/pl-v7';out.mkdir(parents=True,exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
original=src/'platform_source'
implemented=build.parent/'source'
protected=[]
for folder in ['combined/rtl','combined/constraints','display/rtl','display/board','display/vendor']:
 for f in sorted((original/folder).rglob('*')):
  if f.is_file():
   rel=f.relative_to(original);other=implemented/rel
   assert other.exists() and sha(f)==sha(other),f'Protected source changed: {rel}'
   protected.append(dict(path=str(rel).replace('\\','/'),sha256=sha(f)))
for f in (src/'rtl').glob('*'):
 other=implemented/'analysis_v6/rtl'/f.name
 assert sha(f)==sha(other),f'Build RTL differs: {f.name}'
for f in (src/'data').glob('*.mem'):
 other=implemented/'analysis_v6/data'/f.name
 assert sha(f)==sha(other),f'Build coefficient/window bytes differ: {f.name}'
coeff=json.loads((src/'data/coefficients.json').read_text())
all_coefficients=(src/'data/fir_all.mem').read_text(encoding='ascii').split()
assert len(all_coefficients)==4096
for profile,p in enumerate(coeff['profiles']):
 f=src/f'data/fir_{p["fs"]}.mem'
 assert sha(f)==p['file_sha256']
 canonical=('\n'.join(f.read_text(encoding='ascii').splitlines())+'\n').encode('ascii')
 assert hashlib.sha256(canonical).hexdigest()==p['sha256']
 values=f.read_text(encoding='ascii').split()
 assert all_coefficients[profile*1024:profile*1024+len(values)]==values,'ROM and canonical FIR coefficients differ'
 assert values==values[::-1] and len(values)%2==1,'Symmetric FIR requires odd symmetric coefficients'
files=[]
for rel in ['README.md','V7_PERFORMANCE_SCOPE.md']:
 f=src/rel;files.append(dict(path=rel,bytes=f.stat().st_size,sha256=sha(f)))
for sub in ['rtl','scripts','tests','data','platform_source']:
 for f in sorted((src/sub).rglob('*')):
  if f.is_file():files.append(dict(path=str(f.relative_to(src)).replace('\\','/'),bytes=f.stat().st_size,sha256=sha(f)))
for folder in ['reports','export']:
 for f in sorted((build/folder).glob('*')):
  if f.is_file():
   if f.name.endswith('_prebit.xsa'):continue
   target=out/folder/f.name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
ip_xci=build.parent/'fft_ip/fft_v6.srcs/sources_1/ip/detection_fft_8192/detection_fft_8192.xci'
shutil.copy2(ip_xci,out/'export/detection_fft_8192.xci')
timing=(build/'reports/timing_summary.rpt').read_text(errors='replace')
m=re.search(r'WNS\(ns\).*?\n\s*-+.*?\n\s*([\d.-]+)\s+([\d.-]+)\s+(\d+)\s+(\d+)\s+([\d.-]+)',timing,re.S)
assert m,'Timing summary could not be parsed'
wns=float(m[1]);whs=float(m[5]);assert wns>=0 and whs>=0,'Timing failure prohibits passing evidence'
assert int(m[3])==0,'Failing setup endpoints prohibit passing evidence'
assert (build/'export/BUILD_STATUS.txt').read_text().startswith('PASS:'),'Implementation export gate did not pass'
drc=(build/'reports/drc.rpt').read_text(errors='replace')
assert not re.search(r'\|\s*\S+\s*\|\s*Error\s*\|',drc),'DRC Error prohibits passing evidence'
cdc=(build/'reports/cdc.rpt').read_text(errors='replace')
cdc_rows=[line for line in cdc.splitlines() if line.startswith(('Critical','Info'))]
baseline_cdc=candidate.parent/'zynq_vibration_detection_v6/artifacts/pl-v6/reports/cdc.rpt'
assert baseline_cdc.exists(),'Protected V6 CDC comparison required'
baseline_cdc_rows=[line for line in baseline_cdc.read_text(errors='replace').splitlines() if line.startswith(('Critical','Info'))]
cdc_unsafe=sum(int(line.split()[-3]) for line in cdc_rows)
cdc_same=[line.split() for line in cdc_rows]==[line.split() for line in baseline_cdc_rows]
assert cdc_same,'Changed CDC summary requires separate review before sealing'
fir=json.loads((out/'fir_performance_verification.json').read_text())
assert fir['status']=='passed' and fir['symmetric_exact_sample_checks']==688128
assert fir['source_sha256']['rtl/detection_accel.sv']==sha(src/'rtl/detection_accel.sv'),'FIR evidence does not bind final RTL'
numeric=json.loads((out/'numeric_verification.json').read_text())
assert numeric['status']=='passed' and numeric['FIR_profiles']==4 and numeric['fft_complex_checks']==98304
assert numeric['stats_exact'] and numeric['hann_input_exact'] and all(r['bit_exact'] for r in numeric['RTL_vs_installed_AMD_model'])
assert numeric['rtl_sha256']==sha(src/'rtl/detection_accel.sv'),'FFT evidence does not bind final RTL'
utilization=(build/'reports/utilization.rpt').read_text(errors='replace')
def used_metric(label):
 match=re.search(r'\|\s*'+re.escape(label)+r'\*?\s*\|\s*([\d.]+)',utilization)
 assert match,f'Missing resource metric: {label}'
 return float(match[1]) if '.' in match[1] else int(match[1])
seal=dict(schema='zynq_v7_pl_implementation_manifest_v1',generated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
 architecture='ps_fed_axil_replay_coprocessor',rawtap_implemented=False,hp2_dma_implemented=False,
 fir_architecture='symmetric_preadd_three_axis_parallel',fir_multiplier_lanes=3,
 complete_implementation_timing_passed=True,wns_ns=wns,whs_ns=whs,
 complete_platform_resources=dict(lut=used_metric('Slice LUTs'),ff=used_metric('Register as Flip Flop'),bram36_equivalent=used_metric('Block RAM Tile'),dsp=used_metric('DSPs')),
 complete_vivado_project=str(build/'vivado/ax7z020_combined.xpr'),complete_vivado_project_sha256=sha(build/'vivado/ax7z020_combined.xpr'),
 fft_ip_xci_sha256=sha(ip_xci),
 cdc_unsafe_endpoints=cdc_unsafe,cdc_summary_identical_to_protected_v6=cdc_same,drc_error_count=0,
 inherited_constraint_warning='ADC output set_max_delay -datapath_only without explicit -from in unchanged baseline XDC; not a zero-warning build',
 raw_capture_source_unchanged=True,protected_source_files=protected,source_files=files,
 reports=[dict(path='reports/'+f.name,bytes=f.stat().st_size,sha256=sha(f)) for f in sorted((out/'reports').glob('*')) if f.is_file()],
 exports=[dict(path='export/'+f.name,bytes=f.stat().st_size,sha256=sha(f)) for f in sorted((out/'export').glob('*')) if f.is_file()],
 verification_evidence=[dict(path=f.relative_to(out).as_posix(),bytes=f.stat().st_size,sha256=sha(f)) for f in sorted(out.rglob('*')) if f.is_file() and f.name!='IMPLEMENTATION_MANIFEST.json' and f.relative_to(out).parts[0] not in ('reports','export')],
 numeric_verification_sha256=sha(out/'numeric_verification.json'),fir_performance_verification_sha256=sha(out/'fir_performance_verification.json'),
 board_verification='parent-controlled; not implied by this manifest',
 pending_design_items=['live independent raw tap and CDC','HP2 DMA input/result transport','three-slot live overlap manager','precise tach event path','physical calibration'])
(out/'IMPLEMENTATION_MANIFEST.json').write_text(json.dumps(seal,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(implementation='passed',wns_ns=wns,whs_ns=whs,exports=seal['exports']),indent=2))
