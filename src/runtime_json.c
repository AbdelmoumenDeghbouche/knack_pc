/* libSceJson2 (sce::Json, C++ ABI: this in rdi, class results through a hidden pointer in rdi).
 * WipEout builds result objects at the end of a race and serializes them, and parses objects back.
 * Every game-side object is one 8-byte handle to host memory; a null handle is the empty value:
 *   String = char *, Value = JValue *, Array = JArray *, Object = JObject *,
 *   Array::iterator = JValue ** (a Value *), Object::iterator = JMember ** (-> gives the pair, Value at +0x10).
 * Getters return references (the game reads *rax). Copies are deep. Host malloc, not the game's
 * MemAllocator. The per-value null-access callback is not called: missing members read as null. */
#include "runtime.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { J_NULL, J_BOOL, J_INT, J_UINT, J_REAL, J_STRING, J_ARRAY, J_OBJECT };
#define J_PARSE_ERROR ((int32_t)0x80920005)

typedef struct JValue JValue;
typedef struct { uint64_t count, capacity; JValue **items; } JArray;
typedef struct { char *key; uint64_t reserved; JValue *value; } JMember; /* matches the game's pair layout */
typedef struct { uint64_t count, capacity; JMember **items; } JObject;
struct JValue { int32_t type; union { bool b; int64_t i; uint64_t u; double r; char *s; JArray *a; JObject *o; }; };

static char *const no_string=NULL;
static JValue *const no_value=NULL;
static JArray *const no_array=NULL;
static JObject *const no_object=NULL;
static const int64_t no_number=0;
static const double no_real=0;
static uint64_t parses, serializes;

static char *dup_string(const char *s) {
    if (!s) return NULL;
    size_t n=strlen(s)+1;
    return memcpy(malloc(n),s,n);
}
static void free_value(JValue *v);
static JValue *copy_value(const JValue *v);
static void free_array(JArray *a) {
    if (!a) return;
    for (uint64_t i=0;i<a->count;++i) free_value(a->items[i]);
    free(a->items); free(a);
}
static void free_object(JObject *o) {
    if (!o) return;
    for (uint64_t i=0;i<o->count;++i) { free(o->items[i]->key); free_value(o->items[i]->value); free(o->items[i]); }
    free(o->items); free(o);
}
static void free_value(JValue *v) {
    if (!v) return;
    if (v->type==J_STRING) free(v->s);
    else if (v->type==J_ARRAY) free_array(v->a);
    else if (v->type==J_OBJECT) free_object(v->o);
    free(v);
}
static void array_push(JArray **slot, JValue *v) {
    if (!*slot) *slot=calloc(1,sizeof(JArray));
    JArray *a=*slot;
    if (a->count==a->capacity) { a->capacity=a->capacity ? 2*a->capacity : 8; a->items=realloc(a->items,a->capacity*sizeof(*a->items)); }
    a->items[a->count++]=v;
}
static JArray *copy_array(const JArray *a) {
    JArray *out=NULL;
    for (uint64_t i=0;a && i<a->count;++i) array_push(&out,copy_value(a->items[i]));
    return out;
}
static JMember **object_find(const JObject *o, const char *key) {
    for (uint64_t i=0;o && i<o->count;++i) if (!strcmp(o->items[i]->key,key ? key : "")) return &o->items[i];
    return NULL;
}
/* The member's Value slot, added as null when missing. Members are separate blocks, so a slot stays
 * valid while later members are added. */
static JValue **object_slot(JObject **slot, const char *key) {
    JMember **found=object_find(*slot,key);
    if (found) return &(*found)->value;
    if (!*slot) *slot=calloc(1,sizeof(JObject));
    JObject *o=*slot;
    if (o->count==o->capacity) { o->capacity=o->capacity ? 2*o->capacity : 8; o->items=realloc(o->items,o->capacity*sizeof(*o->items)); }
    JMember *m=calloc(1,sizeof(JMember));
    m->key=dup_string(key ? key : "");
    o->items[o->count++]=m;
    return &m->value;
}
static JObject *copy_object(const JObject *o) {
    JObject *out=NULL;
    for (uint64_t i=0;o && i<o->count;++i) *object_slot(&out,o->items[i]->key)=copy_value(o->items[i]->value);
    return out;
}
static JValue *new_value(int32_t type) { JValue *v=calloc(1,sizeof(JValue)); v->type=type; return v; }
static JValue *copy_value(const JValue *v) {
    if (!v) return NULL;
    JValue *out=new_value(v->type);
    if (v->type==J_STRING) out->s=dup_string(v->s);
    else if (v->type==J_ARRAY) out->a=copy_array(v->a);
    else if (v->type==J_OBJECT) out->o=copy_object(v->o);
    else out->u=v->u;
    return out;
}

/* ---- Parser: RFC 8259 text to a value tree. ---- */
typedef struct { const char *p, *end; } JText;
static void skip_space(JText *t) { while (t->p<t->end && (*t->p==' '||*t->p=='\t'||*t->p=='\n'||*t->p=='\r')) ++t->p; }
static int put_utf8(char *out, unsigned c) {
    if (c<0x80) { out[0]=(char)c; return 1; }
    if (c<0x800) { out[0]=(char)(0xc0|c>>6); out[1]=(char)(0x80|(c&0x3f)); return 2; }
    if (c<0x10000) { out[0]=(char)(0xe0|c>>12); out[1]=(char)(0x80|(c>>6&0x3f)); out[2]=(char)(0x80|(c&0x3f)); return 3; }
    out[0]=(char)(0xf0|c>>18); out[1]=(char)(0x80|(c>>12&0x3f)); out[2]=(char)(0x80|(c>>6&0x3f)); out[3]=(char)(0x80|(c&0x3f)); return 4;
}
static int hex4(JText *t, unsigned *c) {
    if (t->end-t->p<4) return 0;
    char digits[5]={0}; memcpy(digits,t->p,4);
    char *stop; *c=(unsigned)strtoul(digits,&stop,16);
    t->p+=4;
    return stop==digits+4;
}
/* A quoted string at t->p; NULL on bad text. */
static char *parse_string(JText *t) {
    if (t->p>=t->end || *t->p!='"') return NULL;
    ++t->p;
    char *out=malloc((size_t)(t->end-t->p)+1), *w=out; /* escapes never grow the text */
    while (t->p<t->end && *t->p!='"') {
        char c=*t->p++;
        if ((unsigned char)c<0x20) break;
        if (c!='\\') { *w++=c; continue; }
        if (t->p>=t->end) break;
        c=*t->p++;
        unsigned u;
        switch (c) {
        case '"': case '\\': case '/': *w++=c; break;
        case 'b': *w++='\b'; break;
        case 'f': *w++='\f'; break;
        case 'n': *w++='\n'; break;
        case 'r': *w++='\r'; break;
        case 't': *w++='\t'; break;
        case 'u':
            if (!hex4(t,&u)) goto fail;
            if (u>=0xd800 && u<0xdc00 && t->end-t->p>=6 && t->p[0]=='\\' && t->p[1]=='u') {
                unsigned low; t->p+=2;
                if (!hex4(t,&low) || low<0xdc00 || low>=0xe000) goto fail;
                u=0x10000+((u-0xd800)<<10)+(low-0xdc00);
            }
            w+=put_utf8(w,u);
            break;
        default: goto fail;
        }
    }
    if (t->p>=t->end || *t->p!='"') goto fail;
    ++t->p;
    *w=0;
    return out;
fail:
    free(out);
    return NULL;
}
static int parse_value(JText *t, JValue **out, int depth);
static int parse_members(JText *t, JValue *v, int depth) {
    char close=v->type==J_OBJECT ? '}' : ']';
    ++t->p;
    skip_space(t);
    if (t->p<t->end && *t->p==close) { ++t->p; return 1; }
    for (;;) {
        JValue *item=NULL;
        skip_space(t);
        if (v->type==J_OBJECT) {
            char *key=parse_string(t);
            if (!key) return 0;
            skip_space(t);
            if (t->p>=t->end || *t->p++!=':' || !parse_value(t,&item,depth+1)) { free(key); free_value(item); return 0; }
            JValue **slot=object_slot(&v->o,key);
            free(key);
            free_value(*slot);
            *slot=item;
        } else {
            if (!parse_value(t,&item,depth+1)) { free_value(item); return 0; }
            array_push(&v->a,item);
        }
        skip_space(t);
        if (t->p>=t->end) return 0;
        char c=*t->p++;
        if (c==close) return 1;
        if (c!=',') return 0;
    }
}
static int parse_value(JText *t, JValue **out, int depth) {
    *out=NULL;
    skip_space(t);
    if (t->p>=t->end || depth>256) return 0;
    char c=*t->p;
    if (c=='{' || c=='[') {
        *out=new_value(c=='{' ? J_OBJECT : J_ARRAY);
        return parse_members(t,*out,depth);
    }
    if (c=='"') {
        char *s=parse_string(t);
        if (!s) return 0;
        *out=new_value(J_STRING); (*out)->s=s;
        return 1;
    }
    static const struct { const char *word; int32_t type; bool b; } words[]={{"true",J_BOOL,true},{"false",J_BOOL,false},{"null",J_NULL,false}};
    for (size_t i=0;i<3;++i) {
        size_t n=strlen(words[i].word);
        if ((size_t)(t->end-t->p)>=n && !memcmp(t->p,words[i].word,n)) {
            t->p+=n;
            if (words[i].type==J_BOOL) { *out=new_value(J_BOOL); (*out)->b=words[i].b; }
            return 1;
        }
    }
    const char *start=t->p;
    if (t->p<t->end && *t->p=='-') ++t->p;
    if (t->p>=t->end || *t->p<'0' || *t->p>'9') return 0;
    bool real=false;
    while (t->p<t->end && *t->p && strchr("0123456789.eE+-",*t->p)) real|=strchr(".eE",*t->p++)!=NULL;
    char number[64];
    size_t n=(size_t)(t->p-start);
    if (n>=sizeof(number)) return 0;
    memcpy(number,start,n); number[n]=0;
    if (real) { *out=new_value(J_REAL); (*out)->r=strtod(number,NULL); }
    else if (*number=='-') { *out=new_value(J_INT); (*out)->i=strtoll(number,NULL,10); }
    else { *out=new_value(J_UINT); (*out)->u=strtoull(number,NULL,10); }
    return 1;
}

/* ---- Serializer: compact JSON text. ---- */
typedef struct { char *data; size_t size, capacity; } JBuffer;
static void put(JBuffer *b, const char *s, size_t n) {
    if (b->size+n+1>b->capacity) { b->capacity=2*(b->size+n+1); b->data=realloc(b->data,b->capacity); }
    memcpy(b->data+b->size,s,n); b->size+=n; b->data[b->size]=0;
}
static void put_string(JBuffer *b, const char *s) {
    put(b,"\"",1);
    for (; s && *s; ++s) {
        unsigned char c=(unsigned char)*s;
        char escape[8];
        if (c=='"' || c=='\\') { escape[0]='\\'; escape[1]=(char)c; put(b,escape,2); }
        else if (c<0x20) put(b,escape,(size_t)snprintf(escape,sizeof(escape),"\\u%04x",c));
        else put(b,s,1);
    }
    put(b,"\"",1);
}
static void put_value(JBuffer *b, const JValue *v) {
    char number[40];
    switch (v ? v->type : J_NULL) {
    case J_BOOL: if (v->b) put(b,"true",4); else put(b,"false",5); break;
    case J_INT: put(b,number,(size_t)snprintf(number,sizeof(number),"%lld",(long long)v->i)); break;
    case J_UINT: put(b,number,(size_t)snprintf(number,sizeof(number),"%llu",(unsigned long long)v->u)); break;
    case J_REAL: put(b,number,(size_t)snprintf(number,sizeof(number),"%.17g",v->r)); break;
    case J_STRING: put_string(b,v->s); break;
    case J_ARRAY:
        put(b,"[",1);
        for (uint64_t i=0;v->a && i<v->a->count;++i) { if (i) put(b,",",1); put_value(b,v->a->items[i]); }
        put(b,"]",1);
        break;
    case J_OBJECT:
        put(b,"{",1);
        for (uint64_t i=0;v->o && i<v->o->count;++i) {
            if (i) put(b,",",1);
            put_string(b,v->o->items[i]->key); put(b,":",1); put_value(b,v->o->items[i]->value);
        }
        put(b,"}",1);
        break;
    default: put(b,"null",4);
    }
}

/* ---- sce::Json entry points ---- */
static ABI void nothing(void *self) { (void)self; }
static ABI void clear_handle(void **self) { *self=NULL; }
static ABI int32_t initialize(void *self, const void *param) { (void)self; (void)param; return 0; }
static ABI void notify_error(void *self, int32_t error, uint64_t size, void *user) {
    (void)self; (void)user;
    printf("Runtime: Json2 allocator error %d (size %llu)\n",error,(unsigned long long)size);
}

static ABI void string_from_text(char **self, const char *text) { *self=dup_string(text); }
static ABI void string_copy(char **self, char *const *other) { *self=dup_string(*other); }
static ABI char **string_assign(char **self, char *const *other) {
    char *copy=dup_string(*other);
    free(*self); *self=copy;
    return self;
}
static ABI void string_free(char **self) { free(*self); *self=NULL; }
static ABI const char *string_c_str(char *const *self) { return *self ? *self : ""; }
static ABI uint64_t string_size(char *const *self) { return *self ? strlen(*self) : 0; }
static ABI bool string_empty(char *const *self) { return !*self || !**self; }
static ABI bool string_equals(char *const *self, const char *text) { return !strcmp(string_c_str(self),text ? text : ""); }

static ABI void value_from_uint(JValue **self, uint64_t u) { *self=new_value(J_UINT); (*self)->u=u; }
static ABI void value_from_string(JValue **self, char *const *s) { *self=new_value(J_STRING); (*self)->s=dup_string(*s); }
static ABI void value_from_array(JValue **self, JArray *const *a) { *self=new_value(J_ARRAY); (*self)->a=copy_array(*a); }
static ABI void value_from_object(JValue **self, JObject *const *o) { *self=new_value(J_OBJECT); (*self)->o=copy_object(*o); }
static ABI void value_free(JValue **self) { free_value(*self); *self=NULL; }
static ABI JValue **value_assign(JValue **self, JValue *const *other) {
    JValue *copy=copy_value(*other);
    free_value(*self); *self=copy;
    return self;
}
static ABI void value_copy(JValue **self, JValue *const *other) { *self=copy_value(*other); }
static int32_t value_replace(JValue **self, JValue *v) { free_value(*self); *self=v; return 0; }
static ABI int32_t value_set_object(JValue **self, JObject *const *o) {
    JValue *v=new_value(J_OBJECT); v->o=copy_object(*o);
    return value_replace(self,v);
}
/* Knack: set(bool), set(long), set(unsigned long), set(double), set(String), set(ValueType). */
static ABI int32_t value_set_bool(JValue **self, bool b) { JValue *v=new_value(J_BOOL); v->b=b; return value_replace(self,v); }
static ABI int32_t value_set_int(JValue **self, int64_t i) { JValue *v=new_value(J_INT); v->i=i; return value_replace(self,v); }
static ABI int32_t value_set_uint(JValue **self, uint64_t u) { JValue *v=new_value(J_UINT); v->u=u; return value_replace(self,v); }
static ABI int32_t value_set_real(JValue **self, double r) { JValue *v=new_value(J_REAL); v->r=r; return value_replace(self,v); }
static ABI int32_t value_set_string(JValue **self, char *const *s) { JValue *v=new_value(J_STRING); v->s=dup_string(*s); return value_replace(self,v); }
/* The type's empty value (null, false, 0, "", [] or {}). */
static ABI int32_t value_set_type(JValue **self, int32_t type) {
    if (type<J_NULL || type>J_OBJECT) { fprintf(stderr,"STOP: Json2 Value::set with unknown type %d\n",type); exit(21); }
    return value_replace(self,type==J_NULL ? NULL : new_value(type));
}
static ABI int32_t value_null_callback(JValue **self, void *callback, void *context) {
    (void)self; (void)callback; (void)context;
    return 0;
}
static ABI int32_t value_type(JValue *const *self) { return *self ? (*self)->type : J_NULL; }
static ABI const bool *value_boolean(JValue *const *self) { return *self && (*self)->type==J_BOOL ? &(*self)->b : (const bool *)&no_number; }
static ABI const int64_t *value_integer(JValue *const *self) {
    return *self && ((*self)->type==J_INT || (*self)->type==J_UINT) ? &(*self)->i : &no_number;
}
static ABI const double *value_real(JValue *const *self) { return *self && (*self)->type==J_REAL ? &(*self)->r : &no_real; }
static ABI char *const *value_string(JValue *const *self) { return *self && (*self)->type==J_STRING ? &(*self)->s : &no_string; }
static ABI JArray *const *value_array(JValue *const *self) { return *self && (*self)->type==J_ARRAY ? &(*self)->a : &no_array; }
static ABI JObject *const *value_object(JValue *const *self) { return *self && (*self)->type==J_OBJECT ? &(*self)->o : &no_object; }
static ABI JValue *const *value_member(JValue *const *self, const char *key) {
    JMember **found=*self && (*self)->type==J_OBJECT ? object_find((*self)->o,key) : NULL;
    return found ? &(*found)->value : &no_value;
}
static ABI JValue *const *value_member_string(JValue *const *self, char *const *key) { return value_member(self,*key); }
static ABI JValue *const *value_element(JValue *const *self, uint64_t index) {
    const JArray *a=*self && (*self)->type==J_ARRAY ? (*self)->a : NULL;
    return a && index<a->count ? &a->items[index] : &no_value;
}
static ABI uint64_t value_count(JValue *const *self) {
    if (!*self) return 0;
    if ((*self)->type==J_ARRAY) return (*self)->a ? (*self)->a->count : 0;
    if ((*self)->type==J_OBJECT) return (*self)->o ? (*self)->o->count : 0;
    return 0;
}
static ABI int32_t value_serialize(JValue *const *self, char **out) {
    JBuffer b={0};
    put_value(&b,*self);
    free(*out); *out=b.data;
    ++serializes;
    return 0;
}
static ABI int32_t parser_parse(JValue **out, const char *text, uint64_t size) {
    JText t={text,text+size};
    JValue *v=NULL;
    ++parses;
    if (!text || !parse_value(&t,&v,0)) { free_value(v); return J_PARSE_ERROR; }
    skip_space(&t);
    while (t.p<t.end && !*t.p) ++t.p; /* a counted size may include the terminator */
    if (t.p!=t.end) { free_value(v); return J_PARSE_ERROR; }
    free_value(*out); *out=v;
    return 0;
}

static ABI void array_copy(JArray **self, JArray *const *other) { *self=copy_array(*other); }
static ABI void array_free(JArray **self) { free_array(*self); *self=NULL; }
static ABI void array_push_back(JArray **self, JValue *const *v) { array_push(self,copy_value(*v)); }
static ABI JValue ***array_begin(JValue ***out, JArray *const *self) { *out=*self ? (*self)->items : NULL; return out; }
static ABI JValue ***array_end(JValue ***out, JArray *const *self) { *out=*self ? (*self)->items+(*self)->count : NULL; return out; }

static ABI void object_copy(JObject **self, JObject *const *other) { *self=copy_object(*other); }
static ABI void object_free(JObject **self) { free_object(*self); *self=NULL; }
static ABI JValue **object_index(JObject **self, char *const *key) { return object_slot(self,*key); }
static ABI JMember ***object_begin(JMember ***out, JObject *const *self) { *out=*self ? (*self)->items : NULL; return out; }
static ABI JMember ***object_end(JMember ***out, JObject *const *self) { *out=*self ? (*self)->items+(*self)->count : NULL; return out; }
static ABI bool object_empty(JObject *const *self) { return !*self || !(*self)->count; }
static ABI JMember ***object_find_key(JMember ***out, JObject *const *self, char *const *key) {
    JMember **found=object_find(*self,*key);
    if (!found) return object_end(out,self);
    *out=found;
    return out;
}

/* Both iterator kinds are one pointer into the item array. */
static ABI void iterator_copy(void **self, void *const *other) { *self=*other; }
static ABI bool iterator_equal(void *const *self, void *const *other) { return *self==*other; }
static ABI bool iterator_not_equal(void *const *self, void *const *other) { return *self!=*other; }
static ABI JValue ***array_iterator_next(JValue ***self) { ++*self; return self; }
static ABI JMember ***object_iterator_next(JMember ***self) { ++*self; return self; }
static ABI JValue **array_iterator_value(JValue **const *self) { return *self; }
static ABI JMember *object_iterator_pair(JMember **const *self) { return **self; }

static const RuntimeExport exports[]={
    {"_ZN3sce4Json11InitializerC1Ev",nothing}, {"_ZN3sce4Json11InitializerD1Ev",nothing},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",initialize},
    {"_ZN3sce4Json12MemAllocatorC2Ev",nothing}, {"_ZN3sce4Json12MemAllocatorD2Ev",nothing},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv",notify_error},
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm",parser_parse},
    {"_ZN3sce4Json6StringC1Ev",clear_handle}, {"_ZN3sce4Json6StringC1EPKc",string_from_text},
    {"_ZN3sce4Json6StringC1ERKS1_",string_copy}, {"_ZN3sce4Json6StringaSERKS1_",string_assign},
    {"_ZN3sce4Json6StringD1Ev",string_free}, {"_ZNK3sce4Json6String5c_strEv",string_c_str},
    {"_ZNK3sce4Json6String4sizeEv",string_size}, {"_ZNK3sce4Json6String5emptyEv",string_empty},
    {"_ZNK3sce4Json6StringeqEPKc",string_equals},
    {"_ZN3sce4Json5ValueC1Ev",clear_handle}, {"_ZN3sce4Json5ValueC1Em",value_from_uint},
    {"_ZN3sce4Json5ValueC1ERKNS0_6StringE",value_from_string}, {"_ZN3sce4Json5ValueC1ERKNS0_5ArrayE",value_from_array},
    {"_ZN3sce4Json5ValueC1ERKNS0_6ObjectE",value_from_object}, {"_ZN3sce4Json5ValueD1Ev",value_free},
    {"_ZN3sce4Json5ValueaSERKS1_",value_assign}, {"_ZN3sce4Json5Value5clearEv",value_free},
    {"_ZN3sce4Json5Value3setERKNS0_6ObjectE",value_set_object}, {"_ZN3sce4Json5ValueC1ERKS1_",value_copy},
    {"_ZN3sce4Json5Value3setEb",value_set_bool}, {"_ZN3sce4Json5Value3setEl",value_set_int},
    {"_ZN3sce4Json5Value3setEm",value_set_uint}, {"_ZN3sce4Json5Value3setEd",value_set_real},
    {"_ZN3sce4Json5Value3setERKNS0_6StringE",value_set_string}, {"_ZN3sce4Json5Value3setENS0_9ValueTypeE",value_set_type},
    {"_ZN3sce4Json5Value21setNullAccessCallBackEPFRKS1_NS0_9ValueTypeEPS2_PvES6_",value_null_callback},
    {"_ZN3sce4Json5Value9serializeERNS0_6StringE",value_serialize},
    {"_ZNK3sce4Json5Value7getTypeEv",value_type}, {"_ZNK3sce4Json5Value10getBooleanEv",value_boolean},
    {"_ZNK3sce4Json5Value10getIntegerEv",value_integer}, {"_ZNK3sce4Json5Value11getUIntegerEv",value_integer},
    {"_ZNK3sce4Json5Value7getRealEv",value_real}, {"_ZNK3sce4Json5Value9getStringEv",value_string},
    {"_ZNK3sce4Json5Value8getArrayEv",value_array}, {"_ZNK3sce4Json5Value9getObjectEv",value_object},
    {"_ZNK3sce4Json5Value8getValueERKNS0_6StringE",value_member_string},
    {"_ZNK3sce4Json5ValueixEPKc",value_member}, {"_ZNK3sce4Json5ValueixEm",value_element},
    {"_ZNK3sce4Json5Value5countEv",value_count},
    {"_ZN3sce4Json5ArrayC1Ev",clear_handle}, {"_ZN3sce4Json5ArrayC1ERKS1_",array_copy},
    {"_ZN3sce4Json5ArrayD1Ev",array_free}, {"_ZN3sce4Json5Array9push_backERKNS0_5ValueE",array_push_back},
    {"_ZNK3sce4Json5Array5beginEv",array_begin}, {"_ZNK3sce4Json5Array3endEv",array_end},
    {"_ZN3sce4Json5Array8iteratorD1Ev",nothing}, {"_ZN3sce4Json5Array8iteratorppEv",array_iterator_next},
    {"_ZNK3sce4Json5Array8iteratorneERKS2_",iterator_not_equal}, {"_ZNK3sce4Json5Array8iteratorptEv",array_iterator_value},
    {"_ZNK3sce4Json5Array8iteratordeEv",array_iterator_value},
    {"_ZNK3sce4Json6Object5beginEv",object_begin}, {"_ZNK3sce4Json6Object5emptyEv",object_empty},
    {"_ZN3sce4Json6Object8iteratorppEv",object_iterator_next},
    {"_ZN3sce4Json6ObjectC1Ev",clear_handle}, {"_ZN3sce4Json6ObjectC1ERKS1_",object_copy},
    {"_ZN3sce4Json6ObjectD1Ev",object_free}, {"_ZN3sce4Json6ObjectixERKNS0_6StringE",object_index},
    {"_ZNK3sce4Json6Object4findERKNS0_6StringE",object_find_key}, {"_ZNK3sce4Json6Object3endEv",object_end},
    {"_ZN3sce4Json6Object8iteratorC1ERKS2_",iterator_copy}, {"_ZN3sce4Json6Object8iteratorD1Ev",nothing},
    {"_ZNK3sce4Json6Object8iteratoreqERKS2_",iterator_equal}, {"_ZNK3sce4Json6Object8iteratorneERKS2_",iterator_not_equal},
    {"_ZNK3sce4Json6Object8iteratorptEv",object_iterator_pair},
};
uintptr_t runtime_json_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_json_report(void) {
    if (parses || serializes) printf("Runtime: Json2 parses=%llu, serializes=%llu\n",(unsigned long long)parses,(unsigned long long)serializes);
}
