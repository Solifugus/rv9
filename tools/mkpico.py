#!/usr/bin/env python3
"""
Turn Espressif's pedestrian detector into a file `detect` can read.

    tools/mkpico.py pedestrian_detect_pico_s8_v1.espdl /tmp/pico.rv9m

The .espdl file is a FlatBuffers graph meant for ESP-DL's C++ loader. RV-9's
engine is a freestanding module with no FlatBuffers, no allocator and no
floating point, so everything that can be decided once is decided here:

  - the graph is flattened into a table of nodes in execution order;
  - weights are reordered out of ESP-DL's vector-lane packing into plain
    layouts the C kernels walk directly;
  - every activation tensor is given an offset in one arena, planned from
    when each tensor is last read, so the engine never allocates.

The model itself is Espressif's (esp-dl, models/pedestrian_detect, MIT).
This changes its container, not one weight.

THE FILE, all little-endian:

    header   magic "RV9M", version, tensors, nodes, input, outputs[6],
             arena bytes, blob bytes, 0                           (56 bytes)
    tensor   h, w, c (u16), exp (i16), arena offset (u32)          (12 each)
    node     op, relu, k, stride, pad, flags (u8), shift (i8), 0,
             in0, in1, out (u16), 0, w_off, b_off, aux, 0 (u32)    (32 each)
    blob     weights, biases and tables, each 16-byte aligned

Weights in the blob:
    conv     [cout][k][k][cin] int8, then bias [cout] int32
    dwconv   [k][k][c] int8, then bias [c] int32
    sigmoid  256 int8: y = table[x + 128]
    gap      aux = the float32 bits of 2^(in-out) / (h*w), which ESP-DL
             multiplies by in float; the engine reproduces that exactly
"""
import struct
import sys

OP_CONV, OP_DW, OP_GAP, OP_LUT, OP_MUL, OP_UP2, OP_CAT = range(1, 8)
F_REM = 1      # flags: the conv's output channels are fewer than 16


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
    """ESP-DL's depthwise packing, [c/16][k][k][16], to [k][k][c]."""
    out = bytearray(k * k * c)
    for ch in range(c):
        for t in range(k * k):
            out[t * c + ch] = raw[((ch // 16) * k * k + t) * 16 + ch % 16]
    return bytes(out)


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


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

    names = {}                  # tensor name -> index
    tensors = []                # [h, w, c, exp]

    def tensor(name, h, w, c):
        names[name] = len(tensors)
        tensors.append([h, w, c, exps[name]])
        return names[name]

    tensor('input', 224, 224, 3)
    blob = bytearray()

    def put(data):
        while len(blob) % 16:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        return off

    nodes = []
    for n in g.tables(0):
        op, a = n.string(3), attrs(n)
        ins = [x for x in n.strings(0) if x]
        out = n.strings(1)[0]
        src = [names[x] for x in ins if x in names]
        h, w, c = tensors[src[0]][:3]
        ein = tensors[src[0]][3]
        node = dict(op=0, relu=0, k=0, stride=0, pad=0, flags=0, shift=0,
                    in0=src[0], in1=src[1] if len(src) > 1 else 0xffff,
                    w_off=0, b_off=0, aux=0)
        if op == 'Conv':
            W, B = inits[ins[1]], inits[ins[2]]
            k, cin = a['kernel_shape'][0], c
            s, p = a['strides'][0], a['pads'][0]
            assert a['kernel_shape'][0] == a['kernel_shape'][1]
            assert len(set(a['pads'])) == 1 and a['strides'][0] == a['strides'][1]
            dw = a['group'] != 1
            cout = c if dw else W['dims'][3]
            assert W['dims'][:3] == ([k, k, c] if dw else [k, k, cin])
            assert B['exp'][0] == ein + W['exp'][0]
            ho, wo = (h + 2 * p - k) // s + 1, (w + 2 * p - k) // s + 1
            nb = k * k * cin * (1 if dw else cout)
            wt = unpack_dw(W['raw'][:nb], k, c) if dw else \
                unpack_conv(W['raw'][:nb], k, cin, cout)
            node.update(op=OP_DW if dw else OP_CONV, k=k, stride=s, pad=p,
                        relu=1 if a['activation'] == 'Relu' else 0,
                        flags=F_REM if cout % 16 else 0,
                        shift=exps[out] - W['exp'][0] - ein,
                        w_off=put(wt), b_off=put(B['raw'][:4 * cout]))
            assert a['activation'] in ('Relu', 'Linear')
            assert node['shift'] > 0
            o = tensor(out, ho, wo, cout)
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
        elif op == 'Concat':
            t1 = tensors[src[1]]
            assert a['axis'] == 3 and len(src) == 2
            assert ein == t1[3] == exps[out] and (h, w) == tuple(t1[:2])
            node.update(op=OP_CAT)
            o = tensor(out, h, w, c + t1[2])
        else:
            sys.exit('unknown op ' + op)
        node['out'] = o
        nodes.append(node)

    outs = [names[x] for x in ('score0', 'bbox0', 'score1', 'bbox1',
                               'score2', 'bbox2')]

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
        return (tensors[t][0] * tensors[t][1] * tensors[t][2] + 15) & ~15

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
    f += struct.pack('<III', 1, len(tensors), len(nodes))
    f += struct.pack('<I6I', 0, *outs)
    f += struct.pack('<III', arena, len(blob), 0)
    assert len(f) == 56
    for i, (h, w, c, e) in enumerate(tensors):
        f += struct.pack('<HHHhI', h, w, c, e, offs[i])
    for n in nodes:
        f += struct.pack('<BBBBBBbBHHHHIIII', n['op'], n['relu'], n['k'],
                         n['stride'], n['pad'], n['flags'], n['shift'], 0,
                         n['in0'], n['in1'], n['out'], 0,
                         n['w_off'], n['b_off'], n['aux'], 0)
    while len(f) % 16:
        f.append(0)
    f += blob
    open(sys.argv[2], 'wb').write(f)
    print('%s: %d nodes, %d tensors, %d KB of weights, %d KB arena, %d bytes'
          % (sys.argv[2], len(nodes), len(tensors), len(blob) // 1024,
             arena // 1024, len(f)))


if __name__ == '__main__':
    main()
