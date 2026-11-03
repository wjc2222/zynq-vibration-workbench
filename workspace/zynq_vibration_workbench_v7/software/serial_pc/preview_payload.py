"""Display-only transport. Raw sample spacing and envelope intervals are explicit."""
import base64
import math
import re
import struct

from protocol import ProtocolError


def require(condition, message):
    if not condition:raise ProtocolError(message)


def integer(value, low, high):
    require(type(value) is int and low<=value<=high,"Invalid display integer")
    return value


def decimal(value):
    require(isinstance(value,str) and re.fullmatch(r"0|[1-9][0-9]{0,19}",value),"Invalid display index")
    result=int(value);require(result<2**64,"Display index overflow");return result


def binary(value, length):
    require(isinstance(value,str) and len(value)==4*((length+2)//3),"Display binary length mismatch")
    try:raw=base64.b64decode(value,validate=True)
    except Exception as exc:raise ProtocolError("Invalid display binary") from exc
    require(len(raw)==length and base64.b64encode(raw).decode()==value,"Noncanonical display binary")
    return raw


def decode_detail(detail):
    require(isinstance(detail,dict) and detail.get('schema')=='vibration.waveform.raw.v1',"Unsupported raw waveform schema")
    require(detail.get('encoding')=='s16le-interleaved-base64',"Unsupported raw waveform encoding")
    count=integer(detail.get('sample_count'),2,4096)
    rate=integer(detail.get('sample_rate_hz'),1,100000)
    first=decimal(detail.get('first_index'));origin=decimal(detail.get('time_origin_index'))
    require(first>=origin and first+count<=2**64,"Invalid raw waveform range")
    scale=integer(detail.get('adc_range_uv'),1,10000000)/1e6/32768.
    raw=binary(detail.get('raw_base64'),count*6)
    channels=[[],[],[]]
    for i,values in enumerate(struct.iter_unpack('<hhh',raw)):
        t=(first-origin+i)/rate
        for ch,value in enumerate(values):channels[ch].append((t,value*scale))
    return channels,(channels[0][0][0],channels[0][-1][0]),rate


def decode_envelope(envelope):
    require(isinstance(envelope,dict) and envelope.get('schema')=='vibration.waveform.envelope.v1' and envelope.get('unit')=='V',"Invalid envelope schema/unit")
    channels=envelope.get('channels')
    require(isinstance(channels,list) and len(channels)==3,"Invalid envelope channels")
    start,end=envelope.get('start_s'),envelope.get('end_s')
    require(all(type(v) in (int,float) and math.isfinite(v) for v in (start,end)) and start<end,"Invalid envelope extent")
    for channel in channels:
        require(isinstance(channel,list) and 0<len(channel)<=1024,"Invalid envelope count")
        previous=start
        for item in channel:
            require(isinstance(item,list) and len(item)==4 and all(type(v) in (int,float) and math.isfinite(v) for v in item),"Invalid envelope interval")
            a,b,lo,hi=item
            require(start<=a<=b<=end and a>=previous and lo<=hi,"Invalid envelope bounds")
            previous=b
    return channels,(start,end)


def decode_spectrum(spectrum):
    require(isinstance(spectrum,dict) and spectrum.get('available') is True,"Spectrum unavailable")
    channels=spectrum.get('channels');require(isinstance(channels,list) and len(channels)==3,"Invalid spectrum channels")
    output=[]
    for channel in channels:
        require(channel.get('encoding')=='f32le-base64',"Unsupported spectrum encoding")
        count=integer(channel.get('bin_count'),2,65537)
        fft=integer(channel.get('fft_size'),128,131072)
        step,first=channel.get('bin_width_hz'),channel.get('first_hz')
        require(type(step) in (int,float) and math.isfinite(step) and step>0 and first==0 and count<=fft//2+1,"Invalid spectrum frequency axis")
        raw=binary(channel.get('peak_amplitude_base64'),count*4)
        amplitudes=struct.unpack('<'+str(count)+'f',raw)
        require(all(math.isfinite(v) and v>=0 for v in amplitudes),"Invalid spectral amplitude")
        output.append([(i*step,v) for i,v in enumerate(amplitudes)])
    return output
