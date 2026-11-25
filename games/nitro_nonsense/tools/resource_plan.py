#!/usr/bin/env python3
"""Plan an additive EEPROM resource/library update; never program hardware.

The existing image and its complete, checksum-bound allocation inventory are
inputs. New payloads use only erased, unclaimed space. Existing entries retain
their indices, installed content, cards and saves. A new game may be appended.
The output image is an offline verification artifact, not a full-image writer.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

GAME=Path(__file__).resolve().parents[1]
SDK=GAME.parents[1]
EE_SIZE=0x20000
KBASE=0x3c40
DIRECTORY=0x20
DATA=0x800
MAX_ENTRIES=47
CARDS=DIRECTORY+MAX_ENTRIES*32
MAGIC=0x36324250
RESERVED=((0xff00,0x10100,'test scratch'),(0x1f000,0x1f800,'standard saves'))


class PlanError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def integer(value):
    return int(value,0) if isinstance(value,str) else int(value)


def checksum(data):
    value=0
    for byte in data:value=((value*33)^byte)&0xffffffff
    return value


def jump(offset):
    if not -(1<<20)<=offset<(1<<20) or offset&1:
        raise PlanError('invalid reset jump')
    value=offset&0x1fffff
    return ((value>>20)&1)<<31|((value>>1)&0x3ff)<<21|((value>>11)&1)<<20|((value>>12)&255)<<12|0x6f


def jump_target(word):
    if word&0xfff!=0x6f:raise PlanError('program does not begin with j <reset>')
    value=((word>>31)&1)<<20|((word>>12)&255)<<12|((word>>20)&1)<<11|((word>>21)&1023)<<1
    return value-(1<<21) if value&(1<<20) else value


def installed_image(raw):
    if not 4<=len(raw)<=KBASE:raise PlanError('program exceeds the original application limit')
    entry=jump_target(struct.unpack_from('<I',raw)[0])
    if not 4<=entry<len(raw):raise PlanError('reset entry is outside the resident image')
    return struct.pack('<I',jump(KBASE))+raw[4:],entry


def decompress(source,size):
    """Bounded decoder for the unchanged shipped PB26-LZ format."""
    bit_at=0
    def bits(count):
        nonlocal bit_at
        if count<0 or bit_at+count>len(source)*8:raise PlanError('truncated compressed stream')
        value=0
        for _ in range(count):
            value=(value<<1)|((source[bit_at//8]>>(7-(bit_at&7)))&1)
            bit_at+=1
        return value
    def gamma():
        zeros=0
        while not bits(1):
            zeros+=1
            if zeros>31:raise PlanError('compressed integer exceeds the kernel register width')
        return (1<<zeros)|bits(zeros)
    output=bytearray()
    while len(output)<size:
        if not bits(1):output.append(bits(8))
        else:
            count=gamma()+1
            offset=((gamma()-1)<<7|bits(7))+1
            if not 0<offset<=len(output) or count<2 or count>0xffffffff:
                raise PlanError('invalid compressed back reference')
            # The loader stops at its declared output size, not after an
            # untrusted match's entire claimed expansion.
            for _ in range(min(count,size-len(output))):output.append(output[-offset])
    return bytes(output)


def compressor():
    path=SDK/'firmware/tools/lz.py'
    spec=importlib.util.spec_from_file_location('pb26_resource_lz',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module.compress


def interval(start,length,owner,kind):
    if not 0<=start<EE_SIZE or not 0<length<=EE_SIZE-start:
        raise PlanError(f'{owner}: allocation exceeds physical EEPROM')
    end=start+length
    if kind!='reserved' and start<0x10000<end:raise PlanError(f'{owner}: allocation crosses an EEPROM bank')
    return {'start':start,'end':end,'length':length,'owner':owner,'kind':kind}


def add_interval(intervals,item):
    for previous in intervals:
        if item['start']>=previous['end'] or previous['start']>=item['end']:continue
        # Named save records are intentional subranges of the fixed save area.
        if item['kind']=='save' and previous['kind']=='reserved' and previous['owner']=='standard saves' and \
                previous['start']<=item['start']<item['end']<=previous['end']:
            continue
        raise PlanError(f"{item['owner']} overlaps {previous['owner']}")
    intervals.append(item)


def resolve_file(base,value):
    path=Path(value)
    return path if path.is_absolute() else base/path


def inventory_image(image,inventory,inventory_path):
    if len(image)!=EE_SIZE:raise PlanError('existing image must contain exactly 128 KiB')
    if inventory.get('format')!='nn-existing-library-v1' or inventory.get('completeFixedAllocations') is not True:
        raise PlanError('an explicitly complete fixed-allocation inventory is required')
    if inventory.get('imageSha256')!=digest(image):raise PlanError('existing image differs from its allocation inventory')
    if 'libraryManifest' in inventory:
        manifest_path=resolve_file(inventory_path.parent,inventory['libraryManifest'])
        manifest=json.loads(manifest_path.read_text());base=manifest_path.parent
        programs=manifest['programs']
    else:
        base=inventory_path.parent;programs=inventory['programs']
    magic,version,count,reserved=struct.unpack_from('<IBBH',image)
    if magic!=MAGIC or version!=1 or not 2<=count<=MAX_ENTRIES or reserved:
        raise PlanError('unsupported or corrupt existing library header')
    if len(programs)!=count-1:raise PlanError('inventory does not describe every existing library entry')
    intervals=[interval(0,DATA,'library metadata','metadata')]
    intervals += [interval(lo,hi-lo,owner,'reserved') for lo,hi,owner in RESERVED]
    entries=[];next_programs=[]
    for index in range(count):
        raw_entry=image[DIRECTORY+index*32:DIRECTORY+(index+1)*32]
        offset,csize,usize,entry,total,name,flags=struct.unpack('<IHHII15sB',raw_entry)
        try:name=name.split(b'\0',1)[0].decode('utf-8')
        except UnicodeError as error:raise PlanError('invalid existing program name') from error
        expected=programs[index-1] if index else {'name':'Menu'}
        if name!=expected['name'] or not name or not 4<=usize<=KBASE or not 4<=entry<usize:
            raise PlanError(f'entry {index}: identity or reset bounds mismatch')
        if index and flags!=(1 if 'tool' in expected.get('flags',[]) else 0):
            raise PlanError(f'{name}: inventory flags differ from the existing identity')
        item=interval(offset,csize,name+' program','program');add_interval(intervals,item)
        decoded=decompress(image[offset:offset+csize],usize)
        if checksum(decoded)!=total or struct.unpack_from('<I',decoded)[0]!=jump(KBASE):
            raise PlanError(f'{name}: existing installed program checksum or entry jump is invalid')
        if index:
            if 'file' in expected:
                raw=resolve_file(base,expected['file']).read_bytes()
                prepared,reset=installed_image(raw)
                if prepared!=decoded or reset!=entry:raise PlanError(f'{name}: original program content differs')
            elif expected.get('installedSha256')!=digest(decoded):
                raise PlanError(f'{name}: original installed identity differs')
        card_at,card_size=struct.unpack_from('<IH',image,CARDS+index*6)
        card=None
        if card_size not in (0,0xffff):
            card_item=interval(card_at,card_size,name+' card','card');add_interval(intervals,card_item)
            card=decompress(image[card_at:card_at+card_size],1024)
        description={'index':index,'name':name,'flags':flags,'entry':entry,
                     'installedSha256':digest(decoded),'directorySha256':digest(raw_entry),
                     'programStart':offset,'compressedBytes':csize,'installedBytes':usize,
                     'cardTableSha256':digest(image[CARDS+index*6:CARDS+(index+1)*6]),
                     'cardSha256':digest(card) if card is not None else None}
        entries.append(description)
        if not index:continue
        future={'name':name,'installedSha256':digest(decoded),'flags':expected.get('flags',[])}
        for kind in ('data','save'):
            if kind not in expected:continue
            allocation=expected[kind];at=integer(allocation['at'])
            content=resolve_file(base,allocation['file']).read_bytes() if 'file' in allocation else None
            size=len(content) if content is not None else integer(allocation['size'])
            owned=interval(at,size,name+' '+kind,kind);add_interval(intervals,owned)
            actual=image[at:at+size]
            if content is not None and actual!=content:raise PlanError(f'{name}: original fixed resource differs')
            if kind=='data' and content is None and allocation.get('sha256')!=digest(actual):
                raise PlanError(f'{name}: fixed resource identity mismatch')
            future[kind]={'at':at,'size':size}
            if kind=='data':future[kind]['sha256']=digest(actual)
        next_programs.append(future)
    additional=copy.deepcopy(inventory.get('additionalAllocations',[]))
    for allocation in additional:
        kind=allocation['kind']
        if kind not in ('data','save','reserved'):raise PlanError('unsupported fixed allocation kind')
        at=integer(allocation['at']);size=integer(allocation['size'])
        item=interval(at,size,allocation['owner'],kind);add_interval(intervals,item)
        actual=image[at:at+size]
        if kind=='data' and allocation.get('sha256')!=digest(actual):
            raise PlanError(f"{allocation['owner']}: fixed resource identity mismatch")
    protected=bytearray(EE_SIZE)
    for item in intervals:protected[item['start']:item['end']]=b'\x01'*item['length']
    unknown=0
    for address,byte in enumerate(image):
        if not protected[address] and byte!=0xff:protected[address]=1;unknown+=1
    return {'entries':entries,'intervals':intervals,'protected':protected,'programs':next_programs,
            'additionalAllocations':additional,'count':count,'unknownNonemptyBytes':unknown}


def free_ranges(image,protected):
    result=[]
    for low,high in ((DATA,0x10000),(0x10000,EE_SIZE)):
        start=None
        for address in range(low,high):
            available=not protected[address] and image[address]==0xff
            if available and start is None:start=address
            if not available and start is not None:result.append((start,address));start=None
        if start is not None:result.append((start,high))
    return result


def build_plan(image,inventory,inventory_path,region,address,*,resident=None,name='Nitro Nonsense',card=None,policy=None):
    old=inventory_image(image,inventory,inventory_path)
    if not region:raise PlanError('new resource must not be empty')
    owned=interval(address,len(region),name+' resources','data')
    mask=bytearray(old['protected'])
    if any(mask[address:owned['end']]) or image[address:owned['end']]!=b'\xff'*len(region):
        raise PlanError('requested resource allocation is occupied or contains existing values')
    if policy:
        if policy.get('format')!='nn-overlays-v2' or policy.get('testInstrumentationIdentityOverride') or policy.get('testInstrumentation'):
            raise PlanError('diagnostic or unsupported overlay policy cannot become a library resource')
        source=policy['sourceRegion']
        if (source['address'],source['length'],source['sha256'])!=(address,len(region),digest(region)):
            raise PlanError('resource does not match its overlay policy')
        if resident is not None and policy['residentSha256']!=digest(resident):
            raise PlanError('resident program does not match its overlay policy')
    output=bytearray(image);operations=[]
    def write(at,payload,phase,label):
        before=bytes(output[at:at+len(payload)])
        output[at:at+len(payload)]=payload
        operations.append({'phase':phase,'label':label,'address':at,'length':len(payload),
                           'beforeSha256':digest(before),'afterSha256':digest(payload),'data':payload})
    def allocate(payload,label):
        choices=[(lo,hi) for lo,hi in free_ranges(output,mask) if hi-lo>=len(payload)]
        if not choices:raise PlanError(f'no erased unclaimed space remains for {label}')
        lo,hi=min(choices,key=lambda item:item[1]-item[0])
        mask[lo:lo+len(payload)]=b'\x01'*len(payload)
        write(lo,payload,'payload',label);return lo
    mask[address:owned['end']]=b'\x01'*len(region)
    write(address,region,'payload',name+' resources')
    additional=old['additionalAllocations']+[{'kind':'data','owner':name+' resources',
                 'at':address,'size':len(region),'sha256':digest(region)}]
    programs=copy.deepcopy(old['programs']);new_entry=None
    if resident is not None:
        if card is not None and len(card)!=1024:raise PlanError('card must be 1024 column-major bytes')
        encoded=name.encode('utf-8')
        if not encoded or len(encoded)>15 or b'\0' in encoded:raise PlanError('new name must fit the unchanged 15-byte identity')
        if any(item['name'].casefold()==name.casefold() for item in old['entries']):
            raise PlanError('an existing game identity must not be replaced or silently duplicated')
        index=old['count']
        if index>=MAX_ENTRIES:raise PlanError('existing directory is full')
        entry_at=DIRECTORY+index*32;card_table=CARDS+index*6
        if image[entry_at:entry_at+32]!=b'\xff'*32 or image[card_table:card_table+6]!=b'\xff'*6:
            raise PlanError('unused directory/card slot contains unrecognized existing data')
        prepared,reset=installed_image(resident)
        compress=compressor();compressed=compress(prepared)
        if decompress(compressed,len(prepared))!=prepared:raise PlanError('new program compression mismatch')
        at=allocate(compressed,name+' program')
        card_reference=None
        if card is not None:
            packed_card=compress(card)
            if decompress(packed_card,1024)!=card:raise PlanError('new card compression mismatch')
            card_at=allocate(packed_card,name+' card')
            card_reference=struct.pack('<IH',card_at,len(packed_card))
        raw_entry=struct.pack('<IHHII15sB',at,len(compressed),len(prepared),reset,checksum(prepared),encoded,0)
        write(entry_at,raw_entry,'directory','new appended entry')
        if card_reference is not None:write(card_table,card_reference,'directory','new appended card reference')
        used=sum(item['length'] for item in old['intervals'] if item['kind'] in ('program','card','data'))
        used+=len(region)+len(compressed)+(len(packed_card) if card is not None else 0)
        free=sum(hi-lo for lo,hi in free_ranges(output,mask))
        write(8,struct.pack('<II',used,free),'advisory','library used/free counters')
        write(5,bytes([index+1]),'commit','library count, published last')
        new_entry={'index':index,'name':name,'installedSha256':digest(prepared),
                   'cardSha256':digest(card) if card is not None else None}
        programs.append({'name':name,'installedSha256':digest(prepared),'flags':[]})
    elif card is not None:raise PlanError('a card requires a newly appended program')
    next_inventory={'format':'nn-existing-library-v1','imageSha256':digest(output),
                    'completeFixedAllocations':True,'programs':programs,'additionalAllocations':additional}
    new=inventory_image(bytes(output),next_inventory,inventory_path)
    for index,entry in enumerate(old['entries']):
        if new['entries'][index]!=entry:raise PlanError('old identity, code or card is no longer recognized')
    allowed_metadata=set(range(8,16))|{5} if new_entry else set()
    if new_entry:
        allowed_metadata.update(range(entry_at,entry_at+32));allowed_metadata.update(range(card_table,card_table+6))
    for at,protected in enumerate(old['protected']):
        if protected and at not in allowed_metadata and output[at]!=image[at]:
            raise PlanError('a protected existing value changed')
    for item in old['intervals']:
        if item['kind']!='metadata' and output[item['start']:item['end']]!=image[item['start']:item['end']]:
            raise PlanError(f"existing allocation changed: {item['owner']}")
    report={'format':'nn-resource-plan-v1','sourceImageSha256':digest(image),
            'targetImageSha256':digest(output),'originalEntryCount':old['count'],
            'targetEntryCount':new['count'],'originalEntries':old['entries'],'newEntry':new_entry,
            'unknownNonemptyBytesProtected':old['unknownNonemptyBytes'],
            'preservedAllocations':[dict(item,sha256=digest(image[item['start']:item['end']])) for item in old['intervals']],
            'runtimeEepromWrites':'forbidden','hardwareDeploymentBlocked':True,
            'hardwareDeploymentReason':'Offline planner only. The legacy unjournaled count byte has no proven torn-write recovery; no device writer or atomicity claim is supplied.',
            'requiredLivePrecondition':'A hardware writer must verify the complete current image equals sourceImageSha256 immediately before writing; this planner does not perform that step.',
            'recovery':{'backupSha256':digest(image),'headerBytesHex':image[:32].hex(),
                        'scope':'Restore access to the original library only after a failed commit, before the new entry has been used. Verify old allocations and preserve current save bytes. Do not blindly write an old full image over newer records.'},
            'operations':operations}
    return bytes(output),next_inventory,report


def read_card(path):
    from PIL import Image
    picture=Image.open(path).convert('L')
    if picture.size!=(128,64) or not set(picture.getdata())<={0,255}:
        raise PlanError('card must be an inspected monochrome 128x64 image')
    pixels=bytes(picture.getdata())
    return bytes(sum((pixels[(page*8+bit)*128+x]>128)<<bit for bit in range(8)) for x in range(128) for page in range(8))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--inventory',type=Path,required=True)
    parser.add_argument('--region',type=Path,required=True)
    parser.add_argument('--address',type=lambda value:int(value,0),default=0xc000)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--resident',type=Path)
    parser.add_argument('--name',default='Nitro Nonsense')
    parser.add_argument('--card',type=Path)
    parser.add_argument('--policy',type=Path)
    args=parser.parse_args()
    try:
        if args.output.exists():raise PlanError('output must be a new directory; existing artifacts are never overwritten')
        image=args.image.read_bytes();inventory=json.loads(args.inventory.read_text())
        output,next_inventory,report=build_plan(image,inventory,args.inventory,args.region.read_bytes(),args.address,
            resident=args.resident.read_bytes() if args.resident else None,name=args.name,
            card=read_card(args.card) if args.card else None,policy=json.loads(args.policy.read_text()) if args.policy else None)
        args.output.mkdir(parents=True,exist_ok=False)
        (args.output/'original.eeprom').write_bytes(image)
        (args.output/'candidate.eeprom').write_bytes(output)
        (args.output/'original-header.bin').write_bytes(image[:32])
        (args.output/'inventory.input.json').write_text(json.dumps(inventory,indent=2)+'\n')
        (args.output/'inventory.next.json').write_text(json.dumps(next_inventory,indent=2)+'\n')
        for index,operation in enumerate(report['operations']):
            filename=f"{index+1:02}-{operation['phase']}.bin"
            (args.output/filename).write_bytes(operation.pop('data'));operation['file']=filename
        (args.output/'plan.json').write_text(json.dumps(report,indent=2)+'\n')
        if args.image.read_bytes()!=image:raise PlanError('original input changed during planning')
        print(f"PASS offline preservation: {report['originalEntryCount']} old entries retained; hardware deployment remains blocked")
    except (PlanError,OSError,KeyError,TypeError,ValueError,struct.error) as error:
        parser.exit(1,f'FAIL resource plan: {error}\n')


if __name__=='__main__':main()
