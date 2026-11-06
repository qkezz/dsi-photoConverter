#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
/*
 * DSi JPEG converter: cross-platform C99 version.
 * Encodes 640x480 JPEG, Nintendo DSi-style EXIF + thumbnail,
 * and the AES-based signature used by DSi Camera.
 *
 * Depends on libjpeg, libpng >=1.6, OpenSSL >=1.1.1.
 * No Python dependency. See README.md for limitations.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <jpeglib.h>
#include <png.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define OUT_W 640
#define OUT_H 480
#define TH_W 160
#define TH_H 120
#define MAX_JPEG_BYTES 140000
#define SIGNATURE_NONCE_OFFSET 0x18A
#define SIGNATURE_TAG_OFFSET 0x196
#define SIGNATURE_END_OFFSET 0x1A6
#define MAX_INPUT_PIXELS 64000000U
#define MAX_FILE_BYTES (64U * 1024U * 1024U)

static const uint8_t dsi_key[16] = {
    0x70,0x88,0x52,0x06,0xDF,0xE5,0x01,0x6D,
    0x45,0xEA,0xC5,0x23,0x33,0xD6,0x44,0x6F
};

typedef struct {
    uint8_t *data;
    size_t len, cap;
} Buffer;

typedef struct { uint8_t *rgb; unsigned w,h; } Image;

static void image_free(Image *image) {
    free(image->rgb);
    image->rgb = NULL;
}

static int reserve(Buffer *b, size_t add) {
    size_t new_cap;
    uint8_t *new_data;
    if (add > SIZE_MAX - b->len) return 0;
    if (b->len + add <= b->cap) return 1;
    new_cap = b->cap ? b->cap : 1024;
    while (new_cap < b->len + add) {
        if (new_cap > SIZE_MAX / 2) { new_cap = b->len + add; break; }
        new_cap *= 2;
    }
    new_data = (uint8_t*)realloc(b->data, new_cap);
    if (!new_data) return 0;
    b->data = new_data; b->cap = new_cap;
    return 1;
}

static int append_bytes(Buffer *b, const void *ptr, size_t n) {
    if (!reserve(b, n)) return 0;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    return 1;
}
static int append_zeros(Buffer *b, size_t n) {
    if (!reserve(b, n)) return 0;
    memset(b->data + b->len, 0, n); b->len += n;
    return 1;
}
static int u16be(Buffer *b, unsigned v) {
    uint8_t tmp[2] = {(uint8_t)(v >> 8),(uint8_t)v};
    return append_bytes(b,tmp,2);
}
static int u32be(Buffer *b, uint32_t v) {
    uint8_t tmp[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    return append_bytes(b,tmp,4);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

typedef struct { unsigned tag, type; uint32_t count, value; } IfdEntry;
static int append_ifd(Buffer *b, const IfdEntry *e, unsigned n, uint32_t next) {
    unsigned i;
    if (!u16be(b,n)) return 0;
    for(i=0;i<n;i++) {
        if(!u16be(b,e[i].tag) || !u16be(b,e[i].type) ||
           !u32be(b,e[i].count) || !u32be(b,e[i].value)) return 0;
    }
    return u32be(b,next);
}

static int exif_dsi(const char *stamp, const uint8_t *thumb, size_t thumb_len,
                    Buffer *app1) {
    static const IfdEntry ifd0[] = {
        {0x010F,2,9,0x7A}, {0x0110,2,11,0x84}, {0x011A,5,1,0x90},
        {0x011B,5,1,0x98}, {0x0128,3,1,0x00020000}, {0x0131,2,5,0xA0},
        {0x0132,2,20,0xA6}, {0x0213,3,1,0x00020000}, {0x8769,4,1,0xBA}
    };
    static const IfdEntry sub[] = {
        {0x9000,7,4,0x30323230}, {0x9003,2,20,0x138},
        {0x9004,2,20,0x14C}, {0x9101,7,4,0x01020300},
        {0x927C,7,66,0x160}, {0xA000,7,4,0x30303130},
        {0xA001,3,1,0x00010000}, {0xA002,4,1,640},
        {0xA003,4,1,480}, {0xA005,4,1,0x1A2}
    };
    static const IfdEntry maker[] = {
        {0x1000,7,0x1C,0x17E}, {0x1001,7,8,0x19A}
    };
    static const IfdEntry interop[] = {
        {0x0001,2,4,0x52393800}, {0x0002,7,4,0x30313030},
        {0x1000,2,18,0x1CC}
    };
    IfdEntry thumb_ifd[] = {
        {0x0103,3,1,0x00060000}, {0x011A,5,1,0x22C},
        {0x011B,5,1,0x234}, {0x0128,3,1,0x00020000},
        {0x0201,4,1,0x23C}, {0x0202,4,1,0}
    };
    Buffer tiff = {0};
    int ok = 0;
    if (strlen(stamp)!=19 || thumb_len>UINT32_MAX) return 0;
    if (!append_bytes(&tiff,"MM\0\x2A",4) || !u32be(&tiff,8) ||
        !append_ifd(&tiff,ifd0,9,0x1DE) ||
        !append_bytes(&tiff,"Nintendo\0\0",10) ||
        !append_bytes(&tiff,"NintendoDS\0\0",12) ||
        !u32be(&tiff,72) || !u32be(&tiff,1) ||
        !u32be(&tiff,72) || !u32be(&tiff,1) ||
        !append_bytes(&tiff,"EINH\0\0",6) ||
        !append_bytes(&tiff,stamp,20) ||
        !append_ifd(&tiff,sub,10,0) ||
        !append_bytes(&tiff,stamp,20) || !append_bytes(&tiff,stamp,20) ||
        !append_ifd(&tiff,maker,2,0) ||
        !append_zeros(&tiff,28) || !append_zeros(&tiff,8) ||
        !append_ifd(&tiff,interop,3,0) ||
        !append_bytes(&tiff,"JPEG Exif Ver 2.2\0",18)) goto finish;
    thumb_ifd[5].value=(uint32_t)thumb_len;
    if (!append_ifd(&tiff,thumb_ifd,6,0) ||
        !u32be(&tiff,72) || !u32be(&tiff,1) ||
        !u32be(&tiff,72) || !u32be(&tiff,1)) goto finish;
    if (tiff.len != 0x23C) {
        fprintf(stderr,"Internal EXIF layout mismatch: %zu != 0x23C\n",tiff.len);
        goto finish;
    }
    if (!append_bytes(&tiff,thumb,thumb_len)) goto finish;
    if (tiff.len > 65533-6) {
        fprintf(stderr,"EXIF thumbnail too large for JPEG APP1\n"); goto finish;
    }
    if (!append_bytes(app1,"\xFF\xE1",2) || !u16be(app1,(unsigned)(8+tiff.len)) ||
        !append_bytes(app1,"Exif\0\0",6) ||
        !append_bytes(app1,tiff.data,tiff.len)) goto finish;
    ok=1;
finish:
    free(tiff.data);
    return ok;
}

/* libjpeg errors normally exit(); convert them into recoverable failures. */
struct jpeg_error { struct jpeg_error_mgr pub; jmp_buf jmp; char detail[JMSG_LENGTH_MAX]; };
static void jpeg_failed(j_common_ptr cinfo) {
    struct jpeg_error *error = (struct jpeg_error*)cinfo->err;
    (*cinfo->err->format_message)(cinfo,error->detail);
    longjmp(error->jmp,1);
}

static int read_jpeg(const char *path, Image *out) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error err;
    FILE *f=fopen(path,"rb");
    unsigned row_stride=0;
    volatile int created=0;
    volatile uint8_t *pixels=NULL;
    if(!f) { perror(path); return 0; }
    memset(&cinfo,0,sizeof(cinfo));
    cinfo.err=jpeg_std_error(&err.pub); err.pub.error_exit=jpeg_failed;
    if (setjmp(err.jmp)) {
        fprintf(stderr,"JPEG error: %s\n",err.detail);
        if(created) jpeg_destroy_decompress(&cinfo);
        free((void*)pixels); fclose(f); return 0;
    }
    jpeg_create_decompress(&cinfo); created=1;
    jpeg_stdio_src(&cinfo,f); jpeg_read_header(&cinfo,TRUE);
    cinfo.out_color_space=JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (!cinfo.output_width || !cinfo.output_height ||
        (uint64_t)cinfo.output_width*cinfo.output_height > MAX_INPUT_PIXELS ||
        cinfo.output_components != 3) {
        fprintf(stderr,"Unsupported or oversized JPEG image\n");
        jpeg_destroy_decompress(&cinfo); fclose(f); return 0;
    }
    row_stride=(unsigned)cinfo.output_width*3U;
    pixels=(uint8_t*)malloc((size_t)row_stride*cinfo.output_height);
    if(!pixels) {jpeg_destroy_decompress(&cinfo); fclose(f); return 0;}
    while(cinfo.output_scanline<cinfo.output_height) {
        JSAMPROW row[1];
        row[0]=(JSAMPROW)(pixels+(size_t)cinfo.output_scanline*row_stride);
        jpeg_read_scanlines(&cinfo,row,1);
    }
    out->rgb=(uint8_t*)pixels;
    out->w=cinfo.output_width; out->h=cinfo.output_height;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo); fclose(f);
    return 1;
}

static int read_png(const char *path, Image *out) {
    png_image png;
    uint8_t *rgba=NULL, *rgb=NULL;
    size_t n,i;
    memset(&png,0,sizeof(png)); png.version=PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png,path)) {
        fprintf(stderr,"PNG error: %s\n",png.message); return 0;
    }
    if (!png.width || !png.height || (uint64_t)png.width*png.height>MAX_INPUT_PIXELS) {
        fprintf(stderr,"Unsupported or oversized PNG image\n");
        png_image_free(&png); return 0;
    }
    n=(size_t)png.width*png.height;
    png.format=PNG_FORMAT_RGBA;
    rgba=(uint8_t*)malloc(n*4);
    rgb=(uint8_t*)malloc(n*3);
    if(!rgba || !rgb) {
        fprintf(stderr,"Out of memory\n");
        free(rgba); free(rgb); png_image_free(&png); return 0;
    }
    if(!png_image_finish_read(&png,NULL,rgba,0,NULL)) {
        fprintf(stderr,"PNG error: %s\n",png.message);
        free(rgba); free(rgb); png_image_free(&png); return 0;
    }
    /* Composite transparent pixels on white, as JPEG cannot store alpha. */
    for(i=0;i<n;i++) {
        unsigned a=rgba[i*4+3]; unsigned c;
        for(c=0;c<3;c++) rgb[i*3+c]=(uint8_t)((rgba[i*4+c]*a+255U*(255-a)+127U)/255U);
    }
    out->rgb=rgb; out->w=png.width; out->h=png.height;
    free(rgba); png_image_free(&png); return 1;
}

static int read_image(const char *path, Image *out) {
    FILE *f=fopen(path,"rb");
    unsigned char sig[8]; size_t n;
    if(!f) {perror(path);return 0;}
    n=fread(sig,1,8,f); fclose(f);
    if(n>=8 && !png_sig_cmp(sig,0,8)) return read_png(path,out);
    if(n>=2 && sig[0]==0xff && sig[1]==0xd8) return read_jpeg(path,out);
    fprintf(stderr,"Unsupported format (only PNG and JPEG)\n");
    return 0;
}

/* Pixel-center aligned bilinear resizing, to avoid additional dependencies. */
static uint8_t *resize_rgb(const uint8_t *src, unsigned sw, unsigned sh,
                           unsigned dw, unsigned dh) {
    uint8_t *dst=(uint8_t*)malloc((size_t)dw*dh*3);
    unsigned x,y,c;
    if(!dst) return NULL;
    for(y=0;y<dh;y++) {
        double sy=((double)y+0.5)*sh/dh-0.5;
        unsigned y0,y1; double fy;
        if (sy<=0) {y0=y1=0;fy=0;}
        else if(sy>=sh-1) {y0=y1=sh-1;fy=0;}
        else {y0=(unsigned)sy;y1=y0+1;fy=sy-y0;}
        for(x=0;x<dw;x++) {
            double sx=((double)x+0.5)*sw/dw-0.5;
            unsigned x0,x1; double fx;
            size_t idx=((size_t)y*dw+x)*3;
            if(sx<=0) {x0=x1=0;fx=0;}
            else if(sx>=sw-1) {x0=x1=sw-1;fx=0;}
            else {x0=(unsigned)sx;x1=x0+1;fx=sx-x0;}
            for(c=0;c<3;c++) {
                double a=src[((size_t)y0*sw+x0)*3+c];
                double b=src[((size_t)y0*sw+x1)*3+c];
                double cc=src[((size_t)y1*sw+x0)*3+c];
                double d=src[((size_t)y1*sw+x1)*3+c];
                double v=(1-fy)*((1-fx)*a+fx*b)+fy*((1-fx)*cc+fx*d);
                dst[idx+c]=(uint8_t)(v+0.5);
            }
        }
    }
    return dst;
}

static int write_jpeg_mem(const uint8_t *pixels, unsigned width, unsigned height,
                          int quality, int full444, Buffer *out) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error err;
    unsigned char *mem=NULL; unsigned long mem_len=0;
    volatile int created=0;
    memset(&cinfo,0,sizeof(cinfo));
    cinfo.err=jpeg_std_error(&err.pub); err.pub.error_exit=jpeg_failed;
    if (setjmp(err.jmp)) {
        fprintf(stderr,"JPEG encoding error: %s\n",err.detail);
        if(created) jpeg_destroy_compress(&cinfo);
        free(mem); return 0;
    }
    jpeg_create_compress(&cinfo); created=1;
    jpeg_mem_dest(&cinfo,&mem,&mem_len);
    cinfo.image_width=width; cinfo.image_height=height;
    cinfo.input_components=3; cinfo.in_color_space=JCS_RGB;
    jpeg_set_defaults(&cinfo);
    if(full444) {
        int i;
        for(i=0;i<3;i++) {
            cinfo.comp_info[i].h_samp_factor=1;
            cinfo.comp_info[i].v_samp_factor=1;
        }
    }
    jpeg_set_quality(&cinfo,quality,TRUE);
    jpeg_start_compress(&cinfo,TRUE);
    while(cinfo.next_scanline<cinfo.image_height) {
        JSAMPROW row[1];
        row[0]=(JSAMPROW)(pixels+(size_t)cinfo.next_scanline*width*3);
        jpeg_write_scanlines(&cinfo,row,1);
    }
    jpeg_finish_compress(&cinfo);
    if(mem_len>=2 && mem[0]==0xFF && mem[1]==0xD8) {
        if(append_bytes(out,mem,mem_len)) {
            jpeg_destroy_compress(&cinfo); free(mem); return 1;
        }
    }
    jpeg_destroy_compress(&cinfo); free(mem); return 0;
}

static void reverse16(const uint8_t *in, uint8_t *out) {
    unsigned i; for(i=0;i<16;i++) out[i]=in[15-i];
}
static void cmac_mul2_le(uint8_t a[16]) {
    unsigned i; unsigned carry=(a[15]>>7)&1;
    unsigned prev=0;
    for(i=0;i<16;i++) {
        unsigned next=(a[i]>>7)&1;
        a[i]=(uint8_t)((a[i]<<1)|prev);
        prev=next;
    }
    if(carry) a[0]^=0x87;
}

/* AES-ECB single-block helper (fixed key and context, with no padding). */
static int encrypt16(EVP_CIPHER_CTX *ctx, const uint8_t src[16],uint8_t dst[16]) {
    int n=0;
    return EVP_EncryptUpdate(ctx,dst,&n,src,16)==1 && n==16;
}

static int dsi_signature(const uint8_t *jpeg, size_t len,
                         const uint8_t nonce[12], uint8_t tag[16]) {
    EVP_CIPHER_CTX *ctx=NULL;
    uint8_t rev_key[16],zero[16]={0},k[16],blk[16],state[16],b0[16]={0};
    uint8_t s0[16],tmp[16],payload[16],state_rev[16];
    uint8_t *data=NULL;
    size_t padded,remain,pos,i;
    int ok=0;
    if(len<SIGNATURE_END_OFFSET || len>MAX_FILE_BYTES) return 0;
    padded=(len+15)&~(size_t)15;
    data=(uint8_t*)calloc(padded,1);
    ctx=EVP_CIPHER_CTX_new();
    if(!data || !ctx) goto finish;
    memcpy(data,jpeg,len);
    memset(data+SIGNATURE_NONCE_OFFSET,0,28);
    reverse16(dsi_key,rev_key);
    if(EVP_EncryptInit_ex(ctx,EVP_aes_128_ecb(),NULL,rev_key,NULL)!=1 ||
       EVP_CIPHER_CTX_set_padding(ctx,0)!=1 ||
       !encrypt16(ctx,zero,blk)) goto finish;
    reverse16(blk,k);
    cmac_mul2_le(k);
    remain=((len-1)&15)+1;
    if(remain==16) {
        for(i=0;i<16;i++) blk[i]=(uint8_t)(k[i]^data[len-16+i]);
    } else {
        cmac_mul2_le(k);
        memset(tmp,0,16);
        memcpy(tmp+16-remain,data+len-remain,remain);
        tmp[15-remain]=0x80;
        for(i=0;i<16;i++) blk[i]=(uint8_t)(k[i]^tmp[i]);
    }
    memcpy(data+len-remain,blk,16);
    b0[0]=0x7A;
    for(i=0;i<12;i++) b0[1+i]=nonce[11-i];
    if(!encrypt16(ctx,b0,state)) goto finish;
    for(pos=0;pos<padded;pos+=16) {
        for(i=0;i<16;i++) payload[i]=(uint8_t)(data[pos+15-i]^state[i]);
        if(!encrypt16(ctx,payload,state)) goto finish;
    }
    memset(b0,0,16);b0[0]=0x02;
    for(i=0;i<12;i++) b0[1+i]=nonce[11-i];
    if(!encrypt16(ctx,b0,tmp)) goto finish;
    reverse16(tmp,s0); reverse16(state,state_rev);
    for(i=0;i<16;i++) tag[i]=state_rev[i]^s0[i];
    ok=1;
finish:
    free(data); EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static int read_binary(const char *path, Buffer *b) {
    FILE *f=fopen(path,"rb"); long bytes;
    if(!f){perror(path);return 0;}
    if(fseek(f,0,SEEK_END)!=0 || (bytes=ftell(f))<0 ||
       (unsigned long)bytes>MAX_FILE_BYTES || fseek(f,0,SEEK_SET)!=0 ||
       !reserve(b,(size_t)bytes)) {fclose(f);return 0;}
    b->len=(size_t)bytes;
    if(fread(b->data,1,b->len,f)!=b->len) {fclose(f);return 0;}
    fclose(f);return 1;
}
static int write_binary(const char *path, const Buffer *b) {
    FILE *f=fopen(path,"wb");
    if(!f){perror(path);return 0;}
    if(fwrite(b->data,1,b->len,f)!=b->len) {
        fprintf(stderr,"Error writing %s\n",path);fclose(f);return 0;
    }
    if(fclose(f)!=0) {perror(path);return 0;}
    return 1;
}

/* Exclusive creation protects existing photos, backups, and temporary files. */
static int write_new(const char *path, const Buffer *b) {
    int fd, ok;
    FILE *f;
#if defined(_WIN32)
    fd=_open(path,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
#else
    fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
#endif
    if(fd<0) {perror(path);return 0;}
#if defined(_WIN32)
    f=_fdopen(fd,"wb");
#else
    f=fdopen(fd,"wb");
#endif
    if(!f) {
#if defined(_WIN32)
        _close(fd);
#else
        close(fd);
#endif
        remove(path);return 0;
    }
    ok=fwrite(b->data,1,b->len,f)==b->len;
    if(fclose(f)!=0)ok=0;
    if(!ok) {fprintf(stderr,"Error writing %s\n",path);remove(path);}
    return ok;
}

static int verify_buffer(const Buffer *jpeg) {
    uint8_t computed[16];
    const uint8_t *p=jpeg->data;
    if(jpeg->len<SIGNATURE_END_OFFSET || p[0]!=0xFF || p[1]!=0xD8 ||
       p[2]!=0xFF || p[3]!=0xE1 || memcmp(p+6,"Exif\0\0",6)!=0 ||
       memcmp(p+12,"MM\0\x2A",4)!=0 ||
       get_be32(p+16)!=8) {
        fprintf(stderr,"Not a DSi-style signed JPEG\n"); return 0;
    }
    if(!dsi_signature(p,jpeg->len,p+SIGNATURE_NONCE_OFFSET,computed)) return 0;
    if(memcmp(p+SIGNATURE_TAG_OFFSET,computed,16)!=0) {
        fprintf(stderr,"Invalid DSi signature\n"); return 0;
    }
    return 1;
}

static int verify_file(const char *path) {
    Buffer data={0}; int ok=read_binary(path,&data);
    if(ok)ok=verify_buffer(&data);
    if(ok)printf("OK: signature valid, %zu bytes: %s\n",data.len,path);
    free(data.data);return ok;
}

static int convert_file(const char *source, const char *output, int exclusive) {
    Image original={0}, img={0};
    Buffer thumb_jpeg={0}, app1={0}, raw_jpeg={0}, final={0};
    uint8_t *thumb_pixels=NULL;
    uint8_t nonce[12],tag[16];
    char stamp[32]; time_t now; struct tm clock_time;
    int q,ok=0;
    const int qualities[]={95,85,75,65,55,45,35,25};
    size_t qi;
    if(strcmp(source,output)==0) {
        fprintf(stderr,"Input and output paths must be different\n"); return 0;
    }
    if(!read_image(source,&original))goto finish;
    img.w=OUT_W;img.h=OUT_H;
    img.rgb=resize_rgb(original.rgb,original.w,original.h,OUT_W,OUT_H);
    if(!img.rgb)goto finish;
    thumb_pixels=resize_rgb(img.rgb,OUT_W,OUT_H,TH_W,TH_H);
    if(!thumb_pixels || !write_jpeg_mem(thumb_pixels,TH_W,TH_H,70,0,&thumb_jpeg))goto finish;
    now=time(NULL);
#if defined(_WIN32)
    if(localtime_s(&clock_time,&now)!=0)goto finish;
#else
    {
        struct tm *tm_ptr=localtime(&now);
        if(!tm_ptr)goto finish;
        clock_time=*tm_ptr;
    }
#endif
    if(strftime(stamp,sizeof(stamp),"%Y:%m:%d %H:%M:%S",&clock_time)!=19)goto finish;
    if(!exif_dsi(stamp,thumb_jpeg.data,thumb_jpeg.len,&app1))goto finish;
    if(RAND_bytes(nonce,12)!=1) {fprintf(stderr,"CSPRNG unavailable\n");goto finish;}
    for(qi=0;qi<sizeof(qualities)/sizeof(qualities[0]);qi++) {
        free(raw_jpeg.data);raw_jpeg=(Buffer){0};
        free(final.data);final=(Buffer){0};
        q=qualities[qi];
        if(!write_jpeg_mem(img.rgb,OUT_W,OUT_H,q,1,&raw_jpeg))goto finish;
        if(raw_jpeg.len<2 ||
           !append_bytes(&final,raw_jpeg.data,2) ||
           !append_bytes(&final,app1.data,app1.len) ||
           !append_bytes(&final,raw_jpeg.data+2,raw_jpeg.len-2))goto finish;
        if(final.len>MAX_FILE_BYTES || !dsi_signature(final.data,final.len,nonce,tag))goto finish;
        memcpy(final.data+SIGNATURE_NONCE_OFFSET,nonce,12);
        memcpy(final.data+SIGNATURE_TAG_OFFSET,tag,16);
        if(final.len<=MAX_JPEG_BYTES)break;
    }
    if(final.len>MAX_JPEG_BYTES) {
        fprintf(stderr,"Unable to reduce JPEG to <= %d bytes\n",MAX_JPEG_BYTES);
        goto finish;
    }
    if(!verify_buffer(&final)) {
        fprintf(stderr,"Internal signature verification failed\n");goto finish;
    }
    if(!(exclusive ? write_new(output,&final) : write_binary(output,&final)))goto finish;
    printf("Created %s (%zu bytes, 640x480, quality=%d, signed & verified)\n",
           output,final.len,q);
    ok=1;
finish:
    if(!ok)fprintf(stderr,"Conversion failed\n");
    image_free(&original);image_free(&img);
    free(thumb_pixels);free(thumb_jpeg.data);free(app1.data);
    free(raw_jpeg.data);free(final.data);
    return ok;
}

#include "batch.h"
