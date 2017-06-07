from collections import namedtuple
import math

Vertex = namedtuple('Vertex', 'position uv')
BakedVertex = namedtuple('BakedVertex', 'position uv normal tangent')

def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])

def muls(a, b):
    return (a[0] * b, a[1] * b, a[2] * b)

def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def cross(a, b):
    x = a[1] * b[2] - a[2] * b[1]
    y = a[0] * b[2] - a[2] * b[0]
    z = a[0] * b[1] - a[1] * b[0]
    return (x, -y, z)

def normalize(v):
    factor = 1.0 / math.sqrt(dot(v, v))
    return muls(v, factor)

def pad_tangent(normal, tangent, bitangent):
    sign = dot(cross(normal, tangent), bitangent)
    return (tangent[0], tangent[1], tangent[2], 1.0 if sign > 0.0 else -1.0)

def compute_tangent_bitangent(v0, v1, v2):
    p0 = v0.position
    p1 = v1.position
    p2 = v2.position
    uv0 = v0.uv
    uv1 = v1.uv
    uv2 = v2.uv
    s0 = uv0[0]
    t0 = uv0[1]
    s1 = uv1[0] - s0
    t1 = uv1[1] - t0
    s2 = uv2[0] - s0
    t2 = uv2[1] - t0
    q1 = sub(p1, p0)
    q2 = sub(p2, p0)

    det = 1.0 / (s1 * t2 - s2 * t1)

    t = muls(sub(muls(q1, t2), muls(q2, t1)), det)
    b = muls(sub(muls(q2, s1), muls(q1, s2)), det)
    return (normalize(t), normalize(b))

class MeshBuilder():
    def __init__(self):
        self.vertices = []
        self.indices = []
        self.index_count = 0
        pass

    def add_triangle(self, v0, v1, v2):
        self.add_vertex(v0)
        self.add_vertex(v1)
        self.add_vertex(v2)

    def add_vertex(self, v):
        found = v in self.vertices
        if found:
            index = self.vertices.index(v)
            self.indices.append(index)
        else:
            self.indices.append(self.index_count)
            self.index_count += 1
            self.vertices.append(v)

    def add_quad(self, v0, v1, v2, v3):
        self.add_triangle(v0, v1, v2)
        self.add_triangle(v3, v2, v1)

    def build_normals(self):
        self.normals = [(0.0, 0.0, 0.0)] * len(self.vertices)
        self.tangents = [(0.0, 0.0, 0.0)] * len(self.vertices)
        bitangents = [(0.0, 0.0, 0.0)] * len(self.vertices)
        for i in range(0, len(self.indices) // 3):
            indices = self.indices[i * 3 : (i + 1) * 3]
            v0 = self.vertices[indices[0]].position
            v1 = self.vertices[indices[1]].position
            v2 = self.vertices[indices[2]].position
            normal = normalize(cross(sub(v1, v0), sub(v2, v0)))
            tangent, bitangent = compute_tangent_bitangent(self.vertices[indices[0]], self.vertices[indices[1]], self.vertices[indices[2]])
            for index in indices:
                self.normals[index] = add(self.normals[index], normal)
                self.tangents[index] = add(self.tangents[index], tangent)
                bitangents[index] = add(bitangents[index], bitangent)

        self.normals = [normalize(x) for x in self.normals]
        self.tangents = [normalize(x) for x in self.tangents]
        bitangents = [normalize(x) for x in bitangents]

        for i in range(0, len(self.tangents)):
            self.tangents[i] = pad_tangent(self.normals[i], self.tangents[i], bitangents[i])

