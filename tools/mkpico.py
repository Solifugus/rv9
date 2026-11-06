#!/usr/bin/env python3
"""
Turn Espressif's detectors into files `detect` can read.

    tools/mkpico.py pedestrian_detect_pico_s8_v1.espdl /tmp/pico.rv9m
    tools/mkpico.py human_face_detect_msr_s8_v1.espdl  /tmp/face1.rv9m
    tools/mkpico.py human_face_detect_mnp_s8_v1.espdl  /tmp/face2.rv9m
    tools/mkpico.py human_face_feat_mfn_s8_v1.espdl    /tmp/feat.rv9m

Which of the three a file is follows from its outputs, and is written into
it, so the engine knows how to read the answer.

The .espdl file is a FlatBuffers graph meant for ESP-DL's C++ loader. RV-9's
engine is a freestanding module with no FlatBuffers, no allocator and no
floating point, so everything that can be decided once is decided here:

  - the graph is flattened into a table of nodes in execution order;
  - weights are reordered out of ESP-DL's vector-lane packing into plain
    layouts the C kernels walk directly;
  - every activation tensor is given an offset in one arena, planned from
    when each tensor is last read, so the engine never allocates.

The models are Espressif's (esp-dl, models/pedestrian_detect and
models/human_face_detect, MIT). This changes their container, not one
weight.

THE FILE, version 3, all little-endian:

    header   magic "RV9M", version, tensors, nodes, input tensor,
             kind (KIND_*), input h, input w, flags (FL_BGR),
             outputs, output tensors[8], arena bytes, blob bytes  (80 bytes)
    tensor   h, w, c, stride (u16), exp (i16), 0, arena offset (u32) (16 each)
    node     op, relu, k, stride, pad top, bottom, left, right,
             flags, shift (i8), 0, 0, in0, in1, out (u16), 0,
             w_off, b_off (NONE: no bias), aux (u32)               (32 each)
    blob     weights, biases and tables, each 16-byte aligned

CHANNELS ARE PADDED TO SIXTEENS in memory: a pixel of a 24-channel tensor
takes 32 bytes, its `stride`, and the last 8 are never read for an answer.
That is what lets the P4's vector unit, which loads sixteen aligned bytes,
run every layer rather than only those whose channels happen to come in
sixteens -- the face detector's first stage is mostly 24, 20, 8 and 2. The
weights are padded to match, with zeros, so a padded channel contributes
nothing and computes nothing but zero. The input picture alone is not
padded (stride 3): padding it would make it five times the size.

Weights in the blob:
    conv     [cout/16 rounded up][k][k][cin stride][16] int8: sixteen
             output channels side by side, as the vector unit multiplies
             them -- ESP-DL's own order, padded; then bias, int32, one per
             output channel rounded up to sixteen, if it has one
    dwconv   [k][k][c stride] int8, then bias [c stride] int32 if any
    sigmoid  256 int8: y = table[x + 128]
    prelu    alpha [c stride] int8; shift = the negative side's right shift,
             aux = the positive side's left shift
    gap      aux = the float32 bits of 2^(in-out) / (h*w), which ESP-DL
             multiplies by in float; the engine reproduces that exactly
"""
import struct
import sys

OP_CONV, OP_DW, OP_GAP, OP_LUT, OP_MUL, OP_UP2, OP_CAT, OP_PRELU, OP_ADD = range(1, 10)
KIND_PICO, KIND_MSR, KIND_MNP, KIND_FEAT = 1, 2, 3, 4
FL_BGR = 1     # header flags: the model wants blue first
FL_CENTRED = 2 # and its input as (v - 127.5) / 127.5, not v / 2
NONE = 0xffffffff
F_REM = 1      # flags: the conv's output channels are not a multiple of 16
F_LANES = 2    # weights kept in ESP-DL's 16-lane order, for the vector unit


class T:
    """A FlatBuffers table, read by hand: seven field kinds is all this needs."""

    def __init__(s, b, pos):
        s.b, s.pos = b, pos
        s.vt = pos - struct.unpack_from('<i', b, pos)[0]
        s.vlen = struct.unpack_from('<H', b, s.vt)[0]

    def off(s, i):
        o = 4 + 2 * i
        return struct.unpack_from('<H', s.b, s.vt + o)[0] if o < s.vlen else 0

    def scalar(s, i, fmt, d=0):
        o = s.off(i)
        return struct.unpack_from('<' + fmt, s.b, s.pos + o)[0] if o else d

    def ref(s, i):
        o = s.off(i)
        if not o:
            return None
        p = s.pos + o
        return p + struct.unpack_from('<I', s.b, p)[0]

    def table(s, i):
        p = s.ref(i)
        return T(s.b, p) if p is not None else None

    def string(s, i):
        p = s.ref(i)
        if p is None:
            return None
        n = struct.unpack_from('<I', s.b, p)[0]
        return s.b[p + 4:p + 4 + n].decode()

    def vec(s, i):
        p = s.ref(i)
        if p is None:
            return 0, 0
        return struct.unpack_from('<I', s.b, p)[0], p + 4

    def tables(s, i):
        n, p = s.vec(i)
        return [T(s.b, p + 4 * k + struct.unpack_from('<I', s.b, p + 4 * k)[0])
                for k in range(n)]

    def strings(s, i):
        out = []
        n, p = s.vec(i)
        for k in range(n):
            q = p + 4 * k + struct.unpack_from('<I', s.b, p + 4 * k)[0]
            m = struct.unpack_from('<I', s.b, q)[0]
            out.append(s.b[q + 4:q + 4 + m].decode())
        return out

    def scalars(s, i, fmt, size):
        n, p = s.vec(i)
        return [struct.unpack_from('<' + fmt, s.b, p + size * k)[0]
                for k in range(n)]

    def raw(s, i):
        n, p = s.vec(i)              # [AlignedBytes]: 16 bytes each
        return s.b[p:p + 16 * n]


def attrs(n):
    out = {}
    for a in n.tables(5):
        name, kind = a.string(0), a.scalar(3, 'i')
        if kind == 2:
            out[name] = a.scalar(5, 'q')
        elif kind == 7:
            out[name] = a.scalars(11, 'q', 8)
        elif kind == 1:
            out[name] = a.scalar(4, 'f')
        elif kind == 3:
            k, p = a.vec(6)
            out[name] = a.b[p:p + k].decode()
    return out


def unpack_conv(raw, k, cin, cout):
    """ESP-DL's packing to [cout][k][k][cin].

    Output channels go in lanes of 16: all of a lane group's filters,
    [k][k][cin][16], then the next group; channels past the last whole
    group follow as [n][k][k][cin]. For cout of 16 or 1 that is the plain
    layout, which is why the first and last layers look ordinary."""
    full = cout // 16 * 16
    out = bytearray(cout * k * k * cin)
    for co in range(cout):
        for t in range(k * k):
            for ci in range(cin):
                if co < full:
                    src = ((co // 16) * k * k * cin + t * cin + ci) * 16 + co % 16
                else:
                    src = full * k * k * cin + ((co - full) * k * k + t) * cin + ci
                out[(co * k * k + t) * cin + ci] = raw[src]
    return bytes(out)


def unpack_dw(raw, k, c):
    """ESP-DL's depthwise packing to [k][k][c]: whole groups of sixteen
    channels as [c/16][k][k][16], then what is left as [k][k][rem]."""
    full, rem = c // 16 * 16, c % 16
    out = bytearray(k * k * c)
    for ch in range(c):
        for t in range(k * k):
            if ch < full:
                src = ((ch // 16) * k * k + t) * 16 + ch % 16
            else:
                src = full * k * k + t * rem + (ch - full)
            out[t * c + ch] = raw[src]
    return bytes(out)


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


KINDS = {
    KIND_PICO: ('score0', 'bbox0', 'score1', 'bbox1', 'score2', 'bbox2'),
    KIND_MSR:  ('score0', 'box0', 'score1', 'box1'),
    KIND_MNP:  ('score', 'box', 'landmark'),
    KIND_FEAT: ('embedding',),
}


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: mkpico.py <model.espdl> <out.rv9m>')
    raw = open(sys.argv[1], 'rb').read()
    if raw[:4] != b'EDL2' or struct.unpack_from('<I', raw, 4)[0] != 0:
        sys.exit('not an unencrypted .espdl file')
    b = raw[16:]
    g = T(b, struct.unpack_from('<I', b, 0)[0]).table(7)

    inits = {}
    for t in g.tables(2):
        inits[t.string(6)] = dict(dims=t.scalars(0, 'q', 8),
                                  dt=t.scalar(1, 'i'),
                                  exp=t.scalars(13, 'q', 8),
                                  raw=t.raw(8))
    exps = {}
    for v in g.tables(4) + g.tables(5) + g.tables(6):
        e = v.scalars(3, 'q', 8)
        if e:
            exps[v.string(0)] = e[0]

    graph_outs = [v.string(0) for v in g.tables(5)]
    kind = next((k for k, o in KINDS.items() if set(o) == set(graph_outs)),
                None)
    if kind is None and set(KINDS[KIND_MSR]) <= set(graph_outs):
        kind = KIND_MSR          # its landmark outputs are never read
    if kind is None:
        sys.exit('not a detector this knows: outputs %s' % graph_outs)
    # Size and channel order, which the file does not say: Espressif's
    # wrappers do (models/*/...cpp), and these are theirs.
    in_h, in_w, flags = {KIND_PICO: (224, 224, 0),
                         KIND_MSR: (120, 160, FL_BGR),
                         KIND_MNP: (48, 48, FL_BGR),
                         KIND_FEAT: (112, 112, FL_BGR | FL_CENTRED)}[kind]
    in_name = g.tables(4)[0].string(0)

    names = {}                  # tensor name -> index
    tensors = []                # [h, w, c, exp]

    def tensor(name, h, w, c):
        names[name] = len(tensors)
        tensors.append([h, w, c, exps[name]])
        return names[name]

    def stride(t):
        return 3 if t == 0 else (tensors[t][2] + 15) & ~15

    def pad16(n):
        return (n + 15) & ~15

    tensor(in_name, in_h, in_w, 3)
    blob = bytearray()

    def put(data):
        while len(blob) % 16:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        return off

    # Nodes nothing reads -- MSR's landmark heads -- are left out.
    nodes_all = g.tables(0)
    needed = set(KINDS[kind])
    for n in reversed(nodes_all):
        if n.strings(1)[0] in needed:
            needed.update(x for x in n.strings(0) if x)

    nodes = []
    for n in nodes_all:
        op, a = n.string(3), attrs(n)
        ins = [x for x in n.strings(0) if x]
        out = n.strings(1)[0]
        if out not in needed:
            continue
        src = [names[x] for x in ins if x in names]
        h, w, c = tensors[src[0]][:3]
        ein = tensors[src[0]][3]
        node = dict(op=0, relu=0, k=0, stride=0, pads=(0, 0, 0, 0), flags=0,
                    shift=0, in0=src[0], in1=src[1] if len(src) > 1 else 0xffff,
                    w_off=0, b_off=NONE, aux=0)
        if op == 'Conv':
            W = inits[ins[1]]
            B = inits[ins[2]] if len(ins) > 2 else None
            k, cin = a['kernel_shape'][0], c
            s = a['strides'][0]
            # ONNX order is top, left, bottom, right.
            pt, pl, pb, pr = a['pads']
            assert a['kernel_shape'][0] == a['kernel_shape'][1]
            assert a['strides'][0] == a['strides'][1]
            dw = a['group'] != 1
            assert not dw or a['group'] == c
            cout = c if dw else W['dims'][3]
            assert W['dims'][:3] == ([k, k, c] if dw else [k, k, cin])
            assert B is None or B['exp'][0] == ein + W['exp'][0]
            ho, wo = (h + pt + pb - k) // s + 1, (w + pl + pr - k) // s + 1
            nb = k * k * cin * (1 if dw else cout)
            cs = stride(src[0])
            if dw:
                plain = unpack_dw(W['raw'][:nb], k, c)          # [k][k][c]
                wt = bytearray(k * k * cs)
                for t in range(k * k):
                    wt[t * cs:t * cs + c] = plain[t * c:(t + 1) * c]
            else:
                plain = unpack_conv(W['raw'][:nb], k, cin, cout)  # [co][k][k][ci]
                groups = pad16(cout) // 16
                wt = bytearray(groups * k * k * cs * 16)
                for co in range(cout):
                    for t in range(k * k):
                        for ci in range(cin):
                            wt[((co // 16) * k * k + t) * cs * 16 + ci * 16 + co % 16] = \
                                plain[(co * k * k + t) * cin + ci]
            wt = bytes(wt)
            nbias = cs if dw else pad16(cout)
            bias = (B['raw'][:4 * cout] + bytes(4 * (nbias - cout))) if B else None
            assert a['activation'] in ('Relu', 'Linear')
            node.update(op=OP_DW if dw else OP_CONV, k=k, stride=s,
                        pads=(pt, pb, pl, pr),
                        relu=1 if a['activation'] == 'Relu' else 0,
                        flags=(F_REM if cout % 16 else 0) |
                              (0 if dw else F_LANES),
                        shift=exps[out] - W['exp'][0] - ein,
                        w_off=put(wt),
                        b_off=put(bias) if B else NONE)
            assert node['shift'] > 0
            o = tensor(out, ho, wo, cout)
        elif op == 'PRelu':
            A = inits[ins[1]]
            assert A['dims'][0] == c and A['dt'] == 3
            neg = exps[out] - ein - A['exp'][0]
            assert neg > 0 and ein >= exps[out]
            node.update(op=OP_PRELU, shift=neg, aux=ein - exps[out],
                        w_off=put(A['raw'][:c] + bytes(stride(src[0]) - c)))
            o = tensor(out, h, w, c)
        elif op == 'GlobalAveragePool':
            node['op'] = OP_GAP
            # As ESP-DL computes it: 2^in * (float)(1/(h*w)) * 2^-out, each
            # step float32. The powers of two are exact, so only 1/(h*w)
            # rounds -- and it must round exactly as it does there.
            inv = struct.unpack('<f', struct.pack('<f', 1.0 / (h * w)))[0]
            node['aux'] = f32_bits(2.0 ** ein * inv * 2.0 ** -exps[out])
            o = tensor(out, 1, 1, c)
        elif op == 'Sigmoid':
            L = inits[a['lut']]
            assert L['exp'][0] == exps[out] and len(L['raw']) >= 256
            node.update(op=OP_LUT, w_off=put(L['raw'][:256]))
            o = tensor(out, h, w, c)
        elif op == 'Mul':
            t1 = tensors[src[1]]
            node.update(op=OP_MUL, shift=exps[out] - ein - t1[3])
            assert node['shift'] > 0
            o = tensor(out, max(h, t1[0]), max(w, t1[1]), max(c, t1[2]))
        elif op == 'Resize':
            sc = struct.unpack('<4f', inits[ins[-1]]['raw'][:16])
            assert sc == (1.0, 1.0, 2.0, 2.0) and a['mode'] == 'nearest'
            assert exps[out] == ein
            node.update(op=OP_UP2)
            o = tensor(out, 2 * h, 2 * w, c)
        elif op == 'Add':
            # ESP-DL's int8 Add, in this version, adds and saturates and
            # never rescales -- so it is only right when the exponents
            # agree, which in these models they always do.
            t1 = tensors[src[1]]
            assert ein == t1[3] == exps[out] and tuple(t1[:3]) == (h, w, c)
            node.update(op=OP_ADD)
            o = tensor(out, h, w, c)
        elif op == 'Concat':
            # Any number of inputs: chained in pairs, the last one writing
            # the real output. Exponents must agree; ESP-DL never rescales.
            assert a['axis'] == 3
            for x in src:
                assert tensors[x][3] == exps[out] and tuple(tensors[x][:2]) == (h, w)
            acc = src[0]
            for j, x in enumerate(src[1:]):
                last = j == len(src) - 2
                cc = tensors[acc][2] + tensors[x][2]
                tname = out if last else '%s#cat%d' % (out, j)
                exps.setdefault(tname, exps[out])
                t_ = tensor(tname, h, w, cc)
                nodes.append(dict(node, op=OP_CAT, in0=acc, in1=x, out=t_))
                acc = t_
            continue
        else:
            sys.exit('unknown op ' + op)
        node['out'] = o
        nodes.append(node)

    outs = [names[x] for x in KINDS[kind]]

    # The arena: each tensor lives from the node that writes it to the last
    # node that reads it, and outputs to the end. First fit, by address.
    last = {i: i for i in range(len(tensors))}
    first = {0: -1}
    for i, n in enumerate(nodes):
        first[n['out']] = i
        for x in (n['in0'], n['in1']):
            if x != 0xffff:
                last[x] = max(last.get(x, 0), i)
    for x in outs:
        last[x] = len(nodes)

    def size(t):
        return (tensors[t][0] * tensors[t][1] * stride(t) + 15) & ~15

    offs = {}
    live = []                   # (offset, size, tensor)
    order = [0] + [n['out'] for n in nodes]
    for t in order:
        live = [x for x in live if last[x[2]] >= first[t]]
        sz, at = size(t), 0
        for o, s_, _ in sorted(live):
            if at + sz <= o:
                break
            at = max(at, o + s_)
        offs[t] = at
        live.append((at, sz, t))
    arena = max(offs[t] + size(t) for t in offs)

    f = bytearray(b'RV9M')
    f += struct.pack('<9I', 3, len(tensors), len(nodes), 0, kind, in_h, in_w,
                     flags, len(outs))
    f += struct.pack('<8I', *(outs + [0] * (8 - len(outs))))
    f += struct.pack('<II', arena, len(blob))
    assert len(f) == 80
    for i, (h, w, c, e) in enumerate(tensors):
        f += struct.pack('<HHHHhHI', h, w, c, stride(i), e, 0, offs[i])
    for n in nodes:
        f += struct.pack('<BBBBBBBBBbBBHHHHIII', n['op'], n['relu'], n['k'],
                         n['stride'], *n['pads'], n['flags'], n['shift'], 0, 0,
                         n['in0'], n['in1'], n['out'], 0,
                         n['w_off'], n['b_off'], n['aux'])
    while len(f) % 16:
        f.append(0)
    f += blob
    open(sys.argv[2], 'wb').write(f)
    print('%s: kind %d, %d nodes, %d tensors, %d KB of weights, %d KB arena, '
          '%d bytes' % (sys.argv[2], kind, len(nodes), len(tensors),
                        len(blob) // 1024, arena // 1024, len(f)))


if __name__ == '__main__':
    main()
