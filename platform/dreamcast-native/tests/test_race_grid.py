"""ROM-free authored grid extraction checks; run directly with python3."""
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from inspect_course import race_grid

p=bytearray(0x35000)
m=bytearray(0x86fac0+48*3)
struct.pack_into("<I",p,0x2f1f0,3)
for i in range(3):
    struct.pack_into("<5I",p,0x2f1f4+20*i,0,i,768,0,0x501220+4*i)
struct.pack_into("<4I",p,0x3980,3,3,3,3)
struct.pack_into("<4h",p,0x34be8,100,-1,0,2)
for i in range(3):
    struct.pack_into("<4I",m,0x86fac0+48*i+32,i,0xffffffff if i==2 else i,0x438e8000,0xab02)
roads=[dict(logical_count=196) for _ in range(4)]
grid=race_grid(p,m,roads)
assert grid[0]["player_id"]==0 and len(grid[0]["cars"])==3
assert grid[0]["cars"][0]==dict(id=0,route=0,authored_profile=0,profile=100,
    previous=0,initial_lap=-1,lateral=1408,speed_parameter_bits=0x438e8000,style=2)
assert grid[0]["cars"][1]["lateral"]==384
assert grid[0]["cars"][2]["profile"]==99
assert grid[1]["cars"][0]["profile"]==0xffffffff%196
for data,at,value in ((p,0x2f1f0,0),(p,0x2f1f8,256),(p,0x2f204,0),(p,0x2f20c,9),
                      (p,0x3980,0),(p,0x3980,45),(m,0x86fae0,8),(m,0x86fae8,0x7fc00000)):
    old=bytes(data[at:at+4]);struct.pack_into("<I",data,at,value)
    try: race_grid(p,m,roads)
    except ValueError: pass
    else: raise AssertionError("invalid grid source accepted")
    data[at:at+4]=old
print("PASS: authored grid identity, signed offsets, wrap, parity lanes and invalid sources")
