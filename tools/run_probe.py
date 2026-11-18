"""Run only our native prototype, with a bounded diagnostic interval."""
from pathlib import Path
import argparse, datetime, hashlib, json, os, re, subprocess, time

PORT=Path(__file__).resolve().parents[1]
BUTTONS={'NONE':0xffff,'START':0xfff7,'SELECT':0xfffe,'CROSS':0xbfff,
         'CIRCLE':0xdfff,'SQUARE':0x7fff,'TRIANGLE':0xefff,
         'UP':0xffef,'RIGHT':0xffdf,'DOWN':0xffbf,'LEFT':0xff7f,
         'L2':0xfeff,'R2':0xfdff,'L1':0xfbff,'R1':0xf7ff}

def scripted_input(value):
    try:
        frame,button=value.split(':'); frame=int(frame); button=button.upper()
        if not 1<=frame<=18000 or button not in BUTTONS:raise ValueError()
        return frame,button
    except ValueError:
        raise argparse.ArgumentTypeError('Use FRAME:BUTTON, frame 1..18000 and button '+', '.join(BUTTONS))

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--seconds',type=int,default=15)
    parser.add_argument('--stack',action='store_true')
    parser.add_argument('--press-start',action='store_true')
    parser.add_argument('--input',action='append',type=scripted_input,default=[],help='FRAME:BUTTON in prototype UI frames; use NONE to release')
    parser.add_argument('--capture-frames',action='store_true')
    parser.add_argument('--no-diag-dumps',action='store_true',help='Measure execution without periodic PNG/VRAM exports')
    parser.add_argument('--renderer',choices=['auto','cpu','d3d11'],help='Select a renderer explicitly for an A/B comparison')
    parser.add_argument('--gpu-verify',type=int,default=0,help='Compare the first N GPU textured sprites against full CPU VRAM')
    parser.add_argument('--lightweight',action='store_true',help='Keep timing and menu state without full GS/kernel diagnostics')
    parser.add_argument('--dump-ram',action='store_true',help='Save private EE RAM only if runtime stops itself')
    parser.add_argument('--live-inputs',action='store_true',help='Accept bounded internal pad commands from control_probe.py')
    parser.add_argument('--ipu-verify',type=int,default=0,help='Compare the first N in-process ipu2 decodes with the ffmpeg.exe reference')
    parser.add_argument('--ipu-cli',action='store_true',help='Use the former external ffmpeg.exe ipu2 decoder (A/B measurement only)')
    args=parser.parse_args()
    if not 1<=args.seconds<=300:parser.error('Duration must be 1..300 seconds')
    if len({frame for frame,_ in args.input})!=len(args.input):parser.error('Duplicate input frame')
    if args.press_start and any(frame in (600,630) for frame,_ in args.input):parser.error('Input overlaps START preset')
    runner=PORT/'build-tools/ps2xRuntime/Release/WarriorsOrochi.exe'
    elf=PORT/'local/SLUS_216.62'
    iso=PORT.parent/'Warriors Orochi (USA).iso'
    assert runner.exists() and elf.exists() and iso.exists()
    logs=PORT/'logs';logs.mkdir(exist_ok=True)
    start=time.monotonic();started_wall=time.time();timed_out=False
    environment=os.environ.copy()
    for name in ('OROCHI_TEST_START','OROCHI_TEST_INPUTS','OROCHI_CAPTURE_DIR','OROCHI_CAPTURE_RAM','OROCHI_TEST_CONTROL_FILE','PS2X_IOP_RPC_TRACE_SID','OROCHI_IPU_DECODER','OROCHI_IPU_VERIFY'):environment.pop(name,None)
    # 2026-10-23: probes keep boot-frame.png/gpu-vram.bin; manual launches no longer write them.
    environment.pop('OROCHI_DIAG_DUMPS',None)
    if not args.no_diag_dumps:environment['OROCHI_DIAG_DUMPS']='1'
    if args.renderer:environment['OROCHI_RENDERER']=args.renderer
    environment.pop('OROCHI_LIGHTWEIGHT_DIAGNOSTICS',None)
    if args.lightweight:environment['OROCHI_LIGHTWEIGHT_DIAGNOSTICS']='1'
    if args.gpu_verify>0:environment['OROCHI_GPU_VERIFY']=str(min(args.gpu_verify,1000))
    if args.ipu_verify>0:environment['OROCHI_IPU_VERIFY']=str(min(args.ipu_verify,1000))
    if args.ipu_cli:environment['OROCHI_IPU_DECODER']='ffmpeg-cli'
    if args.live_inputs:
        control=PORT/'local/probe-control.txt'
        control.write_text('',encoding='ascii')
        environment['OROCHI_TEST_CONTROL_FILE']=str(control)
    if args.dump_ram:
        environment['OROCHI_CAPTURE_RAM']='1'
        environment['PS2X_IOP_RPC_TRACE_SID']='0x4b534e47'
    if args.press_start:environment['OROCHI_TEST_START']='1'
    if args.input:environment['OROCHI_TEST_INPUTS']=','.join(f'{frame}:{BUTTONS[button]}' for frame,button in sorted(args.input))
    captures=None
    if args.capture_frames:
        captures=PORT/'local/probe-frames'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        environment['OROCHI_CAPTURE_DIR']=str(captures)
    with (logs/'boot.stdout.log').open('wb') as out,(logs/'boot.stderr.log').open('wb') as err:
        process=subprocess.Popen([str(runner),str(elf),str(iso)],cwd=PORT/'local',stdout=out,stderr=err,
                                 creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),env=environment)
        try:
            if args.stack:
                try:
                    process.wait(timeout=min(5,args.seconds/2))
                except subprocess.TimeoutExpired:
                    helper=PORT/'build-tools/diagnostics/Release/OrochiStackProbe.exe'
                    with (logs/'boot.stack.log').open('w',encoding='utf8') as stack:
                        subprocess.run([str(helper),str(process.pid)],stdout=stack,stderr=subprocess.STDOUT,timeout=15)
            code=process.wait(timeout=max(0.1,args.seconds-(time.monotonic()-start)))
        except subprocess.TimeoutExpired:
            timed_out=True;process.kill();code=process.wait()
        finally:
            if process.poll() is None:
                process.kill();process.wait()
    stdout=(logs/'boot.stdout.log').read_text(encoding='utf8',errors='replace')
    stderr=(logs/'boot.stderr.log').read_text(encoding='utf8',errors='replace')
    blocker=re.search(r'\[Orochi:blocker\] ([^\r\n]+)',stderr)
    missing=re.search(r'\[guest-branch:missing-target\].*?source=(0x[0-9a-f]+) target=(0x[0-9a-f]+)',stderr,re.I)
    ticks=re.findall(r'\[run:tick\] tick=(\d+) pc=(0x[0-9a-f]+).*?activeThreads=(\d+) dma=(\d+) gif=(\d+) gsw=(\d+) vif=(\d+)',stdout,re.I)
    unchanged=len(ticks)>=3 and len({tuple(t[1:]) for t in ticks[-3:]})==1
    modules=re.findall(r'\[Orochi:iop-load\].*?module=(\d+) startResult=(-?\d+)',stdout)
    files=re.findall(r'\[Orochi:disc-search\] (.+?) layer=(\d+) found=(\d+) sector=(\d+) bytes=(\d+)',stdout)
    threads={}
    for entry in re.findall(r'\[Orochi:thread\] ([^\r\n]+)',stdout):
        fields=dict(re.findall(r'(\w+)=(0x[0-9a-f]+|\d+)',entry,re.I))
        if 'id' in fields:threads[fields['id']]=fields
    screenshot=PORT/'relatorios/boot-frame.png'
    status=('blocked_missing_function' if missing else
            'blocked_native_adapter' if blocker else
            'host_process_failed' if code!=0 and not timed_out else
            'suspected_boot_stall' if timed_out and unchanged else
            'stopped_at_test_request' if 'control STOP' in stdout and code==0 else
            'ran_until_probe_timeout_unverified' if timed_out else 'stopped_unverified')
    report=dict(seconds=round(time.monotonic()-start,2),exit_code=code,
                scripted_start_requested=args.press_start,
                scripted_inputs_requested=args.input,
                captures_directory=str(captures) if captures else None,
                scripted_inputs_logged=re.findall(r'\[Orochi:test-input\] ([^\r\n]+)',stdout),
                stopped_by_probe_timeout=timed_out,executable=runner.name,
                executable_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
                live_inputs_enabled=args.live_inputs,
                renderer_requested=args.renderer or environment.get('OROCHI_RENDERER','auto'),
                lightweight_diagnostics=args.lightweight,
                gpu_verified_sprites_logged=re.findall(r'\[Orochi:gpu-verify\] checked=(\d+) identical=1',stdout),
                gpu_summary=re.findall(r'\[Orochi:gpu-summary\] ([^\r\n]+)',stdout),
                status=status,
                entered_recompiled_code='Entering recompiled PS2 code.' in stdout,
                first_missing_function=({'source':missing.group(1),'target':missing.group(2)} if missing else None),
                runtime_blocker=blocker.group(1) if blocker else None,
                verified_overlays=re.findall(r'\[Orochi:overlay\] (\S+) verified',stdout),
                iop_modules_loaded=len([m for m in modules if int(m[0])>0]),
                disc_searches=[dict(path=f[0],layer=int(f[1]),found=f[2]=='1',sector=int(f[3]),bytes=int(f[4])) for f in files],
                last_runtime_snapshot=(dict(zip(['tick','pc','threads','dma','gif','gs_register_writes','vif'],ticks[-1])) if ticks else None),
                last_three_snapshots_unchanged=unchanged,
                last_guest_threads=list(threads.values()),
                native_image_completions_logged=len(re.findall(r'\[Orochi:image-native\].*completed=1',stdout)),
                ipu_decoder_backend=(re.findall(r'\[Orochi:image-decoder\] backend=(\S+)',stdout) or [None])[-1],
                ipu_verify_identical=len(re.findall(r'\[Orochi:ipu-verify\].*identical=1',stdout)),
                ipu_verify_different=len(re.findall(r'\[Orochi:ipu-verify\].*identical=0',stdout)),
                perf_intervals=re.findall(r'\[Orochi:perf\] ([^\r\n]+)',stdout),
                reserved_gif_ad_warnings=len(re.findall(r'\[gs:reserved-ad\]',stdout)),
                captured_bad_gif_packets=len(re.findall(r'\[gs:bad-packet\]',stdout)),
                vsync_causes_logged=sorted(set(map(int,re.findall(r'\[Orochi:vsync\] cause=(\d+)',stdout)))),
                screenshot_captured=screenshot.exists() and screenshot.stat().st_mtime>=started_wall,
                log_tail=(stdout+'\n'+stderr)[-5000:],
                gameplay_verified=False)
    (PORT/'relatorios/teste_boot.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
