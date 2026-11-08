#!/usr/bin/env python3
"""Build and run an isolated real SSH loopback test in emulated Linux.
Uses the existing Docker rvemu-guest image and read-only rvemu-guest-work
volume (initramfs toolchain/kernel build). Never flashes a board or listens
on a host TCP port. First build may take several minutes.
"""
import argparse
import json
import re
import os
from pathlib import Path
import subprocess
import time
import statistics
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'target/ssh-eval'

def run(args,**kw): return subprocess.run(args,check=True,**kw)

def connection_measurements(text, repeats, engine, native, endpoint="on"):
    pattern=r'benchmark point tag=(\d+) guest=(\d+) host=(\d+) ns=(\d+) idle=(\d+) native=\[([^]]+)\]'
    points=[]
    for m in re.finditer(pattern,text):
        tag,guest,host,ns,idle=map(int,m.groups()[:5])
        calls=[int(x) for x in m[6].split(',')]
        if len(calls)!=13: raise ValueError('invalid native counters')
        points.append(dict(tag=tag,guest=guest,host=host,ns=ns,idle=idle,calls=calls))
    if len(points)!=repeats*4: raise ValueError('missing or duplicate connection boundaries')
    samples=[]
    for i in range(repeats):
        p=points[4*i:4*i+4]
        if [x['tag'] for x in p]!=[0,1,2,3]: raise ValueError('out-of-order connection boundaries')
        for a,b in zip(p,p[1:]):
            if b['guest']<=a['guest'] or any(b[k]<a[k] for k in ['host','ns','idle']) or any(y<x for x,y in zip(a['calls'],b['calls'])):
                raise ValueError('non-monotonic connection counters')
            if engine=='jit' and b['host']<=a['host']: raise ValueError('missing host instruction counter')
            if engine=='host' and b['ns']<=a['ns']: raise ValueError('missing host time counter')
        phases={}
        # Dropbear speculatively computes KEX before its asynchronous TCP
        # completion callback. Group setup + KEX; 0->1 is NOT pure TCP cost.
        for name,lo,hi in [('setup_and_key_exchange',0,2),('authentication',2,3),('connection',0,3)]:
            a,b=p[lo],p[hi]
            phases[name]=dict(guest_instructions=b['guest']-a['guest'],
                host_instructions=b['host']-a['host'] if engine=='jit' else None,
                host_elapsed_seconds=(b['ns']-a['ns'])/1e9 if engine=='host' else None,
                guest_idle_seconds=(b['idle']-a['idle'])/25_000_000,
                native_buffer_calls=[y-x for x,y in zip(a['calls'],b['calls'])])
        calls=phases['connection']['native_buffer_calls']
        if not ((calls[4]>0 and calls[5]>0) if native else calls[4]==calls[5]==0):
            raise ValueError('connection did not use the selected crypto path')
        required={6,8,9,10,11,12}
        if endpoint=='client': required.remove(11)
        if endpoint=='server': required-={10,12}
        if native and any(calls[op]==0 for op in required): raise ValueError('missing native handshake operations')
        if not native and any(calls): raise ValueError('software connection used native operations')
        if phases['connection']['guest_idle_seconds']>=60: raise ValueError('connection stalled')
        samples.append(phases)
    medians={name:{key:statistics.median(s[name][key] for s in samples) if samples[0][name][key] is not None else None
        for key in ['guest_instructions','host_instructions','host_elapsed_seconds','guest_idle_seconds']} for name in samples[0]}
    return samples,medians

def isolate_connection_counters(summary):
    """Keep boot/session totals explicitly separate from connection results."""
    summary['whole_run_diagnostics']={key:summary.pop(key) for key in
        ['guest_instructions','host_instructions','guest_idle_seconds','host_elapsed_seconds','native_buffer_calls']}
    summary.pop('bulk_mib',None)
    summary.pop('whole_run_counters_are_connection_measurements',None)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--skip-build',action='store_true')
    p.add_argument('--test',choices=['integration','connection'],default='integration',help='connection measures TCP connect through successful SSH authentication only')
    p.add_argument('--repeats',type=int,default=1,help='connection samples per boot (1–8, default 1); later samples retain translator caches')
    p.add_argument('--memory',type=int,default=32,help='guest MiB (default 32; try 8 after establishing baseline)')
    p.add_argument('--without-extension',action='store_true',help='test software fallback on firmware without the native extension')
    p.add_argument('--bulk-mib',type=int,default=1,help='streamed MiB per direction without storing the whole payload')
    p.add_argument('--offload',choices=['on','off','client','server'],default='on')
    p.add_argument('--engine',choices=['host','jit','lockstep'],default='host')
    p.add_argument('--xip',action='store_true',help='host only: model kernel text in flash, as on the board')
    p.add_argument('--timeout',type=int,default=300,help='emulator wall-clock timeout')
    a=p.parse_args()
    if a.memory < 8 or a.timeout <= 0 or not 1 <= a.bulk_mib <= 16 or not 1<=a.repeats<=8:
        p.error('use at least 8 MiB, a positive timeout, 1–16 bulk MiB, and 1–8 repeats')
    if a.test=='connection' and a.engine=='lockstep': p.error('connection timing supports host or jit; lockstep includes reference execution')
    if a.xip and a.engine!='host': p.error('--xip is currently supported only with --engine host')
    OUT.mkdir(parents=True,exist_ok=True)
    if not a.skip_build:
        archive=OUT/'dropbear.tar.bz2'
        if not archive.exists():
            with urllib.request.urlopen('https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2',timeout=60) as r:
                archive.write_bytes(r.read())
        run(['docker','volume','create','rvemu-ssh-eval-work'],stdout=subprocess.DEVNULL)
        with (OUT/'build.log').open('w') as log:
            run(['docker','run','--rm','--user','0','-v','rvemu-guest-work:/work:ro','-v','rvemu-ssh-eval-work:/eval',
                 '-v',f'{ROOT.parent}:/repo:ro','-v',f'{OUT}:/out','rvemu-guest',
                 'bash','/repo/guest/tests/build-ssh-eval.sh'],stdout=log,stderr=subprocess.STDOUT)
    features=[] if a.without_extension else ['--features','pv-buffers']
    run(['cargo','build','--release','-p','rvemu',*features],cwd=ROOT)
    cmdline=f'earlycon console=ttyS0 rdinit=/init ssh_offload={a.offload} ssh_bulk_mib={a.bulk_mib} ssh_test={a.test} ssh_repeats={a.repeats}'
    command=[str(ROOT/'target/release/rvemu'),'-k',str(OUT/'Image'),'-i',str(OUT/'rootfs.cpio'),
        '-m',str(a.memory),'-a',cmdline,'-B','-u','SSH TEST PASS']
    if a.xip:
        symbols={line.split()[2]:int(line.split()[0],16) for line in (OUT/'System.map').read_text().splitlines() if len(line.split())==3}
        command += ['-X',str((symbols['_etext']-symbols['_start']) & ~4095)]
    if a.engine!='host':
        command=['bash',str(ROOT/'scripts/virt.sh'),a.engine,'-k',str(OUT/'Image'),'-i',str(OUT/'rootfs.cpio'),
            '-m',str(a.memory),'-a',cmdline,'-u','SSH TEST PASS','-n','60000','-c']
    workload=f'bulk{a.bulk_mib}' if a.test=='integration' else f'connection-r{a.repeats}'
    name=f'{a.engine}-{a.memory}-{a.offload}-{workload}'+('-xip' if a.xip else '')+('-noext' if a.without_extension else '')
    start=time.monotonic()
    with (OUT/f'run-{name}.log').open('w') as log:
        try:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=a.timeout,env={**os.environ,'RVEMU_FEATURES':'' if a.without_extension else 'pv-buffers'})
        except subprocess.TimeoutExpired:
            raise SystemExit(f'SSH test timed out; inspect {OUT / ("run-"+name+".log")}')
    text=(OUT/f'run-{name}.log').read_text()
    required=['SSH AUTH PASS','SSH EXIT STATUS PASS','SSH TRANSFER PASS 65536 bytes each direction',
              'SSH WRONG KEY REJECTED','No auth methods could be used.','SSH TEST PASS',f'SSH STREAM PASS {a.bulk_mib} MiB each direction']
    if a.test=='connection': required=['SSH TEST PASS',*[f'SSH CONNECTION PASS {i}' for i in range(1,a.repeats+1)]]
    passed=result.returncode==0 and all(s in text for s in required) and 'SSH TEST FAIL' not in text
    counts=re.search(r'native buffer calls \[([^]]+)\]',text)
    calls=[int(n) for n in counts[1].split(',')] if counts else []
    threshold=10 if a.test=='integration' else 1
    # without the extension the Mac host prints no native counters at all
    if a.without_extension: passed=passed and not any(calls)
    else: passed=passed and len(calls)==13 and ((not any(calls)) if a.offload=='off' else (calls[4]>threshold and calls[5]>threshold))
    if a.offload!='off' and not a.without_extension: passed=passed and 'SSH IOCTL BOUNDS AND AUTH PASS' in text
    if a.test=='integration' and a.offload!='off' and not a.without_extension:
        passed=passed and 'GENERIC CRYPTO ABI PASS' in text
    guest_count=re.search(r'guest instructions (\d+)',text)
    host_count=re.search(r'host instructions (\d+)',text)
    idle_count=re.search(r'idle ticks (\d+)',text)
    idle_seconds=int(idle_count[1])/25_000_000 if idle_count else None
    # The loopback workload sleeps for one second. Catch the final-channel
    # shutdown regression, which otherwise passes after an eight-hour rekey.
    passed=passed and idle_seconds is not None and idle_seconds < 60
    summary=dict(bulk_mib=a.bulk_mib,guest_instructions=int(guest_count[1]) if guest_count else None,
        host_instructions=int(host_count[1]) if host_count else None,guest_idle_seconds=idle_seconds,without_extension=a.without_extension,native_buffer_calls=calls,offload=a.offload,passed=passed,engine=a.engine,memory_mib=a.memory,xip=a.xip,host_elapsed_seconds=round(time.monotonic()-start,3),
        scope='guest loopback, real SSH, kernel ioctl + native SSH packet operation with software fallback',log=str(OUT/f'run-{name}.log'))
    summary['test']=a.test
    summary['crypto_execution']='native emulator code via kernel/SBI; outside guest translation' if a.offload!='off' and not a.without_extension else 'Dropbear guest software cipher'
    if a.test=='connection':
        summary['scope']='TCP connect through initial key exchange and successful client-side authentication; both loopback endpoints; excludes boot, persistent-key generation, command execution and teardown'
        summary['cache_scope']='first connection after fresh boot; later samples, if requested, retain translator caches'
        try:
            samples,medians=connection_measurements(text,a.repeats,a.engine,a.offload!='off' and not a.without_extension,a.offload)
            summary['connection_samples']=samples
            summary['connection_medians']=medians
            summary['negotiated_kex']=re.findall(r'SSH CONNECTION KEX ([^\s]+)',text)
            if len(summary['negotiated_kex'])!=a.repeats:
                raise ValueError('missing negotiated key-exchange algorithm')
        except ValueError as error:
            summary['measurement_error']=str(error)
            passed=False
        summary['passed']=passed
        isolate_connection_counters(summary)
    (OUT/f'result-{name}.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))
    if not passed:
        print(text[-6000:]);raise SystemExit(1)
