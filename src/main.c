#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE_EXTENDED 1
#include <curses.h>
#include <json-c/json.h>
#include <qrencode.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define MAX_CHATS 2048
#define MAX_MESSAGES 20000
#define EDIT_SIZE 1024
#define PASTE_START (KEY_MAX+1)
#define PASTE_END (KEY_MAX+2)
enum { BASE=1, PANEL, MUTED, ACCENT, SELECTED, OUTGOING, ERROR_STYLE, QR_STYLE, QR_INVERSE, PANEL_MUTED, PANEL_ACCENT, SELECTED_MUTED };
enum { CHAT_FOCUS, COMPOSE_FOCUS, SEARCH_FOCUS, NEW_FOCUS };
typedef struct {
    char id[160], name[256], preview[512];
    long time;
    int unread;
    wchar_t draft[EDIT_SIZE];
    char attachment[PATH_MAX];
} Chat;
typedef struct {
    char id[160], jid[160], sender[256], status[32];
    char *text;
    long time;
    bool mine;
    bool photo;
} Message;
static Chat chats[MAX_CHATS];
static Message messages[MAX_MESSAGES];
static int chat_count, message_count, selected=-1, focus=CHAT_FOCUS, scroll_offset, list_top;
static int filtered[MAX_CHATS], filtered_count, compose_cursor;
static wchar_t query[128], phone[32];
static bool running=true, online, demo, awaiting_send, help;
static char status[256]="Starting WhatsApp bridge", notice[512]="", theme_name[128]="terminal";
static char palette_path[PATH_MAX], bridge_path[PATH_MAX], state_dir[PATH_MAX];
static int bridge_in=-1, bridge_out=-1;
static pid_t child=-1;
static QRcode *qr;
static char *wire;
static size_t wire_length, wire_capacity;
static unsigned long theme_hash;
static volatile sig_atomic_t interrupted;
static unsigned send_token;
static int session_lock=-1;
static bool pasting;
static bool viewer, viewer_dirty, viewing_attachment, clipboard_pending;
static unsigned view_request;
static char view_id[160],view_jid[160],image_path[PATH_MAX],view_error[512];
static char *image_data;
static char image_format[32];
static int image_width,image_height,cell_width=8,cell_height=16;
static int visible_row_start;
static const char *graphics="external";
#define THUMB_CACHE 64
typedef struct {
    char id[160],jid[160],format[32],error[256];
    char *data;
    unsigned request;
    unsigned long used;
    int width,height,pixels_w,pixels_h;
    bool pending;
} Thumbnail;
static Thumbnail thumbnails[THUMB_CACHE];
static unsigned thumbnail_request;
static unsigned long thumbnail_clock;
static bool redraw=true,inline_images_drawn;
static int photo_rows,photo_columns;
static int composer_x,composer_y;
static void clear_inline_images(void);
static void draw_inline_images(void);
static Thumbnail *thumbnail_for(Message *m,bool request);
static void update_cell_size(void);
static void open_photo(bool attachment);
static void request_photo(void);
static void close_photo(void);
static void next_photo(int delta);
static void discard_photo(Chat *chat);
static void history_request(void);

static void copy(char *dst, size_t size, const char *src) { snprintf(dst,size,"%s",src?src:""); }
static bool join_path(char *dst,size_t size,const char *base,const char *suffix) {
    size_t a=strlen(base),b=strlen(suffix);
    if(a+b>=size) {fprintf(stderr,"Path is too long\n");return false;}
    memcpy(dst,base,a);memcpy(dst+a,suffix,b+1);return true;
}
static const char *str(json_object *o, const char *key) {
    json_object *v=NULL;
    return json_object_object_get_ex(o,key,&v) && json_object_is_type(v,json_type_string) ? json_object_get_string(v) : "";
}
static long num(json_object *o, const char *key) {
    json_object *v=NULL; return json_object_object_get_ex(o,key,&v)?json_object_get_int64(v):0;
}
static void note(const char *text) { copy(notice,sizeof notice,text);redraw=true; }
static void signal_stop(int sig) { (void)sig; interrupted=1; }
/* Draw text by terminal cells, stripping control characters from remote data. */
static void label(int y, int x, int width, const char *text, int style, bool bold) {
    if (y<0 || y>=LINES || x<0 || x>=COLS || width<=0) return;
    width=width<COLS-x?width:COLS-x;
    attrset(COLOR_PAIR(style) | (bold?A_BOLD:0));
    mbstate_t state={0}; int used=0;
    while (*text && used<width) {
        wchar_t wc; size_t n=mbrtowc(&wc,text,MB_CUR_MAX,&state);
        if (n==(size_t)-1 || n==(size_t)-2) { wc=L'?'; n=1; memset(&state,0,sizeof state); }
        if (!n) break;
        text+=n;
        if (wc<32 || (wc>=127 && wc<160)) wc=L' ';
        int cells=wcwidth(wc); if (cells<0) { wc=L'?'; cells=1; }
        if (used+cells>width) break;
        wchar_t out[2]={wc,0}; mvaddnwstr(y,x+used,out,1); used+=cells;
    }
}
static void wide_label(int y,int x,int width,const wchar_t *s,int style) {
    char buf[EDIT_SIZE*4+1]; mbstate_t st={0};
    const wchar_t *p=s; size_t n=wcsrtombs(buf,&p,sizeof(buf)-1,&st);
    if(n==(size_t)-1) buf[0]=0; else buf[n]=0;
    label(y,x,width,buf,style,false);
}
static void fill(int y,int x,int height,int width,int style) {
    attrset(COLOR_PAIR(style));
    for(int r=0;r<height && y+r<LINES;r++) if(y+r>=0) mvhline(y+r,x,' ',width);
}
static void rule(int y,int x,int width,int style) {
    attrset(COLOR_PAIR(style)); for(int i=0;i<width;i++) mvaddwstr(y,x+i,L"─");
}
static void time_text(long ts,char *out,size_t size,const char *format) {
    time_t t=(time_t)ts; struct tm tm;
    if (localtime_r(&t,&tm)) strftime(out,size,format,&tm); else copy(out,size,"");
}
static short theme_color(const char *value,short slot,short fallback) {
    unsigned r,g,b;
    if (sscanf(value,"#%2x%2x%2x",&r,&g,&b)!=3) return fallback;
    if(can_change_color() && COLORS>slot) {
        init_color(slot,(short)(r*1000/255),(short)(g*1000/255),(short)(b*1000/255)); return slot;
    }
    if(COLORS>=256) return (short)(16+36*((r*5+127)/255)+6*((g*5+127)/255)+(b*5+127)/255);
    return fallback;
}
static void load_theme(bool force) {
    const char *keys[]={"background","foreground","accent","lighter_background","selection","light_foreground","red"};
    char values[7][16]={{0}}, line[512]; unsigned long hash=5381;
    FILE *f=fopen(palette_path,"r");
    if(f) { while(fgets(line,sizeof line,f)) {
        for(const unsigned char *s=(unsigned char*)line;*s;s++) hash=((hash<<5)+hash)^*s;
        char key[64],value[16];
        if(sscanf(line," %63[^ =] = \"%15[^\"]\"",key,value)==2)
            for(int i=0;i<7;i++) if(!strcmp(key,keys[i])) copy(values[i],sizeof values[i],value);
    } fclose(f); }
    if(!force && hash==theme_hash) return;
    theme_hash=hash;
    short bg=theme_color(values[0],16,-1), fg=theme_color(values[1],17,-1);
    short accent=theme_color(values[2],18,COLOR_CYAN), panel=theme_color(values[3],19,bg);
    short selection=theme_color(values[4],20,panel), muted=theme_color(values[5],21,COLOR_WHITE);
    short red=theme_color(values[6],22,COLOR_RED);
    init_pair(BASE,fg,bg); init_pair(PANEL,fg,panel); init_pair(MUTED,muted,bg);
    init_pair(ACCENT,accent,bg); init_pair(SELECTED,accent,selection);
    init_pair(OUTGOING,fg,panel); init_pair(ERROR_STYLE,red,bg);
    init_pair(PANEL_MUTED,muted,panel);init_pair(PANEL_ACCENT,accent,panel);
    init_pair(SELECTED_MUTED,fg,selection);
    /* QR must use pure black and white independently of the current theme. */
    short black=theme_color("#000000",23,COLOR_BLACK), white=theme_color("#ffffff",24,COLOR_WHITE);
    init_pair(QR_STYLE,black,white); init_pair(QR_INVERSE,white,black);
    char name_path[PATH_MAX];
    const char *home=getenv("HOME"), *state=getenv("XDG_STATE_HOME");
    if(state) snprintf(name_path,sizeof name_path,"%s/omarchy/current/theme.name",state);
    else snprintf(name_path,sizeof name_path,"%s/.local/state/omarchy/current/theme.name",home);
    f=fopen(name_path,"r");
    if(f) { if(fgets(theme_name,sizeof theme_name,f)) theme_name[strcspn(theme_name,"\r\n")]=0; fclose(f); }
    bkgd(COLOR_PAIR(BASE)); clearok(stdscr,TRUE);
    redraw=true;
    if(viewer)viewer_dirty=true;
}
static Chat *find_chat(const char *id,bool create) {
    for(int i=0;i<chat_count;i++) if(!strcmp(chats[i].id,id)) return &chats[i];
    if(!create || chat_count==MAX_CHATS || !*id) return NULL;
    Chat *c=&chats[chat_count++]; copy(c->id,sizeof c->id,id); copy(c->name,sizeof c->name,id); return c;
}
static bool matches(const char *name) {
    wchar_t hay[512]; mbstate_t st={0}; const char *s=name;
    size_t n=mbsrtowcs(hay,&s,511,&st); if(n==(size_t)-1) return false;
    hay[n]=0; for(size_t i=0;i<n;i++) hay[i]=towlower(hay[i]);
    wchar_t needle[128]; wcscpy(needle,query);
    for(size_t i=0;needle[i];i++) needle[i]=towlower(needle[i]);
    return wcsstr(hay,needle)!=NULL;
}
static int chat_compare(const void *a,const void *b) {
    const Chat *ca=&chats[*(const int*)a],*cb=&chats[*(const int*)b];
    if(ca->time!=cb->time) return ca->time<cb->time?1:-1;
    return strcmp(ca->name,cb->name);
}
static void filter_chats(void) {
    filtered_count=0;
    for(int i=0;i<chat_count;i++) if(matches(chats[i].name)||matches(chats[i].id)) filtered[filtered_count++]=i;
    qsort(filtered,(size_t)filtered_count,sizeof filtered[0],chat_compare);
    if(selected<0 && filtered_count) {selected=filtered[0];history_request();}
}
static bool command(json_object *o) {
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    size_t len=strlen(s); char *line=malloc(len+2); if(!line) return false;
    memcpy(line,s,len); line[len++]='\n';
    size_t sent=0; while(sent<len) {
        ssize_t n=write(bridge_in,line+sent,len-sent);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { free(line); note("Bridge unavailable. Restart the app to reconnect."); return false; }
        sent+=(size_t)n;
    }
    free(line); return true;
}
static void history_request(void) {
    if(selected<0) return;
    scroll_offset=0; compose_cursor=(int)wcslen(chats[selected].draft);
    if(demo) return;
    json_object *o=json_object_new_object();
    json_object_object_add(o,"type",json_object_new_string("history"));
    json_object_object_add(o,"jid",json_object_new_string(chats[selected].id));
    command(o); json_object_put(o);
}
static void move_chat(int amount) {
    if(awaiting_send || !filtered_count) return;
    int index=0; for(int i=0;i<filtered_count;i++) if(filtered[i]==selected) index=i;
    index+=amount; if(index<0) index=0; if(index>=filtered_count) index=filtered_count-1;
    selected=filtered[index]; history_request();
}
static void add_message(json_object *o) {
    const char *id=str(o,"id"),*jid=str(o,"jid"); if(!*id||!*jid) return;
    Message *m=NULL;
    for(int i=0;i<message_count;i++) if(!strcmp(messages[i].id,id)&&!strcmp(messages[i].jid,jid)) {m=&messages[i];break;}
    if(!m) {
        if(message_count==MAX_MESSAGES) { free(messages[0].text); memmove(messages,messages+1,(MAX_MESSAGES-1)*sizeof *messages); message_count--; }
        m=&messages[message_count++]; memset(m,0,sizeof *m);
    }
    char *body=strdup(str(o,"text")); if(!body) return;
    free(m->text); m->text=body;
    copy(m->id,sizeof m->id,id); copy(m->jid,sizeof m->jid,jid);
    copy(m->sender,sizeof m->sender,str(o,"sender")); copy(m->status,sizeof m->status,str(o,"status"));
    m->mine=num(o,"mine")!=0; m->time=num(o,"time");
    m->photo=num(o,"photo")!=0 || !strncmp(m->text,"[Photo",6);
}
static void event(json_object *o) {
    redraw=true;
    const char *type=str(o,"type");
    if(!strcmp(type,"inline-image") || (!strcmp(type,"error")&&!strcmp(str(o,"scope"),"inline"))) {
        for(int i=0;i<THUMB_CACHE;i++)if(thumbnails[i].request==(unsigned)num(o,"request")&&thumbnails[i].pending) {
            Thumbnail *t=&thumbnails[i];t->pending=false;
            free(t->data);t->data=strdup(str(o,"data"));
            copy(t->format,sizeof t->format,str(o,"format"));
            copy(t->error,sizeof t->error,!strcmp(type,"error")?str(o,"text"):str(o,"error"));
            t->pixels_w=(int)num(o,"width");t->pixels_h=(int)num(o,"height");
            break;
        }
        return;
    }
    if(!strcmp(type,"status")) {
        bool was_online=online;
        copy(status,sizeof status,str(o,"text")); online=num(o,"online")!=0;
        if(online&&!was_online)for(int i=0;i<THUMB_CACHE;i++)if(*thumbnails[i].error) {
            thumbnails[i].error[0]=0;thumbnails[i].request=0;
        }
        if(online && qr) { QRcode_free(qr); qr=NULL; }
    } else if(!strcmp(type,"qr")) {
        if(qr) QRcode_free(qr);
        qr=QRcode_encodeString(str(o,"text"),0,QR_ECLEVEL_L,QR_MODE_8,1);
        copy(status,sizeof status,"Scan to sign in");
        if(!qr) note("Unable to encode pairing QR");
    } else if(!strcmp(type,"clear-qr")) {
        if(qr) {QRcode_free(qr);qr=NULL;}
    } else if(!strcmp(type,"chat")) {
        Chat *c=find_chat(str(o,"id"),true); if(!c) return;
        copy(c->name,sizeof c->name,str(o,"name")); copy(c->preview,sizeof c->preview,str(o,"preview"));
        c->time=num(o,"time"); c->unread=(int)num(o,"unread");
    } else if(!strcmp(type,"message")) add_message(o);
    else if(!strcmp(type,"history")) {
        json_object *arr=NULL; if(json_object_object_get_ex(o,"messages",&arr) && json_object_is_type(arr,json_type_array))
            for(size_t i=0;i<json_object_array_length(arr);i++) add_message(json_object_array_get_idx(arr,i));
    } else if(!strcmp(type,"sent") && (unsigned)num(o,"token")==send_token) {
        awaiting_send=false;
        if(selected>=0) {chats[selected].draft[0]=0;chats[selected].attachment[0]=0;}
        compose_cursor=0; note("Message sent");
    } else if(!strcmp(type,"error")) {
        note(str(o,"text"));
        clipboard_pending=false;
        if(viewer && (unsigned)num(o,"request")==view_request) {
            copy(view_error,sizeof view_error,str(o,"text"));viewer_dirty=true;
        }
        if((unsigned)num(o,"token")==send_token) awaiting_send=false;
    } else if(!strcmp(type,"attached")) {
        clipboard_pending=false;
        Chat *c=find_chat(str(o,"jid"),false);
        if(c) {
            if(*c->attachment)discard_photo(c);
            copy(c->attachment,sizeof c->attachment,str(o,"path"));
            note("Photo attached. Add a caption, then press Enter to send. Ctrl+X removes it.");
            if(selected>=0 && c==&chats[selected]) {focus=COMPOSE_FOCUS;open_photo(true);}
        }
    } else if(!strcmp(type,"image") && viewer && (unsigned)num(o,"request")==view_request) {
        copy(image_path,sizeof image_path,str(o,"path"));
        free(image_data);image_data=strdup(str(o,"data"));
        image_width=(int)num(o,"width");image_height=(int)num(o,"height");
        copy(image_format,sizeof image_format,str(o,"format"));
        copy(view_error,sizeof view_error,str(o,"error"));
        viewer_dirty=true;
    } else if(!strcmp(type,"notice")) note(str(o,"text"));
}
static void read_bridge(void) {
    char buf[16384]; ssize_t n;
    while((n=read(bridge_out,buf,sizeof buf))>0) {
        if(wire_length+(size_t)n+1>wire_capacity) {
            size_t needed=wire_length+(size_t)n+1,next=needed*2;
            if(needed>16*1024*1024) { note("Bridge response exceeds limit"); running=false; return; }
            if(next>16*1024*1024)next=16*1024*1024;
            char *p=realloc(wire,next); if(!p) {running=false;return;} wire=p; wire_capacity=next;
        }
        memcpy(wire+wire_length,buf,(size_t)n); wire_length+=(size_t)n; wire[wire_length]=0;
        char *start=wire,*end;
        while((end=strchr(start,'\n'))) {
            *end=0; json_object *o=json_tokener_parse(start);
            if(o) {event(o);json_object_put(o);} start=end+1;
        }
        size_t left=wire_length-(size_t)(start-wire); memmove(wire,start,left); wire_length=left; wire[left]=0;
    }
    if(n==0) {
        close(bridge_out); bridge_out=-1; online=false; awaiting_send=false;
        copy(status,sizeof status,"Bridge stopped");
        note("Connection bridge exited. See ~/.local/state/whatsapp-tui/bridge.log");
    }
}
typedef struct { wchar_t text[512]; int style, indent, span, message_index, image_row; bool meta; } Row;
static Row *rows; static int row_count;
#define MAX_ROWS 40000
static void add_row(const wchar_t *s,int style,bool meta) {
    if(row_count==MAX_ROWS) return;
    Row *r=&rows[row_count++]; wcsncpy(r->text,s,511); r->text[511]=0; r->style=style; r->meta=meta;r->indent=0;r->span=0;r->message_index=-1;r->image_row=-1;
}
static int message_compare(const void *a,const void *b) {
    const Message *ma=*(Message*const*)a,*mb=*(Message*const*)b;
    return ma->time<mb->time?-1:ma->time>mb->time?1:0;
}
static void build_rows(int width) {
    row_count=0; if(selected<0) return;
    photo_rows=(LINES-18)/2;if(photo_rows>10)photo_rows=10;if(photo_rows<2)photo_rows=2;
    photo_columns=width<48?width:48;
    Message *ordered[MAX_MESSAGES]; int count=0;
    for(int i=0;i<message_count;i++) if(!strcmp(messages[i].jid,chats[selected].id)) ordered[count++]=&messages[i];
    qsort(ordered,(size_t)count,sizeof ordered[0],message_compare);
    /* Keep a bounded view of the most recent messages. */
    int begin=count>400?count-400:0; char last_day[64]="";
    for(int i=begin;i<count;i++) {
        Message *m=ordered[i]; char date[64]; time_text(m->time,date,sizeof date,"%a, %d %b %Y");
        if(strcmp(date,last_day)) {
            wchar_t wide[128]; mbstowcs(wide,date,127); wide[127]=0;
            add_row(L"",BASE,true); add_row(wide,ACCENT,true); copy(last_day,sizeof last_day,date);
        }
        int first=row_count;
        char meta[512],stamp[32]; time_text(m->time,stamp,sizeof stamp,"%H:%M");
        snprintf(meta,sizeof meta,"%s  ·  %s%s%s",m->mine?"You":(*m->sender?m->sender:chats[selected].name),stamp,
                 m->mine?"  ·  ":"",m->mine?m->status:"");
        wchar_t meta_w[512]; mbstowcs(meta_w,meta,511); meta_w[511]=0;
        add_row(meta_w,m->mine?ACCENT:MUTED,true);
        const char *p=m->text;
        if(m->photo && strcmp(graphics,"external")) {
            for(int r=0;r<photo_rows;r++) {
                add_row(L"",m->mine?OUTGOING:BASE,false);
                if(row_count<MAX_ROWS)rows[row_count-1].image_row=r;
            }
            p=strchr(m->text,'\n');
            if(p)p++;else {p=strchr(m->text,']');p=p?p+1:"";while(*p==' ')p++;}
        }
        mbstate_t st={0}; wchar_t line[512]; int len=0,cells=0;
        while(*p) {
            wchar_t wc; size_t n=mbrtowc(&wc,p,MB_CUR_MAX,&st);
            if(n==(size_t)-1||n==(size_t)-2) {wc=L'?';n=1;memset(&st,0,sizeof st);} if(!n) break; p+=n;
            if(wc==L'\r') continue;
            if(wc==L'\n') {line[len]=0;add_row(line,m->mine?OUTGOING:BASE,false);len=cells=0;continue;}
            if(wc<32||(wc>=127&&wc<160)) wc=L' ';
            int w=wcwidth(wc); if(w<0) {wc=L'?';w=1;}
            if(cells+w>width || len>=510) {
                int split=len;
                for(int j=len-1;j>0;j--) if(line[j]==L' ') {split=j;break;}
                line[split]=0;add_row(line,m->mine?OUTGOING:BASE,false);
                int remainder=split<len?len-split-1:0;
                if(remainder) memmove(line,line+split+1,(size_t)remainder*sizeof *line);
                len=remainder;cells=0;
                for(int j=0;j<len;j++) {int cw=wcwidth(line[j]);if(cw>0)cells+=cw;}
            }
            line[len++]=wc;cells+=w;
        }
        line[len]=0;add_row(line,m->mine?OUTGOING:BASE,false);
        if(m->mine) {
            int span=m->photo&&strcmp(graphics,"external")?photo_columns:24;
            for(int r=first;r<row_count;r++) {
                int cells=0;for(int j=0;rows[r].text[j];j++) {int w=wcwidth(rows[r].text[j]);if(w>0)cells+=w;}
                if(cells>span)span=cells;
            }
            if(span>width)span=width;
            for(int r=first;r<row_count;r++) {rows[r].indent=width-span;rows[r].span=span;}
        }
        for(int r=first;r<row_count;r++)rows[r].message_index=(int)(m-messages);
        add_row(L"",BASE,true);
    }
}
static void discard_photo(Chat *chat) {
    if(!*chat->attachment)return;
    json_object *o=json_object_new_object();
    json_object_object_add(o,"type",json_object_new_string("discard"));
    json_object_object_add(o,"path",json_object_new_string(chat->attachment));
    command(o);json_object_put(o);chat->attachment[0]=0;
}
static void delete_terminal_image(void) {
    if(!strcmp(graphics,"kitty")) {printf("\033_Ga=d,d=I,i=31,q=2\033\\");fflush(stdout);}
}
static void close_photo(void) {
    delete_terminal_image();viewer=false;view_request++;
    free(image_data);image_data=NULL;image_path[0]=0;view_error[0]=0;
    clearok(stdscr,TRUE);
    redraw=true;
}
static void request_photo(void) {
    clear_inline_images();
    delete_terminal_image();free(image_data);image_data=NULL;
    image_path[0]=0;view_error[0]=0;image_width=image_height=0;
    viewer_dirty=true;
    clearok(stdscr,TRUE);
    struct winsize ws={0};
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0 && ws.ws_col && ws.ws_row && ws.ws_xpixel && ws.ws_ypixel) {
        cell_width=ws.ws_xpixel/ws.ws_col;cell_height=ws.ws_ypixel/ws.ws_row;
        if(cell_width<1)cell_width=8;
        if(cell_height<1)cell_height=16;
    }
    json_object *o=json_object_new_object();
    json_object_object_add(o,"type",json_object_new_string("view"));
    json_object_object_add(o,"jid",json_object_new_string(view_jid));
    json_object_object_add(o,"id",json_object_new_string(view_id));
    json_object_object_add(o,"request",json_object_new_int64(++view_request));
    json_object_object_add(o,"width",json_object_new_int((COLS-8)*cell_width));
    json_object_object_add(o,"height",json_object_new_int((LINES-13)*cell_height));
    json_object_object_add(o,"format",json_object_new_string(graphics));
    if(viewing_attachment && selected>=0)
        json_object_object_add(o,"attachment",json_object_new_string(chats[selected].attachment));
    if(!command(o))copy(view_error,sizeof view_error,"Image bridge is unavailable");
    json_object_put(o);
}
static void open_photo(bool attachment) {
    if(selected<0)return;
    viewing_attachment=attachment;
    copy(view_jid,sizeof view_jid,chats[selected].id);
    if(attachment) {
        if(!*chats[selected].attachment)return;
        view_id[0]=0;
    } else {
        Message *latest=NULL;
        for(int i=0;i<message_count;i++) if(messages[i].photo&&!strcmp(messages[i].jid,view_jid))
            if(!latest||messages[i].time>latest->time)latest=&messages[i];
        if(!latest) {note("No photos in this chat yet. Ctrl+V attaches a clipboard image.");return;}
        copy(view_id,sizeof view_id,latest->id);
    }
    viewer=true;request_photo();
}
static void next_photo(int delta) {
    if(viewing_attachment)return;
    Message *photos[MAX_MESSAGES];int count=0,index=0;
    for(int i=0;i<message_count;i++) if(messages[i].photo&&!strcmp(messages[i].jid,view_jid)) photos[count++]=&messages[i];
    if(!count)return;
    qsort(photos,(size_t)count,sizeof photos[0],message_compare);
    for(int i=0;i<count;i++)if(!strcmp(photos[i]->id,view_id))index=i;
    index+=delta;if(index<0)index=0;if(index>=count)index=count-1;
    if(strcmp(view_id,photos[index]->id)) {copy(view_id,sizeof view_id,photos[index]->id);request_photo();}
}
static unsigned char *decode_base64(const char *data,size_t *length) {
    size_t n=strlen(data);unsigned char *out=malloc(n/4*3+4);if(!out)return NULL;
    unsigned value=0;int bits=0;*length=0;
    for(size_t i=0;i<n && data[i]!='=';i++) {
        unsigned char c=(unsigned char)data[i];int digit;
        if(c>='A'&&c<='Z')digit=c-'A';else if(c>='a'&&c<='z')digit=c-'a'+26;
        else if(c>='0'&&c<='9')digit=c-'0'+52;else if(c=='+')digit=62;else if(c=='/')digit=63;
        else {free(out);return NULL;}
        value=(value<<6)|(unsigned)digit;bits+=6;
        if(bits>=8) {bits-=8;out[(*length)++]=(unsigned char)(value>>bits);}
    }
    return out;
}
static void update_cell_size(void) {
    struct winsize ws={0};
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0&&ws.ws_col&&ws.ws_row&&ws.ws_xpixel&&ws.ws_ypixel) {
        cell_width=ws.ws_xpixel/ws.ws_col;cell_height=ws.ws_ypixel/ws.ws_row;
        if(cell_width<1)cell_width=8;
        if(cell_height<1)cell_height=16;
    }
}
static Thumbnail *thumbnail_for(Message *m,bool request) {
    int width=photo_columns*cell_width,height=photo_rows*cell_height;
    Thumbnail *t=NULL;int victim=-1,pending=0;
    for(int i=0;i<THUMB_CACHE;i++) {
        Thumbnail *c=&thumbnails[i];
        if(c->pending)pending++;
        if(!strcmp(c->id,m->id)&&!strcmp(c->jid,m->jid)&&c->width==width&&c->height==height)t=c;
        if(!c->pending&&(victim<0||c->used<thumbnails[victim].used))victim=i;
    }
    if(!t&&request&&victim>=0) {
        t=&thumbnails[victim];free(t->data);memset(t,0,sizeof *t);
        copy(t->id,sizeof t->id,m->id);copy(t->jid,sizeof t->jid,m->jid);
        t->width=width;t->height=height;
    }
    if(!t)return NULL;
    t->used=++thumbnail_clock;
    if(request&&!t->request&&pending<3) {
        json_object *o=json_object_new_object();
        json_object_object_add(o,"type",json_object_new_string("thumbnail"));
        json_object_object_add(o,"jid",json_object_new_string(m->jid));
        json_object_object_add(o,"id",json_object_new_string(m->id));
        json_object_object_add(o,"request",json_object_new_int64(t->request=++thumbnail_request));
        json_object_object_add(o,"width",json_object_new_int(width));
        json_object_object_add(o,"height",json_object_new_int(height));
        json_object_object_add(o,"format",json_object_new_string(graphics));
        t->pending=command(o);
        if(!t->pending)copy(t->error,sizeof t->error,"Connection bridge unavailable");
        json_object_put(o);
    }
    return t;
}
static void clear_inline_images(void) {
    if(inline_images_drawn) {
        if(!strcmp(graphics,"kitty"))for(int i=0;i<THUMB_CACHE;i++)printf("\033_Ga=d,d=I,i=%d,q=2\033\\",1000+i);
        clearok(stdscr,TRUE);inline_images_drawn=false;
    }
}
static void draw_inline_images(void) {
    if(selected<0||viewer||qr||help||focus==NEW_FOCUS||COLS<64||LINES<22||!strcmp(graphics,"external"))return;
    int sidebar=COLS/3;if(sidebar>38)sidebar=38;if(sidebar<24)sidebar=24;
    int height=LINES-16;
    for(int i=0;i<height&&visible_row_start+i<row_count;i++) {
        Row *r=&rows[visible_row_start+i];
        if(r->image_row!=0||r->message_index<0||i+photo_rows>height)continue;
        Thumbnail *t=thumbnail_for(&messages[r->message_index],false);
        if(!t||!t->data||!*t->data||*t->error||t->pixels_w<=0||t->pixels_h<=0)continue;
        int cols=(t->pixels_w+cell_width-1)/cell_width,h=(t->pixels_h+cell_height-1)/cell_height;
        if(cols>photo_columns)cols=photo_columns;
        if(h>photo_rows)h=photo_rows;
        int x=sidebar+5+r->indent+(photo_columns-cols)/2;
        int y=9+i+(photo_rows-h)/2;
        printf("\0337\033[%d;%dH",y+1,x+1);
        if(!strcmp(t->format,"kitty")) {
            size_t length=strlen(t->data);
            for(size_t pos=0;pos<length;) {
                size_t chunk=length-pos;if(chunk>4096)chunk=4096;
                if(!pos)printf("\033_Ga=T,f=100,t=d,i=%d,q=2,C=1,c=%d,r=%d,m=%d;",1000+(int)(t-thumbnails),cols,h,pos+chunk<length);
                else printf("\033_Gm=%d;",pos+chunk<length);
                fwrite(t->data+pos,1,chunk,stdout);printf("\033\\");pos+=chunk;
            }
            inline_images_drawn=true;
        } else if(!strcmp(t->format,"sixel")) {
            size_t length;unsigned char *data=decode_base64(t->data,&length);
            if(data) {fwrite(data,1,length,stdout);free(data);inline_images_drawn=true;}
        }
        printf("\0338");
    }
    fflush(stdout);
}
static void output_photo(void) {
    if(!image_data||!*image_data||image_width<=0||image_height<=0)return;
    int cols=(image_width+cell_width-1)/cell_width,rows_needed=(image_height+cell_height-1)/cell_height;
    if(cols>COLS-8)cols=COLS-8;
    if(rows_needed>LINES-13)rows_needed=LINES-13;
    int x=(COLS-cols)/2,y=8+(LINES-13-rows_needed)/2;
    printf("\0337\033[%d;%dH",y+1,x+1);
    if(!strcmp(image_format,"kitty")) {
        size_t length=strlen(image_data);
        for(size_t pos=0;pos<length;) {
            size_t chunk=length-pos;if(chunk>4096)chunk=4096;
            int more=pos+chunk<length;
            if(!pos)printf("\033_Ga=T,f=100,t=d,i=31,q=2,C=1,c=%d,r=%d,m=%d;",cols,rows_needed,more);
            else printf("\033_Gm=%d;",more);
            fwrite(image_data+pos,1,chunk,stdout);printf("\033\\");pos+=chunk;
        }
    } else if(!strcmp(image_format,"sixel")) {
        size_t length=0;unsigned char *data=decode_base64(image_data,&length);
        if(data) {fwrite(data,1,length,stdout);free(data);}
    }
    printf("\0338");fflush(stdout);
}
static void draw_photo(void);
static void header(void) {
    fill(0,0,3,COLS,PANEL);
    label(1,2,22,"◉  WHATSAPP",PANEL_ACCENT,true);
    char right[512]; snprintf(right,sizeof right,"%s  /  %s",theme_name,demo?"Preview":status);
    int width=(int)strlen(right); if(width>COLS-26) width=COLS-26;
    label(1,COLS-width-2,width,right,online?PANEL_ACCENT:PANEL_MUTED,false);
    rule(2,0,COLS,MUTED);
}
static void draw_photo(void) {
    delete_terminal_image();
    erase();curs_set(0);header();
    if(COLS<64||LINES<22) {label(5,2,COLS-4,"Resize terminal to at least 64 × 22",ACCENT,true);refresh();viewer_dirty=false;return;}
    char title[512];snprintf(title,sizeof title,"%s  ·  %s",viewing_attachment?"PHOTO ATTACHED":"PHOTO",selected>=0?chats[selected].name:"");
    label(4,4,COLS-8,title,ACCENT,true);
    if(viewing_attachment && selected>=0) {
        if(*chats[selected].draft)wide_label(6,4,COLS-8,chats[selected].draft,MUTED);
        else label(6,4,COLS-8,"Return to the composer to add a caption.",MUTED,false);
    } else {
        for(int i=0;i<message_count;i++) if(!strcmp(messages[i].jid,view_jid)&&!strcmp(messages[i].id,view_id)) {
            const char *caption=strchr(messages[i].text,'\n');
            char stamp[64];time_text(messages[i].time,stamp,sizeof stamp,"%d %b %Y · %H:%M");
            label(5,4,COLS-8,stamp,MUTED,false);
            if(caption)label(6,4,COLS-8,caption+1,BASE,false);
            break;
        }
    }
    if(*view_error)label(10,4,COLS-8,view_error,ERROR_STYLE,false);
    else if(!*image_path)label(10,4,COLS-8,"Loading photo…",MUTED,false);
    else if(!strcmp(image_format,"external")) {
        label(10,4,COLS-8,"Press O to view this photo in your image viewer.",BASE,false);
        label(12,4,COLS-8,"Foot, Kitty, and Ghostty can display photos inside the terminal.",MUTED,false);
    }
    fill(LINES-2,0,2,COLS,PANEL);
    label(LINES-2,2,COLS-4,viewing_attachment?"Photo staged · it will send only when you press Enter in the composer.":notice,PANEL_MUTED,false);
    label(LINES-1,2,COLS-4,viewing_attachment?"Esc / Enter compose   Ctrl+X remove photo   O open externally   Ctrl+Q quit":"← / → previous / next   Esc close   O open externally   Ctrl+Q quit",PANEL_MUTED,false);
    refresh();output_photo();viewer_dirty=false;
}
static void draw_qr(void) {
    int size=qr->width+8,height=(size+1)/2;
    if(COLS<size+6 || LINES<height+10) {
        label(5,3,COLS-6,"Pair your phone",ACCENT,true);
        char msg[256]; snprintf(msg,sizeof msg,"Resize terminal to at least %d × %d to scan the QR code.",size+6,height+10);
        label(7,3,COLS-6,msg,BASE,false); return;
    }
    int x=(COLS-size)/2,y=5;
    label(3,x,size,"Link WhatsApp to your terminal",ACCENT,true);
    for(int row=0;row<height;row++) for(int col=0;col<size;col++) {
        int a=0,b=0,xx=col-4,yy=row*2-4;
        if(xx>=0 && xx<qr->width && yy>=0 && yy<qr->width) a=qr->data[yy*qr->width+xx]&1;
        if(xx>=0 && xx<qr->width && yy+1>=0 && yy+1<qr->width) b=qr->data[(yy+1)*qr->width+xx]&1;
        attrset(COLOR_PAIR(QR_STYLE));
        mvaddwstr(y+row,x+col,a?(b?L"█":L"▀"):(b?L"▄":L" "));
    }
    label(y+height+1,x,size,"Phone → WhatsApp → Linked devices → Link a device",BASE,false);
    label(y+height+2,x,size,"QR refreshes automatically. Keep your phone connected.",MUTED,false);
}
static void draw_editor(int x,int width) {
    int y=LINES-6;
    rule(y,x,width,MUTED);
    bool attached=selected>=0 && *chats[selected].attachment;
    label(y+1,x+2,width-4,awaiting_send?"SENDING…":attached?"PHOTO ATTACHED  ·  Ctrl+X remove  ·  Ctrl+V replace":"MESSAGE",focus==COMPOSE_FOCUS?ACCENT:MUTED,true);
    fill(y+2,x+1,1,width-2,PANEL);
    if(selected<0) {label(y+2,x+3,width-6,"Select a chat to start writing",MUTED,false);return;}
    wchar_t *draft=chats[selected].draft; int start=0,cells=0;
    for(int i=0;i<compose_cursor;i++) cells+=wcwidth(draft[i])>0?wcwidth(draft[i]):0;
    while(cells>width-8 && start<compose_cursor) {int w=wcwidth(draft[start++]); cells-=w>0?w:0;}
    if(!*draft) label(y+2,x+3,width-6,attached?"Add a caption…":"Write a message…",PANEL_MUTED,false);
    else wide_label(y+2,x+3,width-6,draft+start,PANEL);
    if(focus==COMPOSE_FOCUS && !help && !awaiting_send) {composer_y=y+2;composer_x=x+3+cells;curs_set(1);move(composer_y,composer_x);}
}
static void draw_chats(int sidebar) {
    fill(3,0,LINES-5,sidebar,PANEL);
    char title[128]; snprintf(title,sizeof title,"CHATS  %d",filtered_count);
    label(4,2,sidebar-4,title,focus==CHAT_FOCUS?PANEL_ACCENT:PANEL_MUTED,true);
    fill(6,2,1,sidebar-4,SELECTED);
    if(*query || focus==SEARCH_FOCUS) wide_label(6,3,sidebar-6,query,SELECTED);
    else label(6,3,sidebar-6,"/  Search conversations",SELECTED_MUTED,false);
    rule(8,1,sidebar-2,MUTED);
    int slots=(LINES-12)/3; if(slots<1) slots=1;
    for(int i=0;i<filtered_count;i++) if(filtered[i]==selected) {
        if(i<list_top) list_top=i;
        if(i>=list_top+slots) list_top=i-slots+1;
    }
    if(list_top>filtered_count-1) list_top=0;
    for(int i=0;i<slots && i+list_top<filtered_count;i++) {
        Chat *c=&chats[filtered[i+list_top]]; int y=9+i*3; bool active=filtered[i+list_top]==selected;
        int style=active?SELECTED:PANEL;
        if(active) fill(y,1,2,sidebar-2,style);
        label(y,2,sidebar-12,c->name,style,true);
        char clock[24]=""; if(c->time) time_text(c->time,clock,sizeof clock,"%H:%M");
        label(y,sidebar-7,5,clock,style,false);
        label(y+1,2,sidebar-7,*c->preview?c->preview:"Start a conversation",active?SELECTED_MUTED:PANEL_MUTED,false);
        if(c->unread) {char badge[16];snprintf(badge,sizeof badge,"%d",c->unread>999?999:c->unread);label(y+1,sidebar-5,4,badge,active?SELECTED:PANEL_ACCENT,true);}
    }
    if(!filtered_count) label(10,2,sidebar-4,*query?"No matching chats":"Waiting for chat sync…",PANEL_MUTED,false);
    attrset(COLOR_PAIR(MUTED)); for(int y=3;y<LINES-2;y++) mvaddwstr(y,sidebar,L"│");
}
static void draw_conversation(int x,int width) {
    if(selected<0) {
        int y=LINES/2-2;
        label(y,x+4,width-8,"Your conversations, a little quieter.",ACCENT,true);
        label(y+2,x+4,width-8,"Select a chat or press Ctrl+N to start one.",MUTED,false);
        label(y+4,x+4,width-8,"Built for the terminal. Colored by Omarchy.",MUTED,false);
        draw_editor(x,width);return;
    }
    Chat *c=&chats[selected]; label(4,x+3,width-6,c->name,BASE,true);
    char subtitle[256];
    if(strstr(c->id,"@g.us")) copy(subtitle,sizeof subtitle,"Group conversation");
    else if(demo || strstr(c->id,"@lid")) copy(subtitle,sizeof subtitle,demo?"Preview conversation":"Private conversation");
    else {char number[160];copy(number,sizeof number,c->id);number[strcspn(number,"@")]=0;snprintf(subtitle,sizeof subtitle,"Private conversation  ·  +%s",number);}
    label(6,x+3,width-6,subtitle,MUTED,false); rule(8,x+1,width-2,MUTED);
    int height=LINES-16; build_rows(width-8);
    int max_scroll=row_count>height?row_count-height:0;
    if(scroll_offset>max_scroll) scroll_offset=max_scroll;
    int start=row_count-height-scroll_offset; if(start<0) start=0;
    if(start>0 && !scroll_offset) {
        while(start<row_count && (!rows[start].meta || !rows[start].text[0])) start++;
    }
    visible_row_start=start;
    if(!row_count) {
        label(11,x+3,width-6,"No messages synced yet",MUTED,false);
        label(13,x+3,width-6,"History arrives from your phone after pairing.",MUTED,false);
    }
    for(int i=0;i<height && start+i<row_count;i++) {
        Row *r=&rows[start+i];
        if(r->style==OUTGOING && !r->meta) fill(9+i,x+2+r->indent,1,r->span+4,OUTGOING);
        wide_label(9+i,x+4+r->indent,width-8-r->indent,r->text,r->style);
        if(r->image_row>=0&&r->message_index>=0&&(r->image_row==0||i==0)) {
            int origin=i-r->image_row;
            if(origin<0||origin+photo_rows>height)
                label(9+i,x+4+r->indent,photo_columns,"Photo · scroll to see preview",r->style,false);
            else {
                Thumbnail *t=thumbnail_for(&messages[r->message_index],true);
                const char *hint=(!t||t->pending||!t->request)?"Loading photo…":*t->error?"Photo unavailable · click to open":(!t->data||!*t->data)?"Photo · click to open":NULL;
                if(hint)label(9+i,x+4+r->indent,photo_columns,hint,r->style,false);
            }
        }
    }
    draw_editor(x,width);
}
static void modal(void) {
    int width=COLS>90?66:COLS-8,x=(COLS-width)/2,y=LINES/2-5;
    fill(y,x,10,width,PANEL);
    label(y+1,x+3,width-6,help?"KEYBOARD":"NEW CONVERSATION",PANEL_ACCENT,true);
    if(help) {
        const char *lines[]={"Tab             Switch chats / message","↑ ↓ or j k      Select a chat","/               Search chats","Enter           Compose / send message","V               View photos; ← / → browse","Ctrl+V / Ctrl+X Paste / remove image","Ctrl+N          New chat  ·  PgUp/Dn scroll","Esc             Close  ·  Ctrl+Q quit"};
        for(int i=0;i<8;i++) label(y+2+i,x+3,width-6,lines[i],PANEL,false);
    } else {
        label(y+3,x+3,width-6,"International phone number, including country code",PANEL,false);
        wide_label(y+5,x+3,width-6,phone,SELECTED);
        label(y+7,x+3,width-6,"Enter  Open chat     Esc  Cancel",MUTED,false);
    }
}
static void draw(void) {
    if(viewer) {if(viewer_dirty)draw_photo();return;}
    if(!redraw)return;
    update_cell_size();
    if(strcmp(graphics,"external")) {printf("\033[?2026h");fflush(stdout);}
    clear_inline_images();
    erase(); curs_set(0); header();
    if(COLS<64 || LINES<22) label(5,2,COLS-4,"Resize terminal to at least 64 × 22",ACCENT,true);
    else if(qr) draw_qr();
    else {
        int sidebar=COLS/3; if(sidebar>38) sidebar=38; if(sidebar<24) sidebar=24;
        draw_chats(sidebar); draw_conversation(sidebar+1,COLS-sidebar-1);
    }
    fill(LINES-2,0,2,COLS,PANEL);
    label(LINES-2,2,COLS-4,notice,PANEL_MUTED,false);
    label(LINES-1,2,COLS-4,"Tab switch   / search   V photos   Ctrl+V image   F1 help   Ctrl+Q quit",PANEL_MUTED,false);
    if(help || focus==NEW_FOCUS) {curs_set(0);modal();}
    else if(focus==COMPOSE_FOCUS&&selected>=0&&!awaiting_send&&!qr&&COLS>=64&&LINES>=22)move(composer_y,composer_x);
    refresh();
    draw_inline_images();
    if(strcmp(graphics,"external")) {printf("\033[?2026l");fflush(stdout);}
    redraw=false;
}
static void send_message(void) {
    if(selected<0 || awaiting_send) return;
    if(clipboard_pending) {note("Wait for the clipboard image to finish loading before sending.");return;}
    if(demo) {note("Preview mode. No messages are sent.");return;}
    if(!online) {note("You are offline. Your draft is preserved.");return;}
    wchar_t *draft=chats[selected].draft; bool content=false;
    for(int i=0;draft[i];i++) if(!iswspace(draft[i])) content=true;
    bool attached=*chats[selected].attachment!=0;
    if(!content && !attached) return;
    char text[EDIT_SIZE*4+1]; const wchar_t *p=draft; mbstate_t st={0};
    size_t n=wcsrtombs(text,&p,sizeof text-1,&st); if(n==(size_t)-1) return; text[n]=0;
    json_object *o=json_object_new_object();
    json_object_object_add(o,"type",json_object_new_string(attached?"send-image":"send"));
    if(attached)json_object_object_add(o,"path",json_object_new_string(chats[selected].attachment));
    json_object_object_add(o,"jid",json_object_new_string(chats[selected].id));
    json_object_object_add(o,"text",json_object_new_string(text));
    json_object_object_add(o,"token",json_object_new_int64(++send_token));
    if(command(o)) {awaiting_send=true;note("Sending…");} json_object_put(o);
}
static void new_chat(void) {
    char digits[32]; int len=0;
    for(int i=0;phone[i];i++) if(phone[i]>=L'0'&&phone[i]<=L'9') digits[len++]=(char)phone[i];
    digits[len]=0;
    if(len<7 || len>15) {note("Use 7–15 digits, including the country code.");return;}
    char jid[64];snprintf(jid,sizeof jid,"%s@s.whatsapp.net",digits);
    Chat *c=find_chat(jid,true);if(!c) return;
    selected=(int)(c-chats); query[0]=0; focus=COMPOSE_FOCUS; history_request();
}
static void edit_key(wchar_t *buf,int capacity,int *cursor,wint_t key,bool special) {
    int len=(int)wcslen(buf);
    if(key==KEY_LEFT&&special) {if(*cursor>0)(*cursor)--;return;}
    if(key==KEY_RIGHT&&special) {if(*cursor<len)(*cursor)++;return;}
    if(key==KEY_HOME&&special) {*cursor=0;return;}
    if(key==KEY_END&&special) {*cursor=len;return;}
    if((special&&key==KEY_BACKSPACE)||(!special&&(key==127||key==8))) {
        if(*cursor>0) {memmove(buf+*cursor-1,buf+*cursor,(size_t)(len-*cursor+1)*sizeof *buf);(*cursor)--;}return;
    }
    if(special&&key==KEY_DC) {if(*cursor<len) memmove(buf+*cursor,buf+*cursor+1,(size_t)(len-*cursor)*sizeof *buf);return;}
    if(!special&&key==21) {buf[0]=0;*cursor=0;return;}
    if(!special&&iswprint(key)&&len<capacity-1) {
        memmove(buf+*cursor+1,buf+*cursor,(size_t)(len-*cursor+1)*sizeof *buf);buf[(*cursor)++]=(wchar_t)key;
    }
}
static void keypress(wint_t key,bool special) {
    if(special && key==PASTE_START) {pasting=true;return;}
    if(special && key==PASTE_END) {pasting=false;return;}
    if(pasting) {
        if(!awaiting_send && focus==COMPOSE_FOCUS && selected>=0 && !special) {
            if(key==L'\n'||key==L'\r'||key==L'\t')key=L' ';
            if(iswprint(key))edit_key(chats[selected].draft,EDIT_SIZE,&compose_cursor,key,false);
        }
        return;
    }
    if(!special&&key==17) {running=false;return;}
    if(viewer) {
        if(special&&key==KEY_RESIZE) {request_photo();return;}
        if(!special&&(key==27||key==L'\n')) {close_photo();if(viewing_attachment)focus=COMPOSE_FOCUS;return;}
        if(!special&&key==24&&viewing_attachment&&selected>=0) {discard_photo(&chats[selected]);close_photo();return;}
        if((special&&key==KEY_LEFT)||(!special&&key==L'h')) {next_photo(-1);return;}
        if((special&&key==KEY_RIGHT)||(!special&&key==L'l')) {next_photo(1);return;}
        if(!special&&(key==L'o'||key==L'O')&&*image_path) {
            pid_t pid=fork();
            if(pid==0) {
                if(bridge_in>=0)close(bridge_in);
                if(bridge_out>=0)close(bridge_out);
                int fd=open("/dev/null",O_RDWR);if(fd>=0) {dup2(fd,0);dup2(fd,1);dup2(fd,2);close(fd);}
                execlp("imv","imv",image_path,(char*)NULL);execlp("xdg-open","xdg-open",image_path,(char*)NULL);_exit(127);
            }
            note("Opened photo in your image viewer");
        }
        return;
    }
    if((special&&key==KEY_F(1))||(!special&&key==L'?'&&focus==CHAT_FOCUS)) {help=!help;return;}
    if(help) {if(!special&&key==27)help=false;return;}
    if(special&&key==KEY_RESIZE) return;
    if(awaiting_send) return;
    if(focus==NEW_FOCUS) {
        if(!special&&key==27) {focus=CHAT_FOCUS;return;}
        if(key==L'\n'||(special&&key==KEY_ENTER)) {new_chat();return;}
        int cursor=(int)wcslen(phone);
        if(special||key==127||key==8||key==21||key==L'+'||key==L' '||(key>=L'0'&&key<=L'9')) edit_key(phone,32,&cursor,key,special);
        return;
    }
    if(qr) return;
    if(!special&&key==22 && selected>=0) {
        if(clipboard_pending)return;
        json_object *o=json_object_new_object();
        json_object_object_add(o,"type",json_object_new_string("clipboard"));
        json_object_object_add(o,"jid",json_object_new_string(chats[selected].id));
        if(command(o)) {clipboard_pending=true;note("Reading clipboard image…");}
        json_object_put(o);return;
    }
    if(!special&&key==24&&selected>=0) {discard_photo(&chats[selected]);note("Photo removed; caption kept.");return;}
    if(!special&&key==14) {focus=NEW_FOCUS;phone[0]=0;return;}
    if(special&&(key==KEY_PPAGE||key==KEY_NPAGE)) {scroll_offset+=key==KEY_PPAGE?10:-10;if(scroll_offset<0)scroll_offset=0;return;}
    if(!special&&key==27) {focus=CHAT_FOCUS;return;}
    if(!special&&key==L'\t') {focus=focus==COMPOSE_FOCUS?CHAT_FOCUS:COMPOSE_FOCUS;compose_cursor=selected>=0?(int)wcslen(chats[selected].draft):0;return;}
    if(focus==SEARCH_FOCUS) {
        if(key==L'\n'||(special&&key==KEY_ENTER)) {
            filter_chats();if(filtered_count) {selected=filtered[0];history_request();}focus=CHAT_FOCUS;return;
        }
        int cursor=(int)wcslen(query);edit_key(query,128,&cursor,key,special);list_top=0;return;
    }
    if(special&&key==KEY_MOUSE) {
        MEVENT mouse;if(getmouse(&mouse)!=OK)return;
        if(mouse.bstate&BUTTON4_PRESSED) {scroll_offset+=3;return;}
        if(mouse.bstate&BUTTON5_PRESSED) {scroll_offset-=3;if(scroll_offset<0)scroll_offset=0;return;}
        int sidebar=COLS/3;if(sidebar>38)sidebar=38;if(sidebar<24)sidebar=24;
        if(mouse.bstate&BUTTON1_CLICKED) {
            if(mouse.x<sidebar && mouse.y>=9 && mouse.y<LINES-4) {
                int index=(mouse.y-9)/3+list_top;
                if(index<filtered_count) {selected=filtered[index];focus=CHAT_FOCUS;history_request();}
            } else if(mouse.x>sidebar&&mouse.y>=LINES-5) focus=COMPOSE_FOCUS;
            else if(mouse.x>sidebar&&mouse.y>=9&&mouse.y<LINES-7) {
                int r=visible_row_start+mouse.y-9;
                if(r>=0&&r<row_count&&rows[r].message_index>=0) {
                    Message *m=&messages[rows[r].message_index];
                    if(m->photo) {copy(view_jid,sizeof view_jid,m->jid);copy(view_id,sizeof view_id,m->id);viewing_attachment=false;viewer=true;request_photo();}
                }
            }
        }return;
    }
    if(focus==COMPOSE_FOCUS) {
        if(key==L'\n'||(special&&key==KEY_ENTER)) {send_message();return;}
        if(selected>=0) edit_key(chats[selected].draft,EDIT_SIZE,&compose_cursor,key,special);
        return;
    }
    if(!special&&key==L'/') {query[0]=0;focus=SEARCH_FOCUS;return;}
    if(!special&&(key==L'v'||key==L'V')) {open_photo(false);return;}
    if((special&&key==KEY_UP)||(!special&&key==L'k')) {move_chat(-1);return;}
    if((special&&key==KEY_DOWN)||(!special&&key==L'j')) {move_chat(1);return;}
    if(key==L'\n'||(special&&key==KEY_ENTER)) {focus=COMPOSE_FOCUS;history_request();return;}
    if(!special&&key==L'q') running=false;
}
static bool start_bridge(void) {
    int incoming[2],outgoing[2];
    if(pipe(incoming)<0)return false;
    if(pipe(outgoing)<0) {close(incoming[0]);close(incoming[1]);return false;}
    child=fork();
    if(child<0) {close(incoming[0]);close(incoming[1]);close(outgoing[0]);close(outgoing[1]);return false;}
    if(child==0) {
        dup2(incoming[0],STDIN_FILENO);dup2(outgoing[1],STDOUT_FILENO);
        close(incoming[0]);close(incoming[1]);close(outgoing[0]);close(outgoing[1]);
        char logfile[PATH_MAX];if(!join_path(logfile,sizeof logfile,state_dir,"/bridge.log"))_exit(1);
        int fd=open(logfile,O_WRONLY|O_CREAT|O_APPEND,0600);if(fd>=0) {dup2(fd,STDERR_FILENO);close(fd);}
        char script[PATH_MAX];copy(script,sizeof script,bridge_path);
        if(demo) {
            char *slash=strrchr(script,'/');if(!slash)_exit(1);
            copy(slash+1,(size_t)(script+sizeof script-slash-1),"preview.mjs");
        }
        execlp("node","node",script,(char*)NULL);perror("node");_exit(127);
    }
    close(incoming[0]);close(outgoing[1]);bridge_in=incoming[1];bridge_out=outgoing[0];
    fcntl(bridge_out,F_SETFL,fcntl(bridge_out,F_GETFL)|O_NONBLOCK);return true;
}
static void demo_data(void) {
    const char *names[]={"Omarchy community","Alex Morgan","Design studio","Weekend plans","Sam Rivera"};
    const char *previews[]={"The terminal feels like home.","That looks great — see you soon!","A calmer way to keep in touch.","Coffee on Saturday?","Thanks for the update."};
    time_t now=time(NULL);
    for(int i=0;i<5;i++) {
        char jid[80];snprintf(jid,sizeof jid,"preview-%d@s.whatsapp.net",i);Chat *c=find_chat(jid,true);
        copy(c->name,sizeof c->name,names[i]);copy(c->preview,sizeof c->preview,previews[i]);c->time=now-i*600;c->unread=i==2?3:0;
    }
    const char *bodies[]={"Welcome to a quieter corner of WhatsApp. Your chats, right here in the terminal.",
        "The colors come from your active Omarchy theme. Switch themes and this view follows along.",
        "Nice. A clean sidebar, a little breathing room, and keyboard controls.",
        "Exactly. Scan the QR with your phone to connect, then choose a chat and start typing.",
        "This is preview mode. Nothing here is connected to your account."};
    for(int i=0;i<5;i++) {
        Message *m=&messages[message_count++];snprintf(m->id,sizeof m->id,"demo-%d",i);
        copy(m->jid,sizeof m->jid,chats[0].id);copy(m->sender,sizeof m->sender,"Omarchy community");
        copy(m->status,sizeof m->status,"read");m->text=strdup(bodies[i]);m->time=now-600+i*90;m->mine=i==2;
    }
    selected=0;online=true;copy(status,sizeof status,"Preview");note("Preview mode · sample conversations · no network connection");
}
static void detect_graphics(void) {
    const char *override=getenv("WHATSAPP_TUI_GRAPHICS");
    if(override&&(!strcmp(override,"kitty")||!strcmp(override,"sixel")||!strcmp(override,"external"))) {graphics=override;return;}
    if(getenv("TMUX")||getenv("STY"))return;
    const char *term=getenv("TERM"),*program=getenv("TERM_PROGRAM");
    if(getenv("KITTY_WINDOW_ID") || (term&&strstr(term,"kitty")) || (program&&(!strcmp(program,"ghostty")||!strcmp(program,"kitty")))) {graphics="kitty";return;}
    if((term&&strstr(term,"foot")) || (program&&!strcmp(program,"foot"))) {graphics="sixel";return;}
    pid_t pid=getppid();
    for(int i=0;i<10&&pid>1;i++) {
        char file[80],comm[128];snprintf(file,sizeof file,"/proc/%ld/comm",(long)pid);
        FILE *f=fopen(file,"r");if(!f)break;
        comm[0]=0;if(fgets(comm,sizeof comm,f))comm[strcspn(comm,"\r\n")]=0;fclose(f);
        if(!strcmp(comm,"foot")||!strcmp(comm,"footclient")) {graphics="sixel";return;}
        if(!strcmp(comm,"kitty")||!strcmp(comm,"ghostty")) {graphics="kitty";return;}
        snprintf(file,sizeof file,"/proc/%ld/status",(long)pid);f=fopen(file,"r");if(!f)break;
        char line[256];long parent=0;while(fgets(line,sizeof line,f))if(sscanf(line,"PPid: %ld",&parent)==1)break;
        fclose(f);pid=(pid_t)parent;
    }
}
static bool paths(void) {
    const char *home=getenv("HOME");if(!home) {fprintf(stderr,"HOME is not set\n");return false;}
    const char *state=getenv("XDG_STATE_HOME");char base[PATH_MAX];
    if(state) copy(base,sizeof base,state);else snprintf(base,sizeof base,"%s/.local/state",home);
    if(!join_path(palette_path,sizeof palette_path,base,"/omarchy/current/theme/colors.toml"))return false;
    if(access(palette_path,R_OK)) snprintf(palette_path,sizeof palette_path,"%s/.config/omarchy/current/theme/colors.toml",home);
    if(!join_path(state_dir,sizeof state_dir,base,"/whatsapp-tui"))return false;
    /* Create only our private state directory; the XDG state root normally exists. */
    if(mkdir(base,0700)<0 && errno!=EEXIST) {perror(base);return false;}
    if(mkdir(state_dir,0700)<0 && errno!=EEXIST) {perror(state_dir);return false;}
    const char *override=getenv("WHATSAPP_TUI_BRIDGE");
    if(override) copy(bridge_path,sizeof bridge_path,override);
    else {
        char executable[PATH_MAX];ssize_t n=readlink("/proc/self/exe",executable,sizeof executable-1);
        if(n<0)return false;
        executable[n]=0;
        char *slash=strrchr(executable,'/');if(!slash)return false;*slash=0;
        slash=strrchr(executable,'/');if(!slash)return false;*slash=0;
        if(!join_path(bridge_path,sizeof bridge_path,executable,"/bridge/index.mjs"))return false;
    }
    return true;
}
int main(int argc,char **argv) {
    setlocale(LC_ALL,"");
    bool reset=false;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--demo")) demo=true;
        else if(!strcmp(argv[i],"--reset-session")) reset=true;
        else if(!strcmp(argv[i],"--help")) {
            puts("WhatsApp TUI · C interface with live Omarchy colors\n\nUsage: whatsapp-tui [--demo | --reset-session]\n\n--demo           Preview without a WhatsApp connection\n--reset-session  Confirm removal of local pairing and cached history/media\n\nTab: focus  /: search  Enter: compose/send  Ctrl+N: new chat\nV: view photos  Ctrl+V: attach clipboard image  Ctrl+X: remove image\nPhoto viewer: arrows browse  O external viewer  Esc close\nPgUp/PgDn: scroll  F1: help  Ctrl+Q: quit\n\nPalette: $XDG_STATE_HOME/omarchy/current/theme/colors.toml\nSession: $XDG_STATE_HOME/whatsapp-tui (default ~/.local/state)\nImages: Foot (Sixel), Kitty/Ghostty (Kitty graphics), otherwise external viewer\nWHATSAPP_TUI_GRAPHICS=sixel|kitty|external overrides image detection");return 0;
        } else {fprintf(stderr,"Unknown option: %s\n",argv[i]);return 1;}
    }
    if(!paths())return 1;
    if(!demo) {
        char lock_path[PATH_MAX];if(!join_path(lock_path,sizeof lock_path,state_dir,"/session.lock"))return 1;
        session_lock=open(lock_path,O_RDWR|O_CREAT|O_CLOEXEC,0600);
        if(session_lock<0 || flock(session_lock,LOCK_EX|LOCK_NB)<0) {
            fprintf(stderr,"WhatsApp TUI is already running, or its state folder is inaccessible.\n");return 1;
        }
    }
    if(reset) {
        fprintf(stderr,"Remove this app's pairing and cached chat history? Type RESET: ");
        char response[32];if(!fgets(response,sizeof response,stdin)||strcmp(response,"RESET\n")) {puts("Cancelled.");return 0;}
        /* Delegate scoped deletion to the bridge's reset utility. */
        char reset_script[PATH_MAX];copy(reset_script,sizeof reset_script,bridge_path);
        char *slash=strrchr(reset_script,'/');if(!slash)return 1;copy(slash+1,(size_t)(reset_script+sizeof reset_script-slash-1),"reset.mjs");
        execlp("node","node",reset_script,(char*)NULL);perror("node");return 1;
    }
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)) {fprintf(stderr,"Run this app in an interactive terminal.\n");return 1;}
    signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);signal(SIGPIPE,SIG_IGN);
    rows=calloc(MAX_ROWS,sizeof *rows);if(!rows)return 1;
    detect_graphics();
    if(!start_bridge()) {perror("WhatsApp bridge");free(rows);return 1;}
    initscr();cbreak();noecho();keypad(stdscr,TRUE);timeout(100);set_escdelay(25);
    define_key("\033[200~",PASTE_START);define_key("\033[201~",PASTE_END);
    printf("\033[?2004h");fflush(stdout);
    start_color();use_default_colors();load_theme(true);mousemask(ALL_MOUSE_EVENTS,NULL);mouseinterval(150);
    if(demo)demo_data();
    time_t last_theme=0;
    while(running&&!interrupted) {
        pid_t exited;while((exited=waitpid(-1,NULL,WNOHANG))>0)if(exited==child)child=-1;
        if(bridge_out>=0)read_bridge();
        time_t now=time(NULL);if(now!=last_theme) {load_theme(false);last_theme=now;}
        filter_chats();draw();wint_t key;int result=get_wch(&key);
        if(result!=ERR) {redraw=true;keypress(key,result==KEY_CODE_YES);}
    }
    delete_terminal_image();
    clear_inline_images();
    printf("\033[?2004l");fflush(stdout);endwin();
    if(bridge_in>=0)close(bridge_in);
    if(bridge_out>=0)close(bridge_out);
    if(child>0) {kill(child,SIGTERM);waitpid(child,NULL,0);}
    if(qr)QRcode_free(qr);
    for(int i=0;i<message_count;i++)free(messages[i].text);
    for(int i=0;i<THUMB_CACHE;i++)free(thumbnails[i].data);
    if(session_lock>=0)close(session_lock);
    free(rows);free(wire);free(image_data);return 0;
}
