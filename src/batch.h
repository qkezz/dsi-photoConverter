/* CLI and SD album import. Included after the converter's private helpers.
 * pit.bin layout: https://problemkaputt.de/gbatek.htm#dsisdmmccamerafilespitbin
 * All PIT integers are little endian. CRC is BIOS CRC16 (polynomial 0xA001).
 */
#define PIT_ENTRIES 3000U
#define PIT_SIZE (0x18U + PIT_ENTRIES * 16U + 8U)
#define PHOTO_SLOTS 90000U
#define PATH_CAP 4096

static unsigned le16(const uint8_t *p) {
    return (unsigned)p[0] | ((unsigned)p[1]<<8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void put16(uint8_t *p, unsigned n) {
    p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);
}
static void put32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);
    p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);
}
static unsigned pit_crc(const Buffer *b) {
    unsigned crc=0,bit;
    size_t i;
    for(i=0;i<b->len;i++) {
        crc ^= (i==0x14 || i==0x15) ? 0 : b->data[i];
        for(bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0xA001U:0);
    }
    return crc;
}
static int pit_validate(const Buffer *b) {
    unsigned i;
    if(b->len!=PIT_SIZE || memcmp(b->data,"0TIP00_1",8)!=0 ||
       le16(b->data+8)!=PIT_ENTRIES || le16(b->data+10)!=1 ||
       le16(b->data+0x16)!=0x18 || le16(b->data+0xC)>=900 ||
       le16(b->data+0xE)>=100 || le16(b->data+0x10)>=900 ||
       le16(b->data+0x12)>=100) {
        fprintf(stderr,"Unsupported pit.bin: expected an original SD album (3000 entries).\n");
        return 0;
    }
    if(le16(b->data+0x14)!=pit_crc(b)) {
        fprintf(stderr,"Invalid pit.bin CRC16; index was not changed.\n");return 0;
    }
    for(i=0;i<PIT_ENTRIES;i++) {
        uint32_t flags=le32(b->data+0x24+i*16);
        if((flags&1) && (((flags>>1)&1023)>=900 || ((flags>>11)&127)>=100)) {
            fprintf(stderr,"Invalid folder/file number in pit.bin entry %u.\n",i);
            return 0;
        }
    }
    return 1;
}
static int path_join(char out[PATH_CAP], const char *dir, const char *name) {
    int n=snprintf(out,PATH_CAP,"%s/%s",dir,name);
    if(n<0 || n>=PATH_CAP) {fprintf(stderr,"Path too long.\n");return 0;}
    return 1;
}
/* Treat permission errors as occupied: never assume an unreadable file is absent. */
static int path_exists(const char *p) {
    struct stat st;
    if(stat(p,&st)==0)return 1;
    return errno!=ENOENT;
}
static int ensure_dir(const char *p) {
    struct stat st;
    int result;
    if(stat(p,&st)==0) {
#if defined(_WIN32)
        if(st.st_mode&_S_IFDIR)return 1;
#else
        if(S_ISDIR(st.st_mode))return 1;
#endif
        fprintf(stderr,"Not a directory: %s\n",p);return 0;
    }
#if defined(_WIN32)
    result=_mkdir(p);
#else
    result=mkdir(p,0777);
#endif
    if(result!=0) {perror(p);return 0;}
    return 1;
}
static int photo_path(char out[PATH_CAP], const char *root, unsigned slot, int indexed) {
    char name[80];
    if(indexed)snprintf(name,sizeof(name),"DCIM/%03uNIN02/HNI_%04u.JPG",100+slot/100,1+slot%100);
    else snprintf(name,sizeof(name),"HNI_%04u.JPG",slot+1);
    return path_join(out,root,name);
}
/* Build a fresh SD index only when no existing album index is being replaced. */
static int pit_time(time_t now, uint32_t *seconds) {
    struct tm *local=localtime(&now);
    unsigned year,days=0;
    uint64_t elapsed;
    if(!local || local->tm_year<100 || local->tm_year>236)return 0;
    for(year=2000;year<(unsigned)(local->tm_year+1900);year++)
        days+=365U+(year%4==0 && (year%100!=0 || year%400==0));
    elapsed=((uint64_t)days+(unsigned)local->tm_yday)*86400U+
        (unsigned)local->tm_hour*3600U+(unsigned)local->tm_min*60U+(unsigned)local->tm_sec;
    if(elapsed>UINT32_MAX)return 0;
    *seconds=(uint32_t)elapsed;
    return 1;
}
static int pit_scan(Buffer *pit, const char *root, unsigned type,
                    unsigned *used, uint32_t fallback) {
    unsigned folder,file,next=0;
    for(folder=0;folder<900;folder++) {
        char dir[PATH_CAP],name[40];
        struct stat st;
        snprintf(name,sizeof(name),"DCIM/%03uNIN02",100+folder);
        if(!path_join(dir,root,name))return 0;
        if(stat(dir,&st)!=0) {
            if(errno==ENOENT)continue;
            perror(dir);return 0;
        }
#if defined(_WIN32)
        if(!(st.st_mode&_S_IFDIR)) {
#else
        if(!S_ISDIR(st.st_mode)) {
#endif
            fprintf(stderr,"Not a directory: %s\n",dir);return 0;
        }
        for(file=0;file<100;file++) {
            char path[PATH_CAP];
            Buffer jpeg={0};
            uint32_t stamp=fallback;
            uint8_t *entry;
            unsigned slot=folder*100+file;
            if(!photo_path(path,root,slot,1))return 0;
            if(stat(path,&st)!=0) {
                if(errno==ENOENT)continue;
                perror(path);return 0;
            }
            if(*used>=PIT_ENTRIES) {
                fprintf(stderr,"Too many existing photos/frames for a 3000-entry SD index.\n");return 0;
            }
            if(!read_binary(path,&jpeg) || !verify_buffer(&jpeg)) {
                fprintf(stderr,"Cannot register existing DSi JPEG: %s\n",path);
                free(jpeg.data);return 0;
            }
            free(jpeg.data);
            (void)pit_time(st.st_mtime,&stamp);
            entry=pit->data+0x18+(*used)*16;
            put32(entry,stamp);
            put32(entry+12,1U|(folder<<1)|(file<<11)|(type<<20));
            (*used)++;
            next=(slot+1)%PHOTO_SLOTS;
        }
    }
    /* On wraparound, do not point the console at an already occupied slot. */
    for(;;) {
        unsigned i;
        for(i=0;i<*used;i++) {
            uint32_t flags=le32(pit->data+0x24+i*16);
            if(((flags>>20)&3)==type &&
               ((flags>>1)&1023)*100+((flags>>11)&127)==next)break;
        }
        if(i==*used)break;
        next=(next+1)%PHOTO_SLOTS;
    }
    put16(pit->data+(type?0x10:0xC),next/100);
    put16(pit->data+(type?0x12:0xE),next%100);
    return 1;
}
static int pit_create(Buffer *pit, const char *root, uint32_t stamp) {
    char frames[PATH_CAP];
    unsigned used=0;
    if(!append_zeros(pit,PIT_SIZE))return 0;
    memcpy(pit->data,"0TIP00_1",8);
    put16(pit->data+8,PIT_ENTRIES);
    put16(pit->data+10,1);
    put16(pit->data+0x16,0x18);
    if(!pit_scan(pit,root,0,&used,stamp) ||
       !path_join(frames,root,"private/ds/app/484E494A") ||
       !pit_scan(pit,frames,1,&used,stamp))return 0;
    put16(pit->data+0x14,pit_crc(pit));
    printf("New SD index prepared: %u existing photo/frame(s) registered.\n",used);
    return pit_validate(pit);
}
static int path_separator(char c) {
#if defined(_WIN32)
    return c=='/' || c=='\\';
#else
    return c=='/';
#endif
}
static int ensure_parent_dirs(const char *path) {
    char parent[PATH_CAP];
    size_t i,n=strlen(path),start=1;
    if(!n || n>=PATH_CAP) {fprintf(stderr,"Invalid index path.\n");return 0;}
    memcpy(parent,path,n+1);
#if defined(_WIN32)
    /* A UNC server/share prefix must already exist. */
    if(n>2 && path_separator(path[0]) && path_separator(path[1])) {
        start=2;
        while(start<n && !path_separator(path[start]))start++;
        if(start<n)start++;
        while(start<n && !path_separator(path[start]))start++;
        if(start<n)start++;
    }
#endif
    for(i=start;i<n;i++) {
        if(!path_separator(parent[i]) || path_separator(parent[i-1]))continue;
#if defined(_WIN32)
        if(i==2 && parent[1]==':')continue;
#endif
        parent[i]='\0';
        if(!ensure_dir(parent))return 0;
        parent[i]=path[i];
    }
    return 1;
}
static int numbered_path(char out[PATH_CAP], const char *path, const char *suffix) {
    unsigned i;
    for(i=0;i<10000;i++) {
        int n;
        if(i==0)n=snprintf(out,PATH_CAP,"%s.%s",path,suffix);
        else n=snprintf(out,PATH_CAP,"%s.%s.%u",path,suffix,i);
        if(n<0 || n>=PATH_CAP)return 0;
        if(!path_exists(out))return 1;
    }
    fprintf(stderr,"No free backup/temporary filename.\n");return 0;
}
static int replace_index(const char *path, const Buffer *original, Buffer *updated) {
    char backup[PATH_CAP], temp[PATH_CAP];
    Buffer current={0};
    int unchanged;
    /* Detect external edits before committing; concurrent writers are unsupported. */
    if(!read_binary(path,&current)) {free(current.data);return 0;}
    unchanged=current.len==original->len && memcmp(current.data,original->data,current.len)==0;
    free(current.data);
    if(!unchanged) {fprintf(stderr,"pit.bin changed during conversion; import cancelled.\n");return 0;}
    put16(updated->data+0x14,pit_crc(updated));
    if(!numbered_path(backup,path,"bak") || !write_new(backup,original))return 0;
    printf("Index backup: %s\n",backup);
    if(!numbered_path(temp,path,"tmp") || !write_new(temp,updated))return 0;
#if defined(_WIN32)
    if(!MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr,"Cannot replace pit.bin (Windows error %lu).\n",(unsigned long)GetLastError());
        remove(temp);return 0;
    }
#else
    if(rename(temp,path)!=0) {perror(path);remove(temp);return 0;}
#endif
    return 1;
}

static int batch_convert(const char **sources, unsigned count, const char *root,
                         const char *pit_path, int create_only) {
    Buffer original={0}, pit={0};
    uint8_t *occupied=NULL;
    char (*outputs)[PATH_CAP]=NULL;
    unsigned *entries=NULL, *slots=NULL;
    unsigned i,j,k,free_entries=0,cursor=0,made=0;
    int ok=0,new_index=0;
    uint32_t seconds=0;
    if(!count && !create_only)return 0;
    if(!pit_path && count>9999) {
        fprintf(stderr,"At most 9999 photos per unindexed output directory.\n");return 0;
    }
    if(pit_path) {
        if(count>PIT_ENTRIES) {
            fprintf(stderr,"At most 3000 images fit in an SD index.\n");goto done;
        }
        if(!pit_time(time(NULL),&seconds)) {
            fprintf(stderr,"Date is outside the PIT range.\n");goto done;
        }
        new_index=!path_exists(pit_path);
        if(create_only && !new_index) {
            fprintf(stderr,"Index already exists; --create-pit will not replace it: %s\n",pit_path);
            goto done;
        }
        if(new_index) {
            if(!pit_create(&pit,root,seconds))goto done;
        } else {
            if(!read_binary(pit_path,&original) || !pit_validate(&original) ||
               !append_bytes(&pit,original.data,original.len))goto done;
        }
        occupied=(uint8_t*)calloc(PHOTO_SLOTS,1);
        if(!occupied)goto done;
        for(i=0;i<PIT_ENTRIES;i++) {
            uint32_t f=le32(pit.data+0x24+i*16);
            if(!(f&1)) {free_entries++;continue;}
            /* Frames have a separate tree. Reserve every photo/unknown type. */
            if(((f>>20)&3)!=1)occupied[((f>>1)&1023)*100+((f>>11)&127)]=1;
        }
        if(count>free_entries) {fprintf(stderr,"Not enough free pit.bin entries (%u available).\n",free_entries);goto done;}
        cursor=le16(pit.data+0xC)*100+le16(pit.data+0xE);
    }
    if(count) {
        outputs=calloc(count,sizeof(*outputs));
        slots=calloc(count,sizeof(*slots));
        entries=calloc(count,sizeof(*entries));
        if(!outputs || !slots || !entries)goto done;
    }
    /* Plan the entire batch before creating photos or changing the index. */
    j=0;
    for(i=0;i<count;i++) {
        unsigned tried,limit=pit_path?PHOTO_SLOTS:9999U;
        for(tried=0;tried<limit;tried++) {
            unsigned slot=cursor;
            cursor=(cursor+1)%limit;
            if(pit_path && occupied[slot])continue;
            if(!photo_path(outputs[i],root,slot,pit_path!=NULL))goto done;
            if(path_exists(outputs[i]))continue;
            slots[i]=slot;
            if(pit_path)occupied[slot]=1;
            break;
        }
        if(tried==limit) {fprintf(stderr,"No free photo names.\n");goto done;}
        if(pit_path) {
            while(j<PIT_ENTRIES && (le32(pit.data+0x24+j*16)&1))j++;
            entries[i]=j++;
        }
        /* Avoid overwriting a supplied source even when it doesn't exist yet. */
        for(k=0;k<count;k++) {
            if(!strcmp(outputs[i],sources[k])) {
                fprintf(stderr,"Output equals an input: %s\n",outputs[i]);goto done;
            }
        }
    }
    if(!ensure_dir(root))goto done;
    if(pit_path) {
        char dcim[PATH_CAP];
        if(!path_join(dcim,root,"DCIM") || !ensure_dir(dcim))goto done;
    }
    for(i=0;i<count;i++) {
        if(pit_path) {
            char dir[PATH_CAP],relative[40];
            snprintf(relative,sizeof(relative),"DCIM/%03uNIN02",100+slots[i]/100);
            if(!path_join(dir,root,relative) || !ensure_dir(dir))goto done;
        }
        if(!convert_file(sources[i],outputs[i],1))goto done;
        made++;
        if(pit_path) {
            uint8_t *entry=pit.data+0x18+entries[i]*16;
            memset(entry,0,16);
            put32(entry,(uint32_t)seconds);
            put32(entry+12,1U|((slots[i]/100)<<1)|((slots[i]%100)<<11));
        }
    }
    if(pit_path) {
        unsigned next=cursor;
        for(i=0;i<PHOTO_SLOTS;i++) {
            char path[PATH_CAP];
            if(!photo_path(path,root,next,1))goto done;
            if(!occupied[next] && !path_exists(path))break;
            next=(next+1)%PHOTO_SLOTS;
        }
        put16(pit.data+0xC,next/100);put16(pit.data+0xE,next%100);
        if(new_index) {
            put16(pit.data+0x14,pit_crc(&pit));
            if(!ensure_parent_dirs(pit_path) || !write_new(pit_path,&pit))goto done;
            printf("Created %s: added %u new photo(s).\n",pit_path,count);
        } else {
            if(!replace_index(pit_path,&original,&pit))goto done;
            printf("Updated %s: added %u photo(s).\n",pit_path,count);
        }
    }
    printf("Completed: %u photo(s), output: %s\n",count,root);
    ok=1;
done:
    if(!ok) {
        for(i=0;i<made;i++)if(remove(outputs[i])!=0)perror(outputs[i]);
        fprintf(stderr,"Batch failed; newly created photos removed, index not committed.\n");
    }
    free(original.data);free(pit.data);free(occupied);free(outputs);free(entries);free(slots);
    return ok;
}
static int has_bin_suffix(const char *s) {
    size_t n=strlen(s);
    return n>=4 && s[n-4]=='.' && (s[n-3]=='b'||s[n-3]=='B') &&
        (s[n-2]=='i'||s[n-2]=='I') && (s[n-1]=='n'||s[n-1]=='N');
}
static void help(const char *name) {
    printf("dsi-photoConverter - Nintendo DSi Camera photo converter\n\n"
        "Usage:\n"
        "  %s input.png                         (automatic HNI_XXXX.JPG)\n"
        "  %s input.png output.JPG               (legacy explicit filename)\n"
        "  %s [-o output_dir] image1 image2 ...\n"
        "  %s --pit pit.bin [-o output_root] image1 image2 ...\n"
        "  %s --verify photo.JPG\n\n"
        "Create a new SD index, optionally importing images:\n"
        "  --create-pit pit.bin [-o output_root] [image1 image2 ...]\n"
        "Explicit output (allows replacement): --output output.JPG input.png\n"
        "A positional .bin file is also accepted as the PIT index.\n"
        "Default output directory: current directory. Use -o for explicit batch mode.\n"
        "With two bare paths, a missing second path is an explicit output filename.\n"
        "With PIT: creates DCIM/xxxNIN02/HNI_XXXX.JPG under output_root,\n"
        "updates an existing index with a .bak backup, or creates a missing index.\n"
        "New indexes scan signed JPEGs in standard photo/frame folders under output_root.\n"
        "Stickers cannot be recovered from JPEGs when creating an index.\n"
        "Copy the DCIM tree and updated pit.bin to their original SD locations.\n"
        "Existing SD pit.bin files must have valid CRC16; invalid files are never replaced.\n"
        "640x480 JPEG/PNG conversion; a valid signature alone does not ensure visibility.\n",
        name,name,name,name,name);
}
int main(int argc,char **argv) {
    const char *pit=NULL,*root=".";
    const char **sources;
    unsigned count=0;
    int i,options=0,end_options=0,result,create_only=0;
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))) {help(argv[0]);return 0;}
    if(argc==3 && !strcmp(argv[1],"--verify"))return verify_file(argv[2])?0:1;
    if(argc==4 && !strcmp(argv[1],"--output"))
        return convert_file(argv[3],argv[2],0)?0:1;
    if(argc<2) {help(argv[0]);return 2;}
    sources=calloc((size_t)argc,sizeof(*sources));
    if(!sources)return 1;
    for(i=1;i<argc;i++) {
        if(!end_options && !strcmp(argv[i],"--")) {end_options=1;options=1;continue;}
        if(!end_options && (!strcmp(argv[i],"--pit") || !strcmp(argv[i],"--create-pit") || !strcmp(argv[i],"-o") || !strcmp(argv[i],"--output-dir"))) {
            int is_create=!strcmp(argv[i],"--create-pit");
            int is_pit=!strcmp(argv[i],"--pit") || is_create;
            if(i+1==argc || (is_pit && pit))goto usage;
            if(is_pit) {pit=argv[++i];create_only=is_create;}else root=argv[++i];
            options=1;continue;
        }
        if(!end_options && argv[i][0]=='-')goto usage;
        if(has_bin_suffix(argv[i])) {
            if(pit)goto usage;
            pit=argv[i];options=1;
        } else sources[count++]=argv[i];
    }
    if(!count && !create_only)goto usage;
    /* Two existing images are a batch. A new second path retains old CLI syntax. */
    if(!options && count==2 && !path_exists(sources[1]))
        result=convert_file(sources[0],sources[1],1)?0:1;
    else result=batch_convert(sources,count,root,pit,create_only)?0:1;
    free(sources);return result;
usage:
    fprintf(stderr,"Invalid arguments. Use --help.\n");free(sources);return 2;
}
