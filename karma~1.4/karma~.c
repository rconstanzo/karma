/**
	@file
	karma~
	@ingroup
 msp
 
 __________________________________________________________________
 */

#include "stdlib.h"
#include "math.h"
#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "z_dsp.h"
#include "ext_atomic.h"

#define LINTRP(x, y, z) (y + x * (z - y))
// Hermitic Cubic Interp ( courtesy of Alex Harker: http://www.alexanderjharker.co.uk/ )
#define HRMCBINTRP(f, z, a, b, c) ((((0.5*(c - z) + 1.5*(a - b))*f + (z - 2.5*a + b + b - 0.5*c))*f + (0.5*(b - z)))*f + a)

typedef struct _karma {
    t_pxobject		ob;
    t_buffer_ref *buf;
    t_symbol *bufname;
    double pos;
    double maxpos;
    double jump;
    double extlwin;
    double xstart;
    double sr;
    double bmsr;
    double srscale;
    double prev;
    double oLprev;
    double oRprev;
    double o3prev;
    double o4prev;
    double oLdif;
    double oRdif;
    double o3dif;
    double o4dif;
    double writevaL;
    double writevaR;
    double writeva3;
    double writeva4;
    double fad;
    double ovdb;
    double ovd;
    t_ptr_int numof;
    t_ptr_int loop;
    t_ptr_int start;
    t_ptr_int end;
    t_ptr_int rpos;
    t_ptr_int rfad;
    t_ptr_int pfad;
    t_ptr_int bframes;
    t_ptr_int bchans;
    t_ptr_int chans;
    t_ptr_int ramp;
    t_ptr_int snramp;
    t_ptr_int rprtime;
    t_ptr_int curv;
    t_ptr_int interpflag;
    char statecontrol;
    char pupdwn;
    char rupdwn;
    char diro;
    char dirp;
    char doend;
    t_bool append;
    t_bool go;
    t_bool rec;
    t_bool recpre;
    t_bool looprec;
    t_bool rectoo;
    t_bool clockgo;
    t_bool wrap;
    t_bool trig;
    t_bool jnoff;
    t_bool first;
    t_bool firstd;
    t_bool skip;
    t_bool buf_mod;
    void *tclock;
    void *messout;
} t_karma;

void *karma_new(t_symbol *s, short argc, t_atom *argv);
void karma_free(t_karma *x);
void karma_stop(t_karma *x);
void karma_play(t_karma *x);
void karma_record(t_karma *x);
void karma_start(t_karma *x, double strt);
t_max_err karmabuf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat);
void karma_assist(t_karma *x, void *b, long m, long a, char *s);
void karma_dblclick(t_karma *x);
void karma_overdub(t_karma *x, double o);
void karma_window(t_karma *x, double dur);
void karma_modset(t_karma *x, t_buffer_obj *b);
void karma_clckdo(t_karma *x);
void karma_bufchange(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void karma_setup(t_karma *x, t_symbol *s);
void karma_jump(t_karma *x, double j);
void karma_append(t_karma *x);
void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags);
void karma_mperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
void karma_sperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
void karma_qperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);

static t_symbol *ps_nothing, *ps_buffer_modified; static t_class *karma_class = NULL;

static inline double dease_func(double y1, char updwn, double ramp, t_ptr_int pfad)
{ return updwn ? y1*(0.5*(1.0-cos((1.0-(((double)pfad)/ramp))*PI))) : y1*(0.5*(1.0-cos((((double)pfad)/ramp)*PI))); }

static inline double snrdease_func(double y1, double fad, t_ptr_int curv)
{
    switch (curv)
    {
        case 0: y1 = y1*(1.0-fad); break;  //linear
        case 1: y1 = y1*(1.0-(sin((fad-1)*PI/2)+1)); break;  //sine ease in
        case 2: y1 = y1*(1.0-(fad*fad*fad)); break;         //cubic ease in
        case 3: fad = fad-1; y1 = y1*(1.0-(fad*fad*fad+1)); break; //cubic ease out
        case 4: y1 = y1*(1.0-((fad==0.0)?fad:pow(2, 10*(fad-1)))); break; //exponential ease in
        case 5: y1 = y1*(1.0-((fad==1.0)?fad:1-(pow(2, -10*fad)))); break; //exponential ease out
        case 6:
            if ((fad>0)&&(fad<0.5)) y1 = y1*(1.0-(0.5*pow(2, (20*fad)-10)));
            else if ((fad<1)&&(fad>0.5)) y1 = y1*(1.0-(-0.5*pow(2, (-20*fad)+10)+1)); break; //exp easinout
    }
    return y1;
}

static inline void bufeasoff(t_ptr_int frms, float *b, t_ptr_int nchn, t_ptr_int mrk, char dr, double ramp)
{
    long i, fadpos;
    for (i=0; i<ramp; i++)
    {
        fadpos = mrk + (dr*i);
        if(!((fadpos<0)||(fadpos>frms)))
        {
            b[fadpos*nchn] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
            if (nchn > 1)
            {
                b[(fadpos*nchn)+1] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                if (nchn > 2)
                {
                    b[(fadpos*nchn)+2] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    if (nchn > 3)
                    {
                        b[(fadpos*nchn)+3] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    }
                }
            }
        }
    } return;
}

static inline void bufeas(t_ptr_int frms,float *b,t_ptr_int nchn,t_ptr_int mrk,t_ptr_int mrk2,char dr, double ramp)
{
    long i, fadpos, fadpos2, fadpos3;
    for (i=0; i<ramp; i++)
    {
        fadpos = (mrk+(-dr)) + (-dr*i); fadpos2 = (mrk2+(-dr)) + (-dr*i); fadpos3 = mrk2 + (dr*i);
        if(!((fadpos<0)||(fadpos>frms)))
        {
            b[fadpos*nchn] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
            if (nchn > 1)
            {
                b[(fadpos*nchn)+1] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                if(nchn > 2)
                {
                    b[(fadpos*nchn)+2] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    if(nchn > 3)
                    {
                        b[(fadpos*nchn)+3] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    }
                }
            }
        }
        if(!((fadpos2<0)||(fadpos2>frms)))
        {
            b[fadpos2*nchn] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
            if (nchn > 1)
            {
                b[(fadpos2*nchn)+1] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                if(nchn > 2)
                {
                    b[(fadpos2*nchn)+2] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    if(nchn > 3)
                    {
                        b[(fadpos2*nchn)+3] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    }
                }
            }
        }
        if(!((fadpos3<0)||(fadpos3>frms)))
        {
            b[fadpos3*nchn] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
            if (nchn > 1)
            {
                b[(fadpos3*nchn)+1] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                if(nchn > 2)
                {
                    b[(fadpos3*nchn)+2] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    if(nchn > 3)
                    {
                        b[(fadpos3*nchn)+3] *= 0.5 * ( 1.0-cos( (((double)i)/ramp) * PI));
                    }
                }
            }
        }
    } return;
}

static inline void interp_index
(t_ptr_int pos,t_ptr_int *indx0,t_ptr_int *indx1,t_ptr_int *indx2,t_ptr_int *indx3,char dir,char diro,t_ptr_int loop,t_ptr_int frmsm)
{
    *indx0 = pos - dir;                                                         //calc of indexes 4 interps
    if (diro>=0) { if(*indx0 < 0){ *indx0 = (loop+1) + *indx0; }else if(*indx0 > loop){ *indx0 = *indx0 - (loop+1); } }
    else
    {
        if(*indx0 < (frmsm-loop))
        { *indx0 = frmsm - ((frmsm-loop)-*indx0); }else if(*indx0 > frmsm){ *indx0 = (frmsm-loop)+(*indx0-frmsm); }
    }
    
    *indx1 = pos; *indx2 = pos + dir;
    if (diro>=0) { if(*indx2 < 0){ *indx2 = (loop+1) + *indx2; }else if(*indx2 > loop){ *indx2 = *indx2 - (loop+1); } }
    else
    {
        if(*indx2 < (frmsm-loop)){ *indx2 = frmsm - ((frmsm-loop)-*indx2); }
        else if(*indx2 > frmsm){ *indx2 = (frmsm-loop)+(*indx2-frmsm); }
    }
    
    *indx3 = *indx2 + dir;
    if (diro>=0)
    { if(*indx3 < 0){ *indx3 = (loop+1) + *indx3; }else if(*indx3 > loop){ *indx3 = *indx3 - (loop+1); } }
    else
    {
        if(*indx3 < (frmsm-loop)){ *indx3 = frmsm - ((frmsm-loop)-*indx3); }
        else if(*indx3 > frmsm){ *indx3 = (frmsm-loop)+(*indx3-frmsm); }
    } return;
}

void ext_main(void *r)
{
    t_class *c = class_new("karma~", (method)karma_new, (method)karma_free, (long)sizeof(t_karma), 0L, A_GIMME, 0);
    
    class_addmethod(c, (method)karma_start, "position", A_FLOAT, 0);
    class_addmethod(c, (method)karma_window, "window", A_FLOAT, 0);
    class_addmethod(c, (method)karma_jump, "jump", A_FLOAT, 0);
    class_addmethod(c, (method)karma_stop, "stop", 0);
    class_addmethod(c, (method)karma_play, "play", 0);
    class_addmethod(c, (method)karma_record, "record", 0);
    class_addmethod(c, (method)karma_append, "append", 0);
    class_addmethod(c, (method)karma_bufchange, "set", A_GIMME, 0);
    class_addmethod(c, (method)karma_overdub, "overdub", A_FLOAT, 0);
    class_addmethod(c, (method)karma_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)karma_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)karma_dblclick, "dblclick", A_CANT, 0);
    class_addmethod(c, (method)karmabuf_notify, "notify", A_CANT, 0);
    
    CLASS_ATTR_LONG(c, "report", 0, t_karma, rprtime);
    CLASS_ATTR_FILTER_MIN(c, "report", 0);
    CLASS_ATTR_LONG(c, "ramp", 0, t_karma, ramp);
    CLASS_ATTR_FILTER_MIN(c, "ramp", 0);
    CLASS_ATTR_FILTER_MAX(c, "ramp", 2048);
    CLASS_ATTR_LONG(c, "snramp", 0, t_karma, snramp);
    CLASS_ATTR_FILTER_MIN(c, "snramp", 0);
    CLASS_ATTR_FILTER_MAX(c, "snramp", 2048);
    CLASS_ATTR_LONG(c, "snrcurv", 0, t_karma, curv);
    CLASS_ATTR_FILTER_MIN(c, "snrcurv", 0);
    CLASS_ATTR_FILTER_MAX(c, "snrcurv", 6);
    CLASS_ATTR_LONG(c, "interp", 0, t_karma, interpflag);
    CLASS_ATTR_FILTER_MIN(c, "interp", 0);
    CLASS_ATTR_FILTER_MAX(c, "interp", 1);
    
    class_dspinit(c); class_register(CLASS_BOX, c); karma_class = c;
    
    ps_nothing = gensym(""); ps_buffer_modified = gensym("buffer_modified");
    
}

void *karma_new(t_symbol *s, short argc, t_atom *argv)
{
    t_karma *x; t_symbol *bufname = 0; t_ptr_int chans = 0;
    t_ptr_int attrstart = attr_args_offset(argc, argv);
    
    x = (t_karma *)object_alloc(karma_class);
    x->skip = 0;
    if (attrstart && argv) { bufname = atom_getsym(argv); if (attrstart>1) { chans = atom_getlong(argv+1); } }
    else { object_error((t_object *)x,"will not load properly without associated buffer~ name!"); goto zero; }
    
    if (x) {
        if (chans <= 1) { dsp_setup((t_pxobject *)x, 2); chans = 1; }
        else if (chans == 2) { dsp_setup((t_pxobject *)x, 3); chans = 2; }
        else { dsp_setup((t_pxobject *)x, 4); chans = 4; }
        
        x->rpos = -1; x->rprtime = 50; x->snramp = x->ramp = 256; x->pfad = x->rfad = 257;
        x->sr = sys_getsr(); x->ovd = x->ovdb = 1.0; x->curv = x->interpflag = 1;
        x->pupdwn = x->rupdwn = x->first = x->firstd = x->append = x->jnoff = x->statecontrol = 0;
        x->dirp = x->diro = x->recpre = x->rec = x->rectoo = x->doend = x->go = x->trig = 0;
        x->numof = x->writevaL = x->writevaR = x->writeva3 = x->writeva4 = x->wrap = x->looprec = 0; x->maxpos = x->pos = 0.0;
        x->xstart = x->jump = x->fad = x->oLdif = x->oRdif = x->o3dif = x->o4dif = x->oLprev = x->oRprev = x->o3prev = x->o4prev = x->prev = 0.0;
        
        if (bufname != 0) x->bufname = bufname; else object_error((t_object *)x, "needs associated buffer~ name");
        
        x->chans = chans; x->messout = listout(x); x->tclock = clock_new((t_object * )x, (method)karma_clckdo);
        attr_args_process(x, argc, argv);
        
        if (chans <= 1) { outlet_new(x, "signal"); outlet_new(x, "signal"); }
        else if (chans == 2) { outlet_new(x, "signal"); outlet_new(x, "signal"); outlet_new(x, "signal");}
        else
        { outlet_new(x, "signal"); outlet_new(x, "signal"); outlet_new(x, "signal"); outlet_new(x, "signal"); outlet_new(x, "signal");}
        x->skip = 1; x->ob.z_misc |= Z_NO_INPLACE;
    }
zero:
    return (x);
}

void karma_free(t_karma *x)
{ if (x->skip) { dsp_free((t_pxobject *)x); object_free(x->buf); object_free(x->tclock); object_free(x->messout); } }

void karma_dblclick(t_karma *x) { buffer_view(buffer_ref_getobject(x->buf)); }

void karma_setup(t_karma *x, t_symbol *s)
{
    t_buffer_obj *buf; x->bufname = s;
    if(!x->buf) x->buf = buffer_ref_new((t_object *)x, s);
    else buffer_ref_set(x->buf, s);
    buf = buffer_ref_getobject(x->buf);
    if (buf==NULL)
    {x->buf = 0; object_error((t_object *)x, "there's no buffer~ named '%s'", s->s_name);}
    else
    {
        x->diro = 0;
        x->maxpos = x->pos = 0.0; x->rpos = -1;
        x->bchans = buffer_getchannelcount(buf);
        x->bframes = buffer_getframecount(buf);
        x->bmsr = buffer_getmillisamplerate(buf);
        x->srscale = buffer_getsamplerate(buf) / x->sr;
        x->xstart = x->start = 0.0; x->extlwin = 1.;
        x->loop = x->end = x->bframes - 1;
    }
}

void karma_modset(t_karma *x, t_buffer_obj *b)
{
    double bsr, bmsr;
    t_ptr_int chans;
    t_ptr_int frames;
    
    if (b)
    {
        bsr = buffer_getsamplerate(b);
        chans = buffer_getchannelcount(b);
        frames = buffer_getframecount(b);
        bmsr = buffer_getmillisamplerate(b);
        if (((x->bchans != chans)||(x->bframes != frames))||(x->bmsr != bmsr))
        {
            x->bmsr = bmsr; x->srscale = bsr/x->sr; x->bframes = frames; x->bchans = chans;
            x->start = 0.0; x->loop = x->end = x->bframes-1;
            karma_window(x, x->extlwin); karma_start(x, x->xstart);
        }
    }
}

void karma_bufchange(t_karma *x, t_symbol *s, short argc, t_atom *argv)
{
    t_buffer_obj *buf;
    t_symbol *b = atom_getsym(argv);
    if (b != ps_nothing)
    {
        x->bufname = b;
        if(!x->buf) x->buf = buffer_ref_new((t_object *)x, b);
        else buffer_ref_set(x->buf, b);
        buf = buffer_ref_getobject(x->buf);
        if (buf==NULL)
        {x->buf = 0; object_error((t_object *)x, "there's no buffer~ named '%s'", b->s_name);}
        else
        {
            x->diro = 0; x->maxpos = x->pos = 0.0; x->rpos = -1;
            x->bchans = buffer_getchannelcount(buf);
            x->bframes = buffer_getframecount(buf);
            x->bmsr = buffer_getmillisamplerate(buf);
            x->srscale = buffer_getsamplerate(buf) / x->sr;
            if(argc<=1)
            {
                x->start = 0.0;
                x->loop = x->end = x->bframes - 1;
                karma_window(x, x->extlwin); karma_start(x, x->xstart);
            }
            else if(argc==2)
            {
                x->start = 0.0;
                x->loop = x->end = atom_getfloat(argv+1) * x->bframes - 1;
            }
            else
            {
                x->start = atom_getfloat(argv+1) * x->bframes - 1;
                x->loop = x->end = atom_getfloat(argv+2) * x->bframes - 1;
            }
            
        }
    } else { object_error((t_object *)x, "needs an associated buffer~ name"); }
}

void karma_clckdo(t_karma *x)
{
    if (x->rprtime!=0)
    {
        t_ptr_int frames = x->bframes-1; t_ptr_int loop = x->loop; t_ptr_int diro = x->diro;
        t_bool rec = x->rec; t_bool go = x->go;
        double bmsr = x->bmsr; double pos = x->pos; double xtlwin = x->extlwin;
        t_atom messlist[6];
        atom_setfloat(messlist, CLAMP((diro<0) ? (pos-(frames-loop))/loop : pos/loop, 0., 1.));
        atom_setlong(messlist+1, go);     atom_setlong(messlist+2, rec);
        atom_setfloat(messlist+3, (diro<0) ? (frames-loop)/bmsr : 0.0);
        atom_setfloat(messlist+4, (diro<0) ? frames/bmsr : loop/bmsr);
        atom_setfloat(messlist+5, (xtlwin*loop)/bmsr);
        outlet_list(x->messout, 0L, 6, messlist);   // &messlist
        if(sys_getdspstate()&&(x->rprtime>0)) { clock_delay(x->tclock, x->rprtime); }
    }
}

void karma_assist(t_karma *x, void *b, long m, long a, char *s)
{
    long fake; fake = a+1; a = (a < x->chans) ? 0 : 1;
    
    if (m == ASSIST_INLET)
    { switch (a) { case 0: sprintf(s, "Signal: Recording Input %ld", fake); break; case 1: sprintf(s, "Signal: Speed"); break; } }
    else
    {
        switch (a)
        {
            case 0: sprintf(s, "Signal: Output %ld", fake); break;
            case 1: sprintf(s, "List: current position (0. to 1.) play state (int) record state (int) start position (ms) end position (ms) window size (ms)"); break;
        }
    }
}

void karma_start(t_karma *x, double strt)
{
    x->xstart = strt;
    if (!x->looprec)
    {
        if(x->diro<0)
        {
            x->start = CLAMP( ((x->bframes-1)-x->loop) + (x->xstart*x->loop), (x->bframes-1)-x->loop, x->bframes-1);
            x->end = x->start + (x->extlwin*x->loop);
            if (x->end>(x->bframes-1)) { x->end=((x->bframes-1)-x->loop)+(x->end-(x->bframes-1)); x->wrap=1; } else x->wrap=0;
        }
        else
        {
            x->start = CLAMP(strt * x->loop, 0.0, x->loop); x->end = x->start + (x->extlwin * x->loop);
            if (x->end > x->loop) { x->end = x->end - x->loop; x->wrap = 1; } else x->wrap = 0;
        }
    }
}

void karma_window(t_karma *x, double dur)
{
    t_ptr_int loop = x->loop;
    if (!x->looprec)
    {
        x->extlwin = (dur<0.001) ? 0.001 : dur;
        if (x->diro<0)
        {
            x->end = x->start + (x->extlwin*loop);
            if (x->end > (x->bframes-1)) { x->end=((x->bframes-1)-x->loop)+(x->end-(x->bframes-1)); x->wrap=1; } else x->wrap=0;
        }
        else { x->end=x->start+(x->extlwin*loop); if(x->end>loop){ x->end=x->end-loop; x->wrap=1; } else x->wrap=0; }
    }
    else { x->extlwin = (dur<0.001) ? 0.001 : dur; }
}

void karma_stop(t_karma *x) { if (x->firstd) { x->statecontrol = x->rectoo ? 6 : 7; x->append = 0; } }

void karma_play(t_karma *x)
{
    if ((!x->go)&&(x->append)) { x->statecontrol=9; x->fad = 0.0; }
    else if((x->rec)||(x->append)){ x->statecontrol=x->rectoo?4:3; } else x->statecontrol=5;
    x->go=1;
}

void karma_record(t_karma *x)
{
    float *b; long i; char sc;
    t_bool r, g, rt, ap, fr;
    t_ptr_int nc; t_ptr_int frms;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    r = x->rec; g = x->go; rt = x->rectoo; ap = x->append; fr = x->first; sc = x->statecontrol;
    if (r) { if (rt) { sc = 2; } else sc = 3; }
    else
    {
        if (ap)
        { if(g) { if (rt) { sc = 2; } else sc = 10; } else { sc = 1; } }
        else
        {
            if (!g)
            {
                fr = 1;
                if (buf)
                {
                    nc = x->bchans; frms = x->bframes;
                    b = buffer_locksamples(buf); if (!b) goto zero;
                    for (i=0; i < frms; i++)
                    {
                        if (nc > 1)
                        {
                            b[i*nc] = 0.0; b[(i*nc)+1] = 0.0;
                            if (nc > 2)
                            {
                                b[(i*nc)+2] = 0.0;
                                if (nc > 3)
                                {
                                    b[(i*nc)+3] = 0.0;
                                }
                            }
                        }
                        else { b[i] = 0.0; }
                    }
                    buffer_setdirty(buf); buffer_unlocksamples(buf);
                }
                sc = 1;
            } else sc=11;
        }
    }
    g = 1;
    x->go = g; x->first = fr; x->statecontrol = sc;
zero:
    return;
}

void karma_append(t_karma *x)
{
    if (x->first)
    {
        if((!x->append)&&(!x->looprec)) { x->append = 1; x->loop = x->bframes - 1; x->statecontrol = 9; }
        else
            object_error((t_object *)x,
                         "can't append if already appending, during creating 'initial-loop', or if buffer~ is completely filled");
    }
    else object_error((t_object *)x, "warning! no 'append' registered until at least one loop has been created first");
}

void karma_overdub(t_karma *x, double o) { x->ovdb = o; }

void karma_jump(t_karma *x, double j) { if(x->firstd) { if((x->looprec)&&(!x->rec)) { } else { x->statecontrol = 8; x->jump = j; } }  }

t_max_err karmabuf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
    if(msg == ps_buffer_modified) x->buf_mod = 1;
    return buffer_ref_notify(x->buf, s, msg, sndr, dat);
}

void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags)
{
    x->sr = srate; x->clockgo = 1;
    if (x->bufname != 0)
    {
        if (!x->firstd) karma_setup(x, x->bufname);
        if (x->chans > 1)
        {
            if (x->chans > 2)
            {
                object_method(dsp64, gensym("dsp_add64"), x, karma_qperf, 0, NULL); post("64bit_v1.4quad");
            }
            else
            { object_method(dsp64, gensym("dsp_add64"), x, karma_sperf, 0, NULL); post("64bit_v1.4stereo"); }
        }
        else { object_method(dsp64, gensym("dsp_add64"), x, karma_mperf, 0, NULL); post("64bit_v1.4mono"); }
        if (!x->firstd) { karma_window(x, 1.); x->firstd = 1; } else { karma_window(x, x->extlwin); karma_start(x, x->xstart); }
    } else object_error((t_object *)x, "fails without buffer~ name!");
}

void karma_mperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    double *in1 = ins[0];
    double *in2 = ins[1];
    
    double *outL = outs[0];
    double *outPh = outs[1];
    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osampL, ovdb, ovd, ovdbdif, xstart, xwin;
    double oLprev, oLdif, frac, fad, ramp, snramp, writevaL, coeffL, recinL;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, index0, index1, index2, index3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf); rec = x->rec; recpre = x->recpre; dirt = 0;
    if (!b || x->ob.z_disabled) goto zero; if(rec||recpre) { dirt=1; }
    if (x->buf_mod) { karma_modset(x, buf); x->buf_mod = 0; }
    
    go = x->go;
    statecontrol = x->statecontrol;
    pupdwn = x->pupdwn;
    rupdwn = x->rupdwn;
    rpre = x->rpos;
    rectoo = x->rectoo;
    nchan = x->bchans;
    srscale = x->srscale;
    frames = x->bframes;
    trig = x->trig;
    jnoff = x->jnoff;
    append = x->append;
    diro = x->diro;
    dirp = x->dirp;
    loop = x->loop;
    xwin = x->extlwin;
    looprec = x->looprec;
    start = x->start;
    xstart = x->xstart;
    end = x->end;
    doend = x->doend;
    ovdb = x->ovd;
    ovd = x->ovdb;
    ovdbdif = (ovdb != ovd) ? (ovd - ovdb)/n : 0.0;
    rfad = x->rfad;
    pfad = x->pfad;
    dpos = x->pos;
    pos = trunc(dpos);
    maxpos = x->maxpos;
    wrap = x->wrap;
    jump = x->jump;
    oLprev = x->oLprev;
    oLdif = x->oLdif;
    writevaL = x->writevaL;
    numof = x->numof;
    fad = x->fad;
    ramp = (double)x->ramp;
    snramp = (double)x->snramp;
    curv = x->curv;
    interp = x->interpflag;
    
    switch(statecontrol)                    //all-in-one 'switch' statement to catch and handle all(most) messages
    {
        case 0: break;
        case 1: rec = go = trig = looprec = 1; rfad = rupdwn = pfad = pupdwn = statecontrol = 0; break;    //rec init
        case 2: doend = 3; rec = rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;              //rec rectoo
        case 3: rupdwn = 1; pupdwn = 3; pfad = rfad = statecontrol = 0; break;                           //rec off reg
        case 4: doend = 2; rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;                  //play rectoo
        case 5: trig = 1; statecontrol = 0; break;                                                     //play on reg
        case 6: pfad = rfad = 0; doend = pupdwn = rupdwn = 1; statecontrol = 0; break;                //stop rectoo
        case 7: if(rec) {rfad = 0; rupdwn = 1;} pfad = 0; pupdwn = 1; statecontrol = 0; break;       //stop reg
        case 8: if(rec) {rfad = 0; rupdwn = 2;} pfad = 0; pupdwn = 2; statecontrol = 0; break;      //jump
        case 9: pupdwn = 4; pfad = 0; statecontrol = 0; break;                                     //append
        case 10: rec = looprec = rectoo = 1; fad = 0.0; rfad = rupdwn = statecontrol = 0; break;  //special case append
        case 11: pupdwn = 3; rupdwn = 5; rfad = pfad = statecontrol = 0;                         //rec on reg
    }
    //note:'fad=0.0' triggers switch&ramp(declick play);'rpre=-1' triggers ipoke-interp cuts and accompanies buf~ fades(declick record)
    while (n--)
    {
        recinL = *in1++; speed = *in2++; dir = (speed>0) ? 1 : ((speed<0)?-1:0);
        if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
        //^declick for change of 'dir'ection
        if((rec-recpre)<0)
        { if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }          //samp@rec-off
        else if((rec-recpre)>0)
        { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//samp@rec-on
        
        if (!looprec)
        {
            if (go)
            {
                if (trig)
                {
                    if (doend)                                                                         //calculate end of loop
                    {
                        if (diro>=0)
                        {
                            loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                            end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                            if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                        }
                        else
                        {
                            loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                            dpos = end = start + (xwin*loop);
                            if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                            if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                        }
                        if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; trig=0;
                        append=rectoo=doend=0;
                    }
                    else
                    {                                                                                                //jump/play
                        if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                        if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; }
                        fad = 0.0; trig = 0;
                    }
                }
                else                                                        //jump-based constraints(outside 'window')
                {
                    sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                    
                    if (jnoff)
                    {
                        if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                        if (diro>=0)
                        {
                            if (dpos > loop)
                            {
                                dpos = dpos-loop; fad=0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = loop+dpos; fad=0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                        }
                        else
                        {
                            if (dpos > (frames-1))
                            {
                                dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                            else if (dpos < ((frames-1)-loop))
                            {
                                dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                        }
                    }
                    else                                                                    //regular 'window'/'position' constraints
                    {
                        if (wrap)
                        {
                            if ((dpos > end)&&(dpos < start))
                            {
                                dpos = (dir>=0) ? start : end; fad = 0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                            else if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop+dpos; fad = 0.0;
                                    if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec)
                                    { if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else
                        {
                            if ((dpos > end)||(dpos < start))
                            {
                                dpos = (dir>=0) ? start : end; fad = 0.0;
                                if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                            }
                        }
                    }
                }
                
                pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);              //find samp-indices 4 interp
                
                if (rec) { osampL = LINTRP(frac, b[index1*nchan], b[index2*nchan]); }     //if recording do linear-interp else..
                else                                                                        //..cubic if interpflag(default on)
                {
                    osampL=interp?
                    HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                    :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                }
                
                if (ramp)
                {                                // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                    if(fad<1.0)
                    {
                        if(fad == 0.0) { oLdif = oLprev - osampL; }
                        osampL += snrdease_func(oLdif, fad, curv); fad += 1/snramp;  //<-easing-curv options(implemented by raja)
                    }                                            //"Switch and Ramp" end
                    
                    if (pfad<ramp)
                    {                                                                            //realtime ramps for play on/off
                        osampL = dease_func(osampL, (pupdwn>0), ramp, pfad); pfad++;
                        if (pfad>=ramp)
                        {
                            switch(pupdwn)
                            {
                                case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                            }
                        }
                    }
                }
                else
                {
                    switch(pupdwn)
                    {
                        case 0: break; case 1: pupdwn = go = 0; break;
                        case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                        case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;  //append
                    }
                }
                
            } else { osampL = 0.0; }
            
            oLprev = osampL; *outL++ = osampL; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
            
            //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
            if (rec)                    //(modded to allow for 'window' and 'position' to change on the fly)
            {//raja's razor: simplest answer to everything was "recinL = dease_func(recinL+(b[pos*nchan]*ovdb),rupdwn,ramp,rfad);"..
                //..placed at the beginning/input of ipoke~ to apply appropriate ramps to old-buf+new-input(errythang all-at-once),..
                //..allows ipoke~ to work its sample-specific math/magic accurately through the ducking/ramps even at high speed
                if ((rfad<ramp)&&(ramp>0.0)) recinL = dease_func(recinL + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                else recinL += ((double)b[pos*nchan])*ovdb;
                
                if (rpre<0) { rpre = pos; numof = 0.0; rdif = writevaL = 0.0; }
                
                if (rpre == pos) { writevaL += recinL; numof += 1.0; }
                else
                {
                    if(numof>1.0) { writevaL=writevaL/numof; numof=1.0; }        //linear-averaging for speed < 1x
                    b[rpre*nchan] = writevaL;
                    rdif = (double)(pos - rpre);                                  //linear-interpolation for speed > 1x
                    if(rdif>0){ coeffL=(recinL - writevaL)/rdif; for (i=rpre+1;i<pos;i++) { writevaL += coeffL; b[i*nchan]=writevaL; } }
                    else{ coeffL=(recinL - writevaL)/rdif; for (i=rpre-1;i>pos;i--) { writevaL -= coeffL; b[i*nchan]=writevaL; } }
                    writevaL = recinL;
                }
                rpre = pos; dirt = 1;                    //~ipoke end
            }
            if (ramp)                                               //realtime ramps for record on/off
            {
                if(rfad<ramp)
                {
                    rfad++;
                    if((rupdwn)&&(rfad>=ramp))
                    { if(rupdwn==2) {trig=jnoff=1; rfad=0;} else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                }
            }
            else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if (rupdwn==5) { rec=1; } else rec=0; rupdwn=0; } }
            dirp = dir;
        }
        else
        {                                                           //initial loop creation
            if(go)
            {
                if (trig)
                {
                    if (jnoff)                                              //jump
                    {
                        if (diro>=0) { dpos = jump*maxpos; } else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                        if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                    }
                    else if (append)                                       //append
                    {
                        fad=0.0; trig=0;
                        if(rec)
                        {
                            dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; }
                            rectoo=1; rupdwn=0; rpre=-1;
                        } else {goto apned;}
                        
                    }                                               //trigger start of initial loop creation
                    else
                    {
                        diro = dir; loop = frames - 1; maxpos = dpos = (dir>=0) ? 0.0 : frames-1;
                        rectoo = 1; rpre=-1; fad=0.0; trig=0;
                    }
                }
                else
                { apned:
                    sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                    if (dir == diro)
                    {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                        if (dpos > frames - 1)
                        {
                            dpos = 0.0; rec = append;
                            if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                            doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                        }
                        else if (dpos < 0.0)
                        {
                            dpos = frames-1; rec = append;
                            if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                            doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                        }
                        else { if ( ((diro>=0)&&(maxpos<dpos)) || ((diro<0)&&(maxpos>dpos)) ) maxpos=dpos; }//<-track max write pos
                    }
                    else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                    {
                        if(dpos < 0.0)
                        { dpos=maxpos+dpos; if (ramp){bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                    }
                    else if(dir>=0)
                    {
                        if(dpos > (frames-1))
                        {
                            dpos=maxpos+(dpos-(frames-1));
                            if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                        }
                    }
                }
                
                pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }  //interp ratio
                
                if (ramp)
                {
                    if (pfad<ramp)                                      //realtime ramps for play on/off
                    {
                        pfad++;
                        if(pupdwn)
                        {
                            if (pfad>=ramp)
                            {
                                if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                            }
                        }
                    }
                }
                else
                {
                    if(pupdwn)
                    {
                        if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                        switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                    }
                }
                
            }
            
            osampL = 0.0; oLprev = osampL; *outL++ = osampL; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
            
            //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
            if (rec)               //(modded to assume maximum distance recorded into buffer~ as the total length)
            {
                if ((rfad<ramp)&&(ramp>0.0)) recinL = dease_func(recinL + ((double)b[pos*nchan])*ovdb, rupdwn, ramp, rfad);
                else recinL += ((double)b[pos*nchan])*ovdb;
                
                if (rpre<0) { rpre = pos; numof = 0.0; rdif = writevaL = 0.0; }
                
                if (rpre == pos) { writevaL += recinL; numof += 1.0; }
                else
                {
                    if (numof>1.0) { writevaL = writevaL/numof; numof = 1.0; }          //linear-averaging for speed < 1x
                    b[rpre*nchan] = writevaL;
                    rdif = (double)(pos - rpre);                                          //linear-interp for speed > 1x
                    if (dir!=diro)
                    {
                        if(diro>=0)
                        {
                            if (rdif > 0)
                            {
                                if (rdif>(maxpos*0.5))
                                {
                                    rdif -= maxpos; coeffL = (recinL - writevaL)/rdif;
                                    for (i=(rpre-1);i>=0;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                    for (i=maxpos;i>pos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                }
                                else
                                {
                                    coeffL=(recinL-writevaL)/rdif;
                                    for(i=(rpre+1);i<pos;i++) { writevaL+=coeffL; b[i*nchan]=writevaL; }
                                }
                            }
                            else
                            {
                                if ((-rdif)>(maxpos*0.5))
                                {
                                    rdif += maxpos; coeffL = (recinL - writevaL)/rdif;
                                    for (i=(rpre+1);i<(maxpos+1);i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                    for (i=0;i<pos;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                }
                                else
                                {
                                    coeffL=(recinL-writevaL)/rdif;
                                    for(i=(rpre-1);i>pos;i--) { writevaL-=coeffL; b[i*nchan]=writevaL; }
                                }
                            }
                        }
                        else
                        {
                            if (rdif > 0)
                            {
                                if (rdif>(((frames-1)-(maxpos))*0.5))
                                {
                                    rdif -= ((frames-1)-(maxpos)); coeffL = (recinL - writevaL)/rdif;
                                    for (i=(rpre-1);i>=maxpos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                    for (i=(frames-1);i>pos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                }
                                else
                                {
                                    coeffL=(recinL-writevaL)/rdif;
                                    for(i=(rpre+1);i<pos;i++) { writevaL+=coeffL; b[i*nchan]=writevaL; }
                                }
                            }
                            else
                            {
                                if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                {
                                    rdif += ((frames-1)-(maxpos)); coeffL = (recinL - writevaL)/rdif;
                                    for (i=(rpre+1);i<frames;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                    for (i=maxpos;i<pos;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                }
                                else
                                {
                                    coeffL=(recinL-writevaL)/rdif;
                                    for(i=(rpre-1);i>pos;i--) { writevaL-=coeffL; b[i*nchan]=writevaL; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rdif>0)
                        { coeffL=(recinL-writevaL)/rdif; for(i=(rpre+1);i<pos;i++){ writevaL+=coeffL; b[i*nchan]=writevaL; } }
                        else { coeffL=(recinL-writevaL)/rdif; for(i=(rpre-1);i>pos;i--) { writevaL-=coeffL; b[i*nchan]=writevaL; } }
                    }
                    writevaL = recinL;                           //~ipoke end
                }
                if(ramp)                                                                //realtime ramps for record on/off
                {
                    if (rfad<ramp)
                    {
                        rfad++;
                        if ((rupdwn)&&(rfad>=ramp))
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                            else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break;
                                case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                }
                else
                {
                    if (rupdwn)
                    {
                        if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                        switch (doend)
                        {
                            case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                            case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                        }
                    }
                }
                rpre = pos; dirt = 1;
            }
            dirp = dir;
        }
        if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
    }
    
    if (dirt) { buffer_setdirty(buf); }                                //notify other buf-related objs of write
    buffer_unlocksamples(buf);
    if (x->clockgo) { clock_delay(x->tclock, 0); x->clockgo = 0; }    //list-outlet stuff
    else if ((!go)||(x->rprtime<=0)) { clock_unset(x->tclock); x->clockgo = 1; }
    x->oLprev = oLprev;
    x->oLdif = oLdif;
    x->writevaL = writevaL;
    x->maxpos = maxpos;
    x->numof = numof;
    x->wrap = wrap;
    x->fad = fad;
    x->pos = dpos;
    x->diro = diro;
    x->dirp = dirp;
    x->rpos = rpre;
    x->rectoo = rectoo;
    x->rfad = rfad;
    x->trig = trig;
    x->jnoff = jnoff;
    x->go = go;
    x->rec = rec;
    x->recpre = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn = pupdwn;
    x->rupdwn = rupdwn;
    x->pfad = pfad;
    x->loop = loop;
    x->looprec = looprec;
    x->start = start;
    x->end = end;
    x->ovd = ovdb;
    x->doend = doend;
    x->append = append;
    return;
zero:
    while (n--) { *outL++ = 0.0; *outPh++ = 0.0;} return;
}

void karma_sperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    double *in1 = ins[0];
    double *in2 = ins[1];
    double *in3 = ins[2];
    
    double *outL = outs[0];
    double *outR = outs[1];
    double *outPh = outs[2];
    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osampL, osampR, ovdb, ovd, ovdbdif, xstart, xwin;
    double oLprev, oRprev, oLdif, oRdif, frac, fad, ramp, snramp, writevaL, writevaR, coeffL, coeffR, recinL, recinR;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, index0, index1, index2, index3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    t_buffer_obj *buf = buffer_ref_getobject(x->buf); float *b = buffer_locksamples(buf);
    rec = x->rec; recpre = x->recpre; dirt = 0;
    if (!b || x->ob.z_disabled) goto zero; if (rec||recpre) { dirt=1; }
    if (x->buf_mod) { karma_modset(x, buf); x->buf_mod = 0; }
    
    go = x->go;
    statecontrol = x->statecontrol;
    pupdwn = x->pupdwn;
    rupdwn = x->rupdwn;
    rpre = x->rpos;
    rectoo = x->rectoo;
    nchan = x->bchans;
    srscale = x->srscale;
    frames = x->bframes;
    trig = x->trig;
    jnoff = x->jnoff;
    append = x->append;
    diro = x->diro;
    dirp = x->dirp;
    loop = x->loop;
    xwin = x->extlwin;
    looprec = x->looprec;
    start = x->start;
    xstart = x->xstart;
    end = x->end;
    doend = x->doend;
    ovdb = x->ovd;
    ovd = x->ovdb;
    ovdbdif = (ovdb != ovd) ? (ovd - ovdb)/n : 0.0;
    rfad = x->rfad;
    pfad = x->pfad;
    dpos = x->pos;
    pos = trunc(dpos);
    maxpos = x->maxpos;
    wrap = x->wrap;
    jump = x->jump;
    oLprev = x->oLprev;
    oRprev = x->oRprev;
    oLdif = x->oLdif;
    oRdif = x->oRdif;
    writevaL = x->writevaL;
    writevaR = x->writevaR;
    numof = x->numof;
    fad = x->fad;
    ramp = (double)x->ramp;
    snramp = (double)x->snramp;
    curv = x->curv;
    interp = x->interpflag;
    
    switch(statecontrol)                    //all-in-one 'switch' statement to catch and handle all(most) messages
    {
        case 0: break;
        case 1: rec = go = trig = looprec = 1; rfad = rupdwn = pfad = pupdwn = statecontrol = 0; break;    //rec init
        case 2: doend = 3; rec = rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;              //rec rectoo
        case 3: rupdwn = 1; pupdwn = 3; pfad = rfad = statecontrol = 0; break;                           //rec off reg
        case 4: doend = 2; rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;                  //play rectoo
        case 5: trig = 1; statecontrol = 0; break;                                                     //play on reg
        case 6: pfad = rfad = 0; doend = pupdwn = rupdwn = 1; statecontrol = 0; break;                //stop rectoo
        case 7: if(rec) {rfad = 0; rupdwn = 1;} pfad = 0; pupdwn = 1; statecontrol = 0; break;       //stop reg
        case 8: if(rec) {rfad = 0; rupdwn = 2;} pfad = 0; pupdwn = 2; statecontrol = 0; break;      //jump
        case 9: pupdwn = 4; pfad = 0; statecontrol = 0; break;                                     //append
        case 10: rec = looprec = rectoo = 1; fad = 0.0; rfad = rupdwn = statecontrol = 0; break;  //special case append
        case 11: pupdwn = 3; rupdwn = 5; rfad = pfad = statecontrol = 0;                         //rec on reg
    }
    //note:'fad=0.0' triggers switch&ramp(declicks play);'rpre=-1' triggers ipoke-interp cuts and accompanies buf~ fades(declicks record)
    
    if(nchan>1)
    {
        while (n--)
        {
            recinL = *in1++; recinR = *in2++; speed = *in3++; dir = (speed>0) ? 1 : ((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0)
            { if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }                        //@rec-off
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@rec-on
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                              //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; }
                            fad = 0.0; trig = 0;
                        }
                    }
                    else                                                       //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop + dpos; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                         //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0)?start:end; fad=0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec)
                                        { if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);            //find samp-indices 4 interp
                    
                    if (rec)
                    {                                                                        //if recording do linear-interp else..
                        osampL = LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osampR = LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                    }
                    else
                    {                                                                         //..cubic if interpflag(default on)
                        osampL = interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osampR = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+1], b[(index1*nchan)+1], b[(index2*nchan)+1], b[(index3*nchan)+1])
                        :LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                    }
                    
                    if(ramp)
                    {
                        // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osampL; oRdif = oRprev - osampR; }
                            osampL+=snrdease_func(oLdif, fad, curv);              //<-easing-curvature options (implemented by raja)
                            osampR+=snrdease_func(oRdif, fad, curv);
                            fad+=1/snramp;
                        }                                               //"Switch and Ramp" end
                        
                        if (pfad<ramp)                                        //realtime ramps for play on/off
                        {
                            osampL = dease_func(osampL, (pupdwn>0), ramp, pfad);
                            osampR = dease_func(osampR, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osampL = 0.0; osampR = 0.0; }
                
                oLprev = osampL; oRprev = osampR; *outL++ = osampL; *outR++ = osampR; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recinL = dease_func(recinL + ((double)b[pos*nchan])*ovdb, rupdwn, ramp, rfad);
                        recinR = dease_func(recinR + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                    }
                    else { recinL += ((double)b[pos*nchan])*ovdb; recinR += ((double)b[(pos*nchan)+1])*ovdb; }
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writevaL = writevaR = 0.0; }
                    
                    if (rpre == pos) { writevaL += recinL; writevaR += recinR; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writevaL = writevaL/numof; writevaR = writevaR/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writevaL; b[(rpre*nchan)+1] = writevaR;
                        rdif = (double)(pos - rpre);                                            //interp for speed > 1x
                        if (rdif > 0)
                        {
                            coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                            for(i=rpre+1;i<pos;i++)
                            { writevaL+=coeffL; b[i*nchan]=writevaL; writevaR+=coeffR; b[(i*nchan)+1]=writevaR; }
                        }
                        else
                        {
                            coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                            for(i=rpre-1;i>pos;i--)
                            { writevaL-=coeffL; b[i*nchan]=writevaL; writevaR-=coeffR; b[(i*nchan)+1]=writevaR; }
                        }
                        writevaL = recinL; writevaR = recinR;               //~ipoke end
                    }
                    rpre = pos; dirt = 1;
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++; if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2){trig=jnoff=1; rfad=0;} else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                           //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0)
                            { dpos = jump*maxpos; } else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; }
                                rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apned;}
                        }                                               //trigger start of initial loop creation
                        else { diro = dir; loop = frames - 1; maxpos = dpos = (dir>=0) ? 0.0 : frames-1; rectoo = 1; rpre=-1; fad=0.0; trig=0; }
                    }
                    else
                    { apned:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if (((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos))) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                
                osampL = 0.0; osampR = 0.0;
                oLprev = osampL; oRprev = osampR; *outL++ = osampL; *outR++ = osampR; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                 //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recinL = dease_func(recinL + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                        recinR = dease_func(recinR + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                    }
                    else{ recinL += ((double)b[pos*nchan])*ovdb; recinR += ((double)b[(pos*nchan)+1])*ovdb; }
                    
                    if(rpre<0){ rpre = pos; numof = 0.0; rdif = writevaL = writevaR = 0.0; }
                    
                    if (rpre == pos) { writevaL += recinL; writevaR += recinR; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writevaL = writevaL/numof; writevaR = writevaR/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writevaL; b[(rpre*nchan)+1] = writevaR;
                        rdif = (double)(pos - rpre);                                            //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos; coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre-1);i>=0;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                        for (i=maxpos;i>pos;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                    else
                                    {
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos; coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                        for (i=0;i<pos;i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                    else
                                    {
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos));
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                        for (i=frames-1;i>pos;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                    else
                                    {
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos));
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre+1);i<frames;i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                        for (i=maxpos;i<pos;i++)
                                        { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                    else
                                    {
                                        coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif > 0)
                            {
                                coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                for (i=(rpre+1);i<pos;i++)
                                { writevaL += coeffL; b[i*nchan] = writevaL; writevaR += coeffR; b[(i*nchan)+1] = writevaR; }
                            }
                            else
                            {
                                coeffL = (recinL - writevaL)/rdif; coeffR = (recinR - writevaR)/rdif;
                                for (i=(rpre-1);i>pos;i--)
                                { writevaL -= coeffL; b[i*nchan] = writevaL; writevaR -= coeffR; b[(i*nchan)+1] = writevaR; }
                            }
                        }
                        writevaL = recinL; writevaR = recinR;
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;                                      //~ipoke end
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    else
    {
        while (n--)
        {
            recinL = *in1++; recinR = *in2++; speed = *in3++; dir=(speed>0)?1:((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0){ if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }//@recoff
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@recon
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                           //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; } fad=0.0; trig=0;
                        }
                    }
                    else                                                     //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop+dpos; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                                //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos=(dir>=0)?start:end; fad=0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);         //find samp-indices 4 interp
                    
                    if (rec) { osampL = LINTRP(frac, b[index1*nchan], b[index2*nchan]); }     //if recording do linear-interp else..
                    else                                                                        //..cubic if interpflag(default on)
                    {
                        osampL=interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                    }
                    if (ramp)
                    {                                // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osampL; }
                            osampL+=snrdease_func(oLdif, fad, curv); fad+=1/snramp;     //<-easing-curv options (implemented by raja)
                        }                                                               // "Switch and Ramp" end
                        
                        if (pfad<ramp)
                        {                                                                         //realtime ramps for play on/off
                            osampL = dease_func(osampL, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osampL = 0.0; }
                
                oLprev = osampL;  *outL++ = osampL; oRprev = osampR = 0.0; *outR++=0.0; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                    //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0)) recinL = dease_func(recinL + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                    else recinL += ((double)b[pos*nchan])*ovdb;
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writevaL = 0.0; }
                    
                    if (rpre == pos) { writevaL += recinL; numof += 1.0; }
                    else
                    {
                        if(numof>1.0) { writevaL=writevaL/numof; numof=1.0; }        //linear-averaging for speed < 1x
                        b[rpre*nchan] = writevaL;
                        rdif = (double)(pos - rpre);                           //linear-interpolation for speed > 1x
                        if(rdif>0)
                        { coeffL=(recinL - writevaL)/rdif; for (i=rpre+1;i<pos;i++) { writevaL += coeffL; b[i*nchan]=writevaL; } }
                        else
                        { coeffL=(recinL - writevaL)/rdif; for (i=rpre-1;i>pos;i--) { writevaL -= coeffL; b[i*nchan]=writevaL; } }
                        writevaL = recinL;
                    }
                    rpre = pos; dirt = 1;                    //~ipoke end
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++;
                        if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2) {trig=jnoff=1; rfad=0;} else if(rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if(rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                   //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0) { dpos = jump*maxpos; }
                            else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apnde;}
                        }                                               //trigger start of initial loop creation
                        else
                        {
                            diro = dir; loop = frames - 1;
                            maxpos = dpos = (dir>=0) ? 0.0 : frames-1;
                            rectoo = 1; rpre=-1; fad=0.0; trig=0;
                        }
                    }
                    else
                    { apnde:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if ( ((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos)) ) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                osampL = 0.0;
                oLprev = osampL; *outL++ = osampL; oRprev = osampR = 0.0; *outR++=0.0; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)            //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0)) recinL = dease_func(recinL + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                    else recinL += ((double)b[pos*nchan])*ovdb;
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writevaL = 0.0; }
                    
                    if (rpre == pos) { writevaL += recinL; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writevaL = writevaL/numof; numof = 1.0; }       //linear-average 4 speed < 1x
                        b[rpre*nchan] = writevaL;
                        rdif = (double)(pos - rpre);                                   //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos; coeffL = (recinL - writevaL)/rdif;
                                        for (i=(rpre-1);i>=0;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                        for (i=maxpos;i>pos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                    }
                                    else
                                    {
                                        coeffL=(recinL-writevaL)/rdif;
                                        for(i=(rpre+1);i<pos;i++) { writevaL+=coeffL; b[i*nchan]=writevaL; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos; coeffL = (recinL - writevaL)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                        for (i=0;i<pos;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                    }
                                    else
                                    {
                                        coeffL=(recinL-writevaL)/rdif;
                                        for(i=(rpre-1);i>pos;i--) { writevaL-=coeffL; b[i*nchan]=writevaL; }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos)); coeffL = (recinL - writevaL)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                        for (i=(frames-1);i>pos;i--) { writevaL -= coeffL; b[i*nchan] = writevaL; }
                                    }
                                    else
                                    {
                                        coeffL=(recinL-writevaL)/rdif;
                                        for(i=(rpre+1);i<pos;i++) { writevaL+=coeffL; b[i*nchan]=writevaL; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos)); coeffL = (recinL - writevaL)/rdif;
                                        for (i=(rpre+1);i<frames;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                        for (i=maxpos;i<pos;i++) { writevaL += coeffL; b[i*nchan] = writevaL; }
                                    }
                                    else
                                    {
                                        coeffL=(recinL-writevaL)/rdif;
                                        for(i=(rpre-1);i>pos;i--) { writevaL-=coeffL; b[i*nchan]=writevaL; }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif>0)
                            { coeffL=(recinL-writevaL)/rdif; for(i=(rpre+1);i<pos;i++){ writevaL+=coeffL; b[i*nchan]=writevaL; } }
                            else
                            { coeffL=(recinL-writevaL)/rdif; for(i=(rpre-1);i>pos;i--){ writevaL-=coeffL; b[i*nchan]=writevaL; } }
                        }
                        writevaL = recinL;                      //~ipoke end
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    
    if (dirt) { buffer_setdirty(buf); }                                //notify other buf-related objs of write
    buffer_unlocksamples(buf);
    if (x->clockgo) { clock_delay(x->tclock, 0); x->clockgo = 0; }    //list-outlet stuff
    else if ((!go)||(x->rprtime<=0)) { clock_unset(x->tclock); x->clockgo = 1; }
    x->oLprev = oLprev;
    x->oRprev = oRprev;
    x->oLdif = oLdif;
    x->oRdif = oRdif;
    x->writevaL = writevaL;
    x->writevaR = writevaR;
    x->maxpos = maxpos;
    x->numof = numof;
    x->wrap = wrap;
    x->fad = fad;
    x->pos = dpos;
    x->diro = diro;
    x->dirp = dirp;
    x->rpos = rpre;
    x->rectoo = rectoo;
    x->rfad = rfad;
    x->trig = trig;
    x->jnoff = jnoff;
    x->go = go;
    x->rec = rec;
    x->recpre = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn = pupdwn;
    x->rupdwn = rupdwn;
    x->pfad = pfad;
    x->loop = loop;
    x->looprec = looprec;
    x->start = start;
    x->end = end;
    x->ovd = ovdb;
    x->doend = doend;
    x->append = append;
    return;
zero:
    while (n--) {*outL++ = 0.0; *outR++ = 0.0; *outPh++ = 0.0;} return;
}

void karma_qperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    double *in1 = ins[0];
    double *in2 = ins[1];
    double *in3 = ins[2];
    double *in4 = ins[3];
    double *in5 = ins[4];
    
    double *out1 = outs[1];
    double *out2 = outs[2];
    double *out3 = outs[3];
    double *out4 = outs[4];
    double *outPh = outs[5];
    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osamp1, osamp2, osamp3, osamp4, ovdb, ovd, ovdbdif, xstart, xwin;
    double oLprev, oRprev, oLdif, oRdif, o3prev, o4prev, o3dif, o4dif, frac, fad, ramp, snramp;
    double writeva1, writeva2, writeva3, writeva4, coeff1, coeff2, coeff3, coeff4, recin1, recin2, recin3, recin4;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, index0, index1, index2, index3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    t_buffer_obj *buf = buffer_ref_getobject(x->buf); float *b = buffer_locksamples(buf);
    rec = x->rec; recpre = x->recpre; dirt = 0;
    if (!b || x->ob.z_disabled) goto zero; if (rec||recpre) { dirt=1; }
    if (x->buf_mod) { karma_modset(x, buf); x->buf_mod = 0; }
    
    go = x->go;
    statecontrol = x->statecontrol;
    pupdwn = x->pupdwn;
    rupdwn = x->rupdwn;
    rpre = x->rpos;
    rectoo = x->rectoo;
    nchan = x->bchans;
    srscale = x->srscale;
    frames = x->bframes;
    trig = x->trig;
    jnoff = x->jnoff;
    append = x->append;
    diro = x->diro;
    dirp = x->dirp;
    loop = x->loop;
    xwin = x->extlwin;
    looprec = x->looprec;
    start = x->start;
    xstart = x->xstart;
    end = x->end;
    doend = x->doend;
    ovdb = x->ovd;
    ovd = x->ovdb;
    ovdbdif = (ovdb != ovd) ? (ovd - ovdb)/n : 0.0;
    rfad = x->rfad;
    pfad = x->pfad;
    dpos = x->pos;
    pos = trunc(dpos);
    maxpos = x->maxpos;
    wrap = x->wrap;
    jump = x->jump;
    oLprev = x->oLprev;
    oLprev = x->oRprev;
    o3prev = x->o3prev;
    oRprev = x->o4prev;
    oLdif = x->oLdif;
    oRdif = x->oRdif;
    o3dif = x->o3dif;
    o4dif = x->o4dif;
    writeva1 = x->writevaL;
    writeva2 = x->writevaR;
    writeva3 = x->writeva3;
    writeva4 = x->writeva4;
    numof = x->numof;
    fad = x->fad;
    ramp = (double)x->ramp;
    snramp = (double)x->snramp;
    curv = x->curv;
    interp = x->interpflag;
    
    switch(statecontrol)                    //all-in-one 'switch' statement to catch and handle all(most) messages
    {
        case 0: break;
        case 1: rec = go = trig = looprec = 1; rfad = rupdwn = pfad = pupdwn = statecontrol = 0; break;    //rec init
        case 2: doend = 3; rec = rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;              //rec rectoo
        case 3: rupdwn = 1; pupdwn = 3; pfad = rfad = statecontrol = 0; break;                           //rec off reg
        case 4: doend = 2; rupdwn = pupdwn = 1; pfad = rfad = statecontrol = 0; break;                  //play rectoo
        case 5: trig = 1; statecontrol = 0; break;                                                     //play on reg
        case 6: pfad = rfad = 0; doend = pupdwn = rupdwn = 1; statecontrol = 0; break;                //stop rectoo
        case 7: if(rec) {rfad = 0; rupdwn = 1;} pfad = 0; pupdwn = 1; statecontrol = 0; break;       //stop reg
        case 8: if(rec) {rfad = 0; rupdwn = 2;} pfad = 0; pupdwn = 2; statecontrol = 0; break;      //jump
        case 9: pupdwn = 4; pfad = 0; statecontrol = 0; break;                                     //append
        case 10: rec = looprec = rectoo = 1; fad = 0.0; rfad = rupdwn = statecontrol = 0; break;  //special case append
        case 11: pupdwn = 3; rupdwn = 5; rfad = pfad = statecontrol = 0;                         //rec on reg
    }
    //note:'fad=0.0' triggers switch&ramp(declicks play);'rpre=-1' triggers ipoke-interp cuts and accompanies buf~ fades(declicks record)
    if (nchan >= 4)
    {
        while (n--)
        {
            recin1 = *in1++; recin2 = *in2++; recin3 = *in3++; recin4 = *in4++; speed = *in5++; dir = (speed>0) ? 1 : ((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0)
            { if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }                        //@rec-off
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@rec-on
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                              //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; }
                            fad = 0.0; trig = 0;
                        }
                    }
                    else                                                       //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop + dpos; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                         //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0)?start:end; fad=0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec)
                                        { if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);            //find samp-indices 4 interp
                    
                    if (rec)
                    {                                                                        //if recording do linear-interp else..
                        osamp1 = LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                        osamp3 = LINTRP(frac, b[(index1*nchan)+2], b[(index2*nchan)+2]);
                        osamp4 = LINTRP(frac, b[(index1*nchan)+3], b[(index2*nchan)+3]);
                    }
                    else
                    {                                                                         //..cubic if interpflag(default on)
                        osamp1 = interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+1], b[(index1*nchan)+1], b[(index2*nchan)+1], b[(index3*nchan)+1])
                        :LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                        osamp3 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+2], b[(index1*nchan)+2], b[(index2*nchan)+2], b[(index3*nchan)+2])
                        :LINTRP(frac, b[(index1*nchan)+2], b[(index2*nchan)+2]);
                        osamp4 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+3], b[(index1*nchan)+3], b[(index2*nchan)+3], b[(index3*nchan)+3])
                        :LINTRP(frac, b[(index1*nchan)+3], b[(index2*nchan)+3]);
                    }
                    
                    if(ramp)
                    {
                        // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osamp1; oRdif = oRprev - osamp2; o3dif=o3prev-osamp3; o4dif = o4prev - osamp4; }
                            osamp1+=snrdease_func(oLdif, fad, curv);              //<-easing-curvature options (implemented by raja)
                            osamp2+=snrdease_func(oRdif, fad, curv);
                            osamp3+=snrdease_func(o3dif, fad, curv);
                            osamp4+=snrdease_func(o4dif, fad, curv);
                            fad+=1/snramp;
                        }                                               //"Switch and Ramp" end
                        
                        if (pfad<ramp)                                        //realtime ramps for play on/off
                        {
                            osamp1 = dease_func(osamp1, (pupdwn>0), ramp, pfad);
                            osamp2 = dease_func(osamp2, (pupdwn>0), ramp, pfad);
                            osamp3 = dease_func(osamp3, (pupdwn>0), ramp, pfad);
                            osamp4 = dease_func(osamp4, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osamp1 = 0.0; osamp2 = 0.0; osamp3 = 0.0; osamp4 = 0.0; }
                
                oLprev = osamp1; oRprev = osamp2; o3prev = osamp3; o4prev = osamp4;
                *out1++ = osamp1; *out2++ = osamp2; *out3++ = osamp3; *out4++ = osamp4; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + ((double)b[pos*nchan])*ovdb, rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                        recin3 = dease_func(recin3 + (((double)b[(pos*nchan)+2])*ovdb), rupdwn, ramp, rfad);
                        recin4 = dease_func(recin4 + (((double)b[(pos*nchan)+3])*ovdb), rupdwn, ramp, rfad);
                    }
                    else
                    {
                        recin1 += ((double)b[pos*nchan])*ovdb; recin2 += ((double)b[(pos*nchan)+1])*ovdb;
                        recin3 += ((double)b[pos*nchan+2])*ovdb; recin4 += ((double)b[(pos*nchan)+3])*ovdb;
                    }
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = writeva3 = writeva4 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; writeva3 += recin3; writeva4 += recin4; numof += 1.0; }
                    else
                    {
                        if (numof>1.0)
                        {
                            writeva1 = writeva1/numof; writeva2 = writeva2/numof;
                            writeva3 = writeva3/numof; writeva4 = writeva4/numof; numof = 1.0;
                        }//average 4 speed < 1x
                        
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2; b[(rpre*nchan)+2] = writeva3; b[(rpre*nchan)+3] = writeva4;
                        rdif = (double)(pos - rpre);                                            //interp for speed > 1x
                        if (rdif > 0)
                        {
                            coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                            coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                            for(i=rpre+1;i<pos;i++)
                            {
                                writeva1+=coeff1; b[i*nchan]=writeva1; writeva2+=coeff2; b[(i*nchan)+1]=writeva2;
                                writeva3+=coeff3; b[(i*nchan)+2]=writeva3; writeva4+=coeff4; b[(i*nchan)+3]=writeva4;
                            }
                        }
                        else
                        {
                            coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                            coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                            for(i=rpre-1;i>pos;i--)
                            {
                                writeva1-=coeff1; b[i*nchan]=writeva1; writeva2-=coeff2; b[(i*nchan)+1]=writeva2;
                                writeva3-=coeff3; b[(i*nchan)+2]=writeva3; writeva4-=coeff4; b[(i*nchan)+3]=writeva4;
                            }
                        }
                        writeva1 = recin1; writeva2 = recin2; writeva3 = recin3; writeva4 = recin4;               //~ipoke end
                    }
                    rpre = pos; dirt = 1;
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++; if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2){trig=jnoff=1; rfad=0;} else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                           //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0)
                            { dpos = jump*maxpos; } else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; }
                                rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apned;}
                        }                                               //trigger start of initial loop creation
                        else { diro = dir; loop = frames - 1; maxpos = dpos = (dir>=0) ? 0.0 : frames-1; rectoo = 1; rpre=-1; fad=0.0; trig=0; }
                    }
                    else
                    { apned:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if (((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos))) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                
                osamp1 = 0.0; osamp2 = 0.0; osamp3 = 0.0; osamp4 = 0.0;
                oLprev = osamp1; oRprev = osamp2; o3prev = osamp3; o4prev = osamp4;
                *out1++ = osamp1; *out2++ = osamp2; *out3++ = osamp3; *out4++ = osamp4; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                 //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                        recin3 = dease_func(recin3 + (((double)b[(pos*nchan)+2])*ovdb), rupdwn, ramp, rfad);
                        recin4 = dease_func(recin4 + (((double)b[(pos*nchan)+3])*ovdb), rupdwn, ramp, rfad);
                    }
                    else
                    {
                        recin1 += ((double)b[pos*nchan])*ovdb; recin2 += ((double)b[(pos*nchan)+1])*ovdb;
                        recin3 += ((double)b[(pos*nchan)+2])*ovdb; recin4 += ((double)b[(pos*nchan)+3])*ovdb;
                    }
                    
                    if(rpre<0){ rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = writeva3 = writeva4 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; writeva3 += recin3; writeva4 += recin4; numof += 1.0; }
                    else
                    {
                        if (numof>1.0)
                        {
                            writeva1 = writeva1/numof; writeva2 = writeva2/numof;
                            writeva2 = writeva2/numof; writeva3 = writeva3/numof; numof = 1.0;
                        }//average 4 speed < 1x
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2; b[(rpre*nchan)+2] = writeva3; b[(rpre*nchan)+3] = writeva4;
                        rdif = (double)(pos - rpre);                                            //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre-1);i>=0;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                        for (i=maxpos;i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                        for (i=0;i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                        for (i=frames-1;i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre+1);i<frames;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                        for (i=maxpos;i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                for (i=(rpre+1);i<pos;i++)
                                {
                                    writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                    writeva3 += coeff3; b[(i*nchan)+2] = writeva3; writeva4 += coeff4; b[(i*nchan)+3] = writeva4;
                                }
                            }
                            else
                            {
                                coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                coeff3 = (recin3 - writeva3)/rdif; coeff4 = (recin4 - writeva4)/rdif;
                                for (i=(rpre-1);i>pos;i--)
                                {
                                    writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                    writeva3 -= coeff3; b[(i*nchan)+2] = writeva3; writeva4 -= coeff4; b[(i*nchan)+3] = writeva4;
                                }
                            }
                        }
                        writeva1 = recin1; writeva2 = recin2; writeva3 = recin3; writeva4 = recin4;
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;                                      //~ipoke end
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    else if(nchan == 3)
    {
        while (n--)
        {
            recin1 = *in1++; recin2 = *in2++; recin3 = *in3++; recin4 = *in4++; speed = *in5++; dir = (speed>0) ? 1 : ((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0)
            { if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }                        //@rec-off
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@rec-on
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                              //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; }
                            fad = 0.0; trig = 0;
                        }
                    }
                    else                                                       //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop + dpos; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                         //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0)?start:end; fad=0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec)
                                        { if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);            //find samp-indices 4 interp
                    
                    if (rec)
                    {                                                                        //if recording do linear-interp else..
                        osamp1 = LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                        osamp3 = LINTRP(frac, b[(index1*nchan)+2], b[(index2*nchan)+2]);
                    }
                    else
                    {                                                                         //..cubic if interpflag(default on)
                        osamp1 = interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+1], b[(index1*nchan)+1], b[(index2*nchan)+1], b[(index3*nchan)+1])
                        :LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                        osamp3 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+2], b[(index1*nchan)+2], b[(index2*nchan)+2], b[(index3*nchan)+2])
                        :LINTRP(frac, b[(index1*nchan)+2], b[(index2*nchan)+2]);
                    }
                    
                    if(ramp)
                    {
                        // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osamp1; oRdif = oRprev - osamp2; o3dif=o3prev-osamp3; o4dif = o4prev - osamp4; }
                            osamp1+=snrdease_func(oLdif, fad, curv);              //<-easing-curvature options (implemented by raja)
                            osamp2+=snrdease_func(oRdif, fad, curv);
                            osamp3+=snrdease_func(o3dif, fad, curv);
                            fad+=1/snramp;
                        }                                               //"Switch and Ramp" end
                        
                        if (pfad<ramp)                                        //realtime ramps for play on/off
                        {
                            osamp1 = dease_func(osamp1, (pupdwn>0), ramp, pfad);
                            osamp2 = dease_func(osamp2, (pupdwn>0), ramp, pfad);
                            osamp3 = dease_func(osamp3, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osamp1 = 0.0; osamp2 = 0.0; osamp3 = 0.0; }
                
                oLprev = osamp1; oRprev = osamp2; o3prev = osamp3;
                *out1++ = osamp1; *out2++ = osamp2; *out3++ = osamp3; o4prev = osamp4 = 0.0; *out4++ = 0.0;
                *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + ((double)b[pos*nchan])*ovdb, rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                        recin3 = dease_func(recin3 + (((double)b[(pos*nchan)+2])*ovdb), rupdwn, ramp, rfad);
                    }
                    else
                    {
                        recin1 += ((double)b[pos*nchan])*ovdb;
                        recin2 += ((double)b[(pos*nchan)+1])*ovdb;
                        recin3 += ((double)b[(pos*nchan)+2])*ovdb;
                    }
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = writeva3 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; writeva3 += recin3; numof += 1.0; }
                    else
                    {
                        if (numof>1.0)
                        { writeva1 = writeva1/numof; writeva2 = writeva2/numof; writeva3 = writeva3/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2; b[(rpre*nchan)+2] = writeva3;
                        rdif = (double)(pos - rpre);                                            //interp for speed > 1x
                        if (rdif > 0)
                        {
                            coeff1 = (recin1 - writeva1)/rdif;
                            coeff2 = (recin2 - writeva2)/rdif;
                            coeff3 = (recin3 - writeva3)/rdif;
                            for(i=rpre+1;i<pos;i++)
                            {
                                writeva1+=coeff1; b[i*nchan]=writeva1;
                                writeva2+=coeff2; b[(i*nchan)+1]=writeva2;
                                writeva3+=coeff3; b[(i*nchan)+2]=writeva3;
                            }
                        }
                        else
                        {
                            coeff1 = (recin1 - writeva1)/rdif;
                            coeff2 = (recin2 - writeva2)/rdif;
                            coeff3 = (recin3 - writeva3)/rdif;
                            for(i=rpre-1;i>pos;i--)
                            {
                                writeva1-=coeff1; b[i*nchan]=writeva1;
                                writeva2-=coeff2; b[(i*nchan)+1]=writeva2;
                                writeva3-=coeff3; b[(i*nchan)+2]=writeva3;
                            }
                        }
                        writeva1 = recin1; writeva2 = recin2; writeva3 = recin3;              //~ipoke end
                    }
                    rpre = pos; dirt = 1;
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++; if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2){trig=jnoff=1; rfad=0;} else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                           //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0)
                            { dpos = jump*maxpos; } else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; }
                                rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apden;}
                        }                                               //trigger start of initial loop creation
                        else { diro = dir; loop = frames - 1; maxpos = dpos = (dir>=0) ? 0.0 : frames-1; rectoo = 1; rpre=-1; fad=0.0; trig=0; }
                    }
                    else
                    { apden:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if (((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos))) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                
                osamp1 = 0.0; osamp2 = 0.0; osamp3 = 0.0;
                oLprev = osamp1; oRprev = osamp2; o3prev = osamp3; *out1++ = osamp1; *out2++ = osamp2; *out3++ = osamp3; o4prev = osamp4 = 0.0;
                *out4++ = 0.0; *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                 //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                        recin3 = dease_func(recin3 + (((double)b[(pos*nchan)+2])*ovdb), rupdwn, ramp, rfad);
                    }
                    else
                    { recin1 += ((double)b[pos*nchan])*ovdb; recin2 += ((double)b[(pos*nchan)+1])*ovdb; recin3 += ((double)b[(pos*nchan)+2])*ovdb; }
                    
                    if(rpre<0){ rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = writeva3 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; writeva3 += recin3; numof += 1.0; }
                    else
                    {
                        if (numof>1.0)
                        { writeva1 = writeva1/numof; writeva2 = writeva2/numof; writeva3 = writeva3/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2; b[(rpre*nchan)+2] = writeva3;
                        rdif = (double)(pos - rpre);                                            //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre-1);i>=0;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                        for (i=maxpos;i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                        for (i=0;i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                        for (i=frames-1;i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre+1);i<frames;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                        for (i=maxpos;i<pos;i++)
                                        {
                                            writeva1 += coeff1; b[i*nchan] = writeva1;
                                            writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif;
                                        coeff2 = (recin2 - writeva2)/rdif;
                                        coeff3 = (recin3 - writeva3)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        {
                                            writeva1 -= coeff1; b[i*nchan] = writeva1;
                                            writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                            writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeva1)/rdif;
                                coeff2 = (recin2 - writeva2)/rdif;
                                coeff3 = (recin3 - writeva3)/rdif;
                                for (i=(rpre+1);i<pos;i++)
                                {
                                    writeva1 += coeff1; b[i*nchan] = writeva1;
                                    writeva2 += coeff2; b[(i*nchan)+1] = writeva2;
                                    writeva3 += coeff3; b[(i*nchan)+2] = writeva3;
                                }
                            }
                            else
                            {
                                coeff1 = (recin1 - writeva1)/rdif;
                                coeff2 = (recin2 - writeva2)/rdif;
                                coeff3 = (recin3 - writeva3)/rdif;
                                for (i=(rpre-1);i>pos;i--)
                                {
                                    writeva1 -= coeff1; b[i*nchan] = writeva1;
                                    writeva2 -= coeff2; b[(i*nchan)+1] = writeva2;
                                    writeva3 -= coeff3; b[(i*nchan)+2] = writeva3;
                                }
                            }
                        }
                        writeva1 = recin1; writeva2 = recin2; writeva3 = recin3;
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;                                      //~ipoke end
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    else if(nchan==2)
    {
        while (n--)
        {
            recin1 = *in1++; recin2 = *in2++; recin3 = *in3++; recin4 = *in4++; speed = *in5++; dir = (speed>0) ? 1 : ((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0)
            { if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }                        //@rec-off
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@rec-on
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                              //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; }
                            fad = 0.0; trig = 0;
                        }
                    }
                    else                                                       //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop + dpos; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                         //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0)?start:end; fad=0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad=0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec)
                                        { if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);            //find samp-indices 4 interp
                    
                    if (rec)
                    {                                                                        //if recording do linear-interp else..
                        osamp1 = LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                    }
                    else
                    {                                                                         //..cubic if interpflag(default on)
                        osamp1 = interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                        osamp2 = interp?
                        HRMCBINTRP(frac, b[(index0*nchan)+1], b[(index1*nchan)+1], b[(index2*nchan)+1], b[(index3*nchan)+1])
                        :LINTRP(frac, b[(index1*nchan)+1], b[(index2*nchan)+1]);
                    }
                    
                    if(ramp)
                    {
                        // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osamp1; oRdif = oRprev - osamp2; }
                            osamp1+=snrdease_func(oLdif, fad, curv);              //<-easing-curvature options (implemented by raja)
                            osamp2+=snrdease_func(oRdif, fad, curv);
                            fad+=1/snramp;
                        }                                               //"Switch and Ramp" end
                        
                        if (pfad<ramp)                                        //realtime ramps for play on/off
                        {
                            osamp1 = dease_func(osamp1, (pupdwn>0), ramp, pfad);
                            osamp2 = dease_func(osamp2, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osamp1 = 0.0; osamp1 = 0.0; }
                
                oLprev = osamp1; oRprev = osamp2; *out1++ = osamp1; *out2++ = osamp2;
                o3prev = osamp3 = 0.0; *out3++ = 0.0; o4prev = osamp4 = 0.0; *out4++ = 0.0;
                *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + ((double)b[pos*nchan])*ovdb, rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                    }
                    else { recin1 += ((double)b[pos*nchan])*ovdb; recin2 += ((double)b[(pos*nchan)+1])*ovdb; }
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writeva1 = writeva1/numof; writeva2 = writeva2/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2;
                        rdif = (double)(pos - rpre);                                            //interp for speed > 1x
                        if (rdif > 0)
                        {
                            coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                            for(i=rpre+1;i<pos;i++)
                            { writeva1+=coeff1; b[i*nchan]=writeva1; writeva2+=coeff2; b[(i*nchan)+1]=writeva2; }
                        }
                        else
                        {
                            coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                            for(i=rpre-1;i>pos;i--)
                            { writeva1-=coeff1; b[i*nchan]=writeva1; writeva2-=coeff2; b[(i*nchan)+1]=writeva2; }
                        }
                        writeva1 = recin1; writeva2 = recin2;               //~ipoke end
                    }
                    rpre = pos; dirt = 1;
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++; if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2){trig=jnoff=1; rfad=0;} else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if (rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                           //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0)
                            { dpos = jump*maxpos; } else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; }
                                rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apdne;}
                        }                                               //trigger start of initial loop creation
                        else { diro = dir; loop = frames - 1; maxpos = dpos = (dir>=0) ? 0.0 : frames-1; rectoo = 1; rpre=-1; fad=0.0; trig=0; }
                    }
                    else
                    { apdne:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if (((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos))) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                
                osamp1 = 0.0; osamp2 = 0.0;
                oLprev = osamp1; oRprev = osamp2; *out1++ = osamp1; *out2++ = osamp2;
                o3prev = osamp3 = 0.0; *out3++ = 0.0; o4prev = osamp4 = 0.0; *out4++ = 0.0;
                *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                 //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0))
                    {
                        recin1 = dease_func(recin1 + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                        recin2 = dease_func(recin2 + (((double)b[(pos*nchan)+1])*ovdb), rupdwn, ramp, rfad);
                    }
                    else{ recin1 += ((double)b[pos*nchan])*ovdb; recin2 += ((double)b[(pos*nchan)+1])*ovdb; }
                    
                    if(rpre<0){ rpre = pos; numof = 0.0; rdif = writeva1 = writeva2 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; writeva2 += recin2; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writeva1 = writeva1/numof; writeva2 = writeva2/numof; numof = 1.0; }//average 4 speed < 1x
                        b[rpre*nchan] = writeva1; b[(rpre*nchan)+1] = writeva2;
                        rdif = (double)(pos - rpre);                                            //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos; coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre-1);i>=0;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                        for (i=maxpos;i>pos;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos; coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                        for (i=0;i<pos;i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                        for (i=frames-1;i>pos;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin1 - writeva2)/rdif;
                                        for (i=(rpre+1);i<pos;i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos));
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre+1);i<frames;i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                        for (i=maxpos;i<pos;i++)
                                        { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                    else
                                    {
                                        coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                        for (i=(rpre-1);i>pos;i--)
                                        { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeva1)/rdif; coeff2 = (recin2 - writeva2)/rdif;
                                for (i=(rpre+1);i<pos;i++)
                                { writeva1 += coeff1; b[i*nchan] = writeva1; writeva2 += coeff2; b[(i*nchan)+1] = writeva2; }
                            }
                            else
                            {
                                coeff1 = (recin1 - writeva1)/rdif; coeff1 = (recin1 - writeva1)/rdif;
                                for (i=(rpre-1);i>pos;i--)
                                { writeva1 -= coeff1; b[i*nchan] = writeva1; writeva2 -= coeff2; b[(i*nchan)+1] = writeva2; }
                            }
                        }
                        writeva1 = recin1; writeva2 = recin2;
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;                                      //~ipoke end
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    else
    {
        while (n--)
        {
            recin1 = *in1++; recin2 = *in2++; recin3 = *in3++; recin4 = *in4++; speed = *in5++; dir=(speed>0)?1:((speed<0)?-1:0);
            if(dirp!=dir){ if(rec&&ramp){ bufeasoff(frames-1, b, nchan, rpre, -dir, ramp); rfad=rupdwn=0; rpre=-1; } fad=0.0; };
            //^declick for change of 'dir'ection
            if((rec-recpre)<0){ if(ramp)bufeasoff(frames-1, b, nchan, rpre, dir, ramp); rpre=-1; dirt=1; }//@recoff
            else if((rec-recpre)>0)
            { rfad=rupdwn=0; if(speed<1.0) fad=0.0; if (ramp)bufeasoff(frames-1, b, nchan, dpos, -dir, ramp); } recpre=rec;//@recon
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)                                                                          //calculate end of loop
                        {
                            if (diro>=0)
                            {
                                loop = CLAMP(maxpos, 4096, frames-1); dpos = start = xstart*loop;
                                end = start + (xwin*loop); if (end > loop) { end = end-(loop+1); wrap = 1; } else wrap = 0;
                                if(dir<0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            else
                            {
                                loop = CLAMP((frames-1) - maxpos, 4096, frames-1); start = ((frames-1)-loop) + (xstart * loop);
                                dpos = end = start + (xwin*loop);
                                if (end > (frames-1)) { end = ((frames-1)-loop)+(end-frames); wrap = 1; } else wrap = 0; dpos = end;
                                if(dir>0) { if(ramp) bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); }
                            }
                            if (ramp) bufeasoff(frames-1, b, nchan, maxpos, -dir, ramp); rpre=-1; fad=0.0; append=rectoo=doend=trig=0;
                        }
                        else
                        {                                                                                           //jump/play
                            if (jnoff) { dpos=(diro>=0)?jump*loop:((frames-1)-loop)+(jump*loop); } else dpos=(dir<0)?end:start;
                            if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rpre=-1; rupdwn=0; } fad=0.0; trig=0;
                        }
                    }
                    else                                                     //jump-based constraints(outside 'window')
                    {
                        sprale=speed*srscale; if(rec) sprale=(fabs(sprale)>(loop/1024))?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        
                        if (jnoff)
                        {
                            if(wrap) { if((dpos<end)||(dpos>start)) jnoff=0; } else { if((dpos<end)&&(dpos>start)) jnoff=0; }
                            if (diro>=0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos-loop; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop+dpos; fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                            else
                            {
                                if (dpos > (frames-1))
                                {
                                    dpos = ((frames-1)-loop)+(dpos-(frames-1)); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (dpos < ((frames-1)-loop))
                                {
                                    dpos = (frames-1)-(((frames-1)-loop)-dpos); fad = 0.0;
                                    if(rec) { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                        else                                                                //regular 'window'/'position' constraints
                        {
                            if (wrap)
                            {
                                if ((dpos > end)&&(dpos < start))
                                {
                                    dpos = (dir>=0) ? start : end; fad = 0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                                else if (diro>=0)
                                {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos-loop; fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, loop, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop+dpos; fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } }
                                }
                                else
                                {
                                    if (dpos < ((frames-1)-loop))
                                    {
                                        dpos = (frames-1) - (((frames-1)-loop)-dpos); fad = 0.0;
                                        if(rec)
                                        {
                                            if(ramp){ bufeasoff(frames-1, b, nchan, ((frames-1)-loop), -dir, ramp); rfad=0; }
                                            rupdwn=0; rpre=-1;
                                        }
                                    }
                                    else if (dpos > (frames-1))
                                    {
                                        dpos = ((frames-1)-loop) + (dpos - (frames-1)); fad = 0.0;
                                        if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                    }
                                }
                            }
                            else
                            {
                                if ((dpos > end)||(dpos < start))
                                {
                                    dpos=(dir>=0)?start:end; fad=0.0;
                                    if(rec){ if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; }
                                }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interp ratio
                    interp_index(pos,&index0,&index1,&index2,&index3,dir,diro,loop,frames-1);         //find samp-indices 4 interp
                    
                    if (rec) { osamp1 = LINTRP(frac, b[index1*nchan], b[index2*nchan]); }     //if recording do linear-interp else..
                    else                                                                        //..cubic if interpflag(default on)
                    {
                        osamp1=interp?
                        HRMCBINTRP(frac, b[index0*nchan], b[index1*nchan], b[index2*nchan], b[index3*nchan])
                        :LINTRP(frac, b[index1*nchan], b[index2*nchan]);
                    }
                    if (ramp)
                    {                                // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if(fad<1.0)
                        {
                            if(fad==0.0) { oLdif=oLprev-osamp1; }
                            osamp1+=snrdease_func(oLdif, fad, curv); fad+=1/snramp;     //<-easing-curv options (implemented by raja)
                        }                                                               // "Switch and Ramp" end
                        
                        if (pfad<ramp)
                        {                                                                         //realtime ramps for play on/off
                            osamp1 = dease_func(osamp1, (pupdwn>0), ramp, pfad); pfad++;
                            if (pfad>=ramp)
                            {
                                switch(pupdwn)
                                {
                                    case 0: break; case 1: pupdwn = go = 0; break;      //rec rectoo //play rectoo //stop rectoo/reg
                                    case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = pfad = 0; break;   //jump //rec off reg
                                    case 4: go = trig = looprec = 1; fad = 0.0; pfad = pupdwn = 0; break;        //append
                                }
                            }
                        }
                    }
                    else
                    {
                        switch(pupdwn)
                        {
                            case 0: break; case 1: pupdwn = go = 0; break;
                            case 2: if (!rec) { trig = jnoff = 1; } case 3: pupdwn = 0; break;    //jump //rec off reg
                            case 4: go = trig = looprec = 1; pupdwn = 0; break;  //append
                        }
                    }
                    
                } else { osamp1 = 0.0; }
                
                oLprev = osamp1;  *out1++ = osamp1; oRprev = osamp2 = 0.0; *out2++=0.0;
                o3prev = osamp3 = 0.0; *out3++=0.0; o4prev = osamp4 = 0.0; *out4++=0.0;
                *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)                    //(modded to allow for 'window' and 'position' to change on the fly)
                {
                    if ((rfad<ramp)&&(ramp>0.0)) recin1 = dease_func(recin1 + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                    else recin1 += ((double)b[pos*nchan])*ovdb;
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writeva1 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; numof += 1.0; }
                    else
                    {
                        if(numof>1.0) { writeva1=writeva1/numof; numof=1.0; }        //linear-averaging for speed < 1x
                        b[rpre*nchan] = writeva1;
                        rdif = (double)(pos - rpre);                           //linear-interpolation for speed > 1x
                        if(rdif>0)
                        { coeff1=(recin1 - writeva1)/rdif; for (i=rpre+1;i<pos;i++) { writeva1 += coeff1; b[i*nchan]=writeva1; } }
                        else
                        { coeff1=(recin1 - writeva1)/rdif; for (i=rpre-1;i>pos;i--) { writeva1 -= coeff1; b[i*nchan]=writeva1; } }
                        writeva1 = recin1;
                    }
                    rpre = pos; dirt = 1;                    //~ipoke end
                }
                if (ramp)                                               //realtime ramps for record on/off
                {
                    if(rfad<ramp)
                    {
                        rfad++;
                        if((rupdwn)&&(rfad>=ramp))
                        { if(rupdwn==2) {trig=jnoff=1; rfad=0;} else if(rupdwn==5) {rec=1;} else rec=0; rupdwn=0; }
                    }
                }
                else { if(rupdwn) { if(rupdwn==2) { trig=jnoff=1; } else if(rupdwn==5) {rec=1;} else rec=0; rupdwn=0; } }
                dirp = dir;
            }
            else
            {                                                   //initial loop creation
                if(go)
                {
                    if (trig)
                    {
                        if (jnoff)                                              //jump
                        {
                            if (diro>=0) { dpos = jump*maxpos; }
                            else { dpos = (frames-1)-(((frames-1)-maxpos)*jump); } jnoff=0; fad=0.0;
                            if(rec)
                            { if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rupdwn=0; rpre=-1; } trig=0;
                        }
                        else if (append)                                       //append
                        {
                            fad=0.0; trig=0;
                            if(rec)
                            {
                                dpos=maxpos; if(ramp){ bufeas(frames-1, b, nchan, dpos, rpre, dir, ramp); rfad=0; } rectoo=1; rupdwn=0; rpre=-1;
                            } else {goto apnde;}
                        }                                               //trigger start of initial loop creation
                        else
                        {
                            diro = dir; loop = frames - 1;
                            maxpos = dpos = (dir>=0) ? 0.0 : frames-1;
                            rectoo = 1; rpre=-1; fad=0.0; trig=0;
                        }
                    }
                    else
                    { apnde:
                        sprale=speed*srscale; if(rec)sprale=fabs(sprale)>(loop/1024)?(loop/1024)*dir:sprale; dpos=dpos+sprale;
                        if (dir == diro)
                        {                                   //buffer~-boundary constraints and registry of maximum distance traversed
                            if (dpos > frames - 1)
                            {
                                dpos = 0.0; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = frames-1;
                            }
                            else if (dpos < 0.0)
                            {
                                dpos = frames-1; rec = append;
                                if(rec){ if(ramp){ bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0; } }
                                doend = trig = 1; looprec = rectoo = 0; maxpos = 0.0;
                            }
                            else { if ( ((diro>=0)&&(maxpos<dpos))||((diro<0)&&(maxpos>dpos)) ) maxpos=dpos; }//<-track max write pos
                        }
                        else if(dir<0)                                         //wraparounds for reversal while creating initial-loop
                        {
                            if(dpos < 0.0)
                            { dpos=maxpos+dpos; if (ramp){bufeasoff(frames-1, b, nchan, 0.0, -dir, ramp); rpre=-1; rupdwn=rfad=0;} }
                        }
                        else if(dir>=0)
                        {
                            if(dpos > (frames-1))
                            {
                                dpos=maxpos+(dpos-(frames-1));
                                if(ramp){ bufeasoff(frames-1, b, nchan, frames-1, -dir, ramp); rpre=-1; rupdwn=rfad=0; }
                            }
                        }
                    }
                    
                    pos=trunc(dpos); if(dir>0){ frac=dpos-pos; }else if(dir<0){ frac=1.0-(dpos-pos); }else{ frac=0.0; }//interpratio
                    
                    if (ramp)
                    {
                        if (pfad<ramp)                                      //realtime ramps for play on/off
                        {
                            pfad++;
                            if(pupdwn)
                            {
                                if (pfad>=ramp)
                                {
                                    if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                                    switch(doend){ case 0:case 1:go=0;break; case 2:case 3:go=1;pfad=0;break; case 4:doend=0;break; }
                                }
                            }
                        }
                    }
                    else
                    {
                        if(pupdwn)
                        {
                            if (pupdwn == 2) { doend = 4; go = 1; } pupdwn = 0;
                            switch(doend) { case 0:case 1:go=0;break;  case 2:case 3:go=1;break; case 4:doend=0;break; }
                        }
                    }
                    
                }
                osamp1 = 0.0;
                oLprev = osamp1; *out1++ = osamp1; oRprev = osamp2 = 0.0; *out2++=0.0;
                o3prev = osamp3 = 0.0; *out3++=0.0; o4prev = osamp4 = 0.0; *out4++=0.0;
                *outPh++ = (diro>=0)?dpos/loop:dpos-(frames-loop)/loop;
                
                //~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                if (rec)            //(modded to assume maximum distance recorded into buffer~ as the total length)
                {
                    if ((rfad<ramp)&&(ramp>0.0)) recin1 = dease_func(recin1 + (((double)b[pos*nchan])*ovdb), rupdwn, ramp, rfad);
                    else recin1 += ((double)b[pos*nchan])*ovdb;
                    
                    if (rpre<0) { rpre = pos; numof = 0.0; rdif = writeva1 = 0.0; }
                    
                    if (rpre == pos) { writeva1 += recin1; numof += 1.0; }
                    else
                    {
                        if (numof>1.0) { writeva1 = writeva1/numof; numof = 1.0; }       //linear-average 4 speed < 1x
                        b[rpre*nchan] = writeva1;
                        rdif = (double)(pos - rpre);                                   //linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if(diro>=0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(maxpos*0.5))
                                    {
                                        rdif -= maxpos; coeff1 = (recin1 - writeva1)/rdif;
                                        for (i=(rpre-1);i>=0;i--) { writeva1 -= coeff1; b[i*nchan] = writeva1; }
                                        for (i=maxpos;i>pos;i--) { writeva1 -= coeff1; b[i*nchan] = writeva1; }
                                    }
                                    else
                                    {
                                        coeff1=(recin1-writeva1)/rdif;
                                        for(i=(rpre+1);i<pos;i++) { writeva1+=coeff1; b[i*nchan]=writeva1; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(maxpos*0.5))
                                    {
                                        rdif += maxpos; coeff1 = (recin1 - writeva1)/rdif;
                                        for (i=(rpre+1);i<(maxpos+1);i++) { writeva1 += coeff1; b[i*nchan] = writeva1; }
                                        for (i=0;i<pos;i++) { writeva1 += coeff1; b[i*nchan] = writeva1; }
                                    }
                                    else
                                    {
                                        coeff1=(recin1-writeva1)/rdif;
                                        for(i=(rpre-1);i>pos;i--) { writeva1-=coeff1; b[i*nchan]=writeva1; }
                                    }
                                }
                            }
                            else
                            {
                                if (rdif > 0)
                                {
                                    if (rdif>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif -= ((frames-1)-(maxpos)); coeff1 = (recin1 - writeva1)/rdif;
                                        for (i=(rpre-1);i>=maxpos;i--) { writeva1 -= coeff1; b[i*nchan] = writeva1; }
                                        for (i=(frames-1);i>pos;i--) { writeva1 -= coeff1; b[i*nchan] = writeva1; }
                                    }
                                    else
                                    {
                                        coeff1=(recin1-writeva1)/rdif;
                                        for(i=(rpre+1);i<pos;i++) { writeva1+=coeff1; b[i*nchan]=writeva1; }
                                    }
                                }
                                else
                                {
                                    if ((-rdif)>(((frames-1)-(maxpos))*0.5))
                                    {
                                        rdif += ((frames-1)-(maxpos)); coeff1 = (recin1 - writeva1)/rdif;
                                        for (i=(rpre+1);i<frames;i++) { writeva1 += coeff1; b[i*nchan] = writeva1; }
                                        for (i=maxpos;i<pos;i++) { writeva1 += coeff1; b[i*nchan] = writeva1; }
                                    }
                                    else
                                    {
                                        coeff1=(recin1-writeva1)/rdif;
                                        for(i=(rpre-1);i>pos;i--) { writeva1-=coeff1; b[i*nchan]=writeva1; }
                                    }
                                }
                            }
                        }
                        else
                        {
                            if (rdif>0)
                            { coeff1=(recin1-writeva1)/rdif; for(i=(rpre+1);i<pos;i++){ writeva1+=coeff1; b[i*nchan]=writeva1; } }
                            else
                            { coeff1=(recin1-writeva1)/rdif; for(i=(rpre-1);i>pos;i--){ writeva1-=coeff1; b[i*nchan]=writeva1; } }
                        }
                        writeva1 = recin1;                      //~ipoke end
                    }
                    if(ramp)                                                                //realtime ramps for record on/off
                    {
                        if (rfad<ramp)
                        {
                            rfad++;
                            if ((rupdwn)&&(rfad>=ramp))
                            {
                                if (rupdwn == 2) { doend = 4; trig = jnoff = 1; rfad = 0; }
                                else if (rupdwn == 5) { rec=1; }              rupdwn = 0;
                                switch (doend)
                                {
                                    case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                    case 2: rec=looprec=0; trig=1; break;
                                    case 3: rec=trig=1; rfad=looprec=0; break; case 4: doend=0; break;
                                }
                            }
                        }
                    }
                    else
                    {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) { doend = 4; trig = jnoff = 1; } else if (rupdwn == 5) { rec=1; } rupdwn = 0;
                            switch (doend)
                            {
                                case 0: rec = 0; break; case 1: if (diro<0) { loop = (frames-1)-maxpos; } else loop = maxpos;
                                case 2: rec=looprec=0; trig=1; break; case 3: rec=trig=1; looprec=0; break; case 4: doend=0; break;
                            }
                        }
                    }
                    rpre = pos; dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif!=0.0) ovdb = ovdb + ovdbdif;
        }
    }
    
    if (dirt) { buffer_setdirty(buf); }                                //notify other buf-related objs of write
    buffer_unlocksamples(buf);
    if (x->clockgo) { clock_delay(x->tclock, 0); x->clockgo = 0; }    //list-outlet stuff
    else if ((!go)||(x->rprtime<=0)) { clock_unset(x->tclock); x->clockgo = 1; }
    x->oLprev = oLprev;
    x->oRprev = oRprev;
    x->o3prev = o3prev;
    x->o4prev = o4prev;
    x->oLdif = oLdif;
    x->oRdif = oRdif;
    x->o3dif = o3dif;
    x->o4dif = o4dif;
    x->writevaL = writeva1;
    x->writevaR = writeva2;
    x->writeva3 = writeva3;
    x->writeva4 = writeva4;
    x->maxpos = maxpos;
    x->numof = numof;
    x->wrap = wrap;
    x->fad = fad;
    x->pos = dpos;
    x->diro = diro;
    x->dirp = dirp;
    x->rpos = rpre;
    x->rectoo = rectoo;
    x->rfad = rfad;
    x->trig = trig;
    x->jnoff = jnoff;
    x->go = go;
    x->rec = rec;
    x->recpre = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn = pupdwn;
    x->rupdwn = rupdwn;
    x->pfad = pfad;
    x->loop = loop;
    x->looprec = looprec;
    x->start = start;
    x->end = end;
    x->ovd = ovdb;
    x->doend = doend;
    x->append = append;
    return;
zero:
    while (n--) {*out1++ = 0.0; *out2++ = 0.0; *out3++ = 0.0; *out4++ = 0.0; *outPh++ = 0.0;} return;
}