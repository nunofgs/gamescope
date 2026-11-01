from pathlib import Path
import hashlib
import json
import os
import subprocess
import time

root = Path(__file__).resolve().parent
tracer = root / 'trace.so'
results = []
mask = (1 << 48) - 1
for case in ['legacy', 'field', 'padding', 'none']:
    environment = os.environ.copy()
    environment.pop('RADV_DEBUG', None)
    environment['LD_PRELOAD'] = str(tracer) + ':libdrm.so.2'
    environment['WSI_REPRO_BO_TRACE'] = str(root / ('image-' + case + '.trace.jsonl'))
    environment['WSI_REPRO_BO_TRACE_SUBMITS'] = '8'
    Path(environment['WSI_REPRO_BO_TRACE']).write_text('')
    started = time.monotonic()
    process = subprocess.run([str(root / 'image-probe'), case, str(root / 'sample.spv')], capture_output=True, text=True,
                             env=environment, timeout=5)
    (root / ('image-' + case + '.stdout')).write_text(process.stdout)
    (root / ('image-' + case + '.stderr')).write_text(process.stderr)
    process.check_returncode()
    record = json.loads(process.stdout)
    events = [json.loads(line) for line in
              Path(environment['WSI_REPRO_BO_TRACE']).read_text().splitlines()]
    submissions = [event for event in events if event['kind'] == 'submit']
    if not submissions:
        raise RuntimeError('No measured GPU submissions')
    record['elapsed_ms'] = round((time.monotonic() - started) * 1000)
    record['submissions'] = len(submissions)
    if len(submissions) != 2:
        raise RuntimeError(('Expected initialization and compute submissions', submissions))
    mappings = [event for event in events if event['kind'] == 'va'
                and event['operation'] == 1 and event['result'] == 0
                and record['allocation_start_ns'] <= event['ns'] <= record['allocation_end_ns']]
    identities = {(event['fd'], event['handle']) for event in mappings}
    if len(identities) != 1:
        raise RuntimeError(('Ambiguous image allocation identity', mappings))
    image = mappings[-1]
    record['image_handle'] = image['handle']
    record['image_initialization_present'] = (image['fd'] == submissions[0]['fd']
                                             and image['handle'] in submissions[0]['bo_handles'])
    record['image_compute_present'] = (image['fd'] == submissions[1]['fd']
                                      and image['handle'] in submissions[1]['bo_handles'])
    if not record['image_initialization_present']:
        raise RuntimeError('Image missing from explicit initialization control')
    address = int(record['control_va'], 16) & mask
    control_maps = [event for event in events if event['kind'] == 'va'
                    and event['operation'] == 1 and event['result'] == 0
                    and (int(event['va'], 16) & mask) <= address
                    < (int(event['va'], 16) & mask) + event['size']]
    if len({(event['fd'], event['handle']) for event in control_maps}) != 1:
        raise RuntimeError(('Ambiguous output buffer identity', control_maps))
    control = control_maps[-1]
    record['control_handle'] = control['handle']
    record['control_compute_present'] = (control['fd'] == submissions[1]['fd']
                                        and control['handle'] in submissions[1]['bo_handles'])
    if not record['control_compute_present']:
        raise RuntimeError('Output control missing from compute')
    results.append(record)
    print(json.dumps(record), flush=True)
manifest = {
    'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
    'source_sha256': hashlib.sha256((root / 'image-probe.c').read_bytes()).hexdigest(),
    'binary_sha256': hashlib.sha256((root / 'image-probe').read_bytes()).hexdigest(),
    'tracer_sha256': hashlib.sha256(tracer.read_bytes()).hexdigest(),
    'scope': 'One 1x1 RGBA8 image, one output buffer, explicit clear followed by one compute sample. '
             'Legacy struct padding is explicitly zeroed. No game, compositor, capture or presentation.',
    'shader_sha256': hashlib.sha256((root / 'sample.spv').read_bytes()).hexdigest(),
    'results': results,
}
(root / 'image-results.json').write_text(json.dumps(manifest, indent=2) + '\n')
