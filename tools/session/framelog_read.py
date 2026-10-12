"""framelog_read.py: reader of a FOIR_FRAMELOG file (foir's src/framelog.h; foir's tools/framelog.py is the full viewer): one record per UpdateScreen."""
import struct

F = ('t_enter', 't_leave', 'frame', 'lists', 'cb', 'cb_max', 'syncfull', 'mtsync', 'drain', 'vi', 'present', 'cmds', 'prims', 'pixels',
     'loads', 'wk_sum', 'wk_max', 'jobs', 'vi_res', 'thr_cpu', 'thr_faults', 'thr_ctxsw', 'helper', 'reserved')


def read(path):
    d = open(path, 'rb').read()
    magic, rb, cap, n = struct.unpack_from('<8sIII', d, 0)
    assert magic == b'FOIRFL01' and rb in (96, 104), 'not a FOIR_FRAMELOG file'      # 96: before the helper_ns field
    fmt = '<QQ17I3I' + ('2I' if rb == 104 else '')
    return [dict(zip(F, struct.unpack_from(fmt, d, 64 + i * rb) + ((0, 0) if rb == 96 else ()))) for i in range(n)]
