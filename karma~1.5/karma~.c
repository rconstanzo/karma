/**
	@file
	karma~.c
 
	@ingroup
    msp
 
	@name
	karma~
	
	@realname
	karma~
 
	@type
	object
	
	@module
	karma
 
	@author
	raja (original to version 1.4) & pete (version 1.5)
	
	@digest
	Varispeed Audio Looper
	
	@description
    <o>karma~</o> is a dynamically lengthed varispeed record/playback looper object with complex functionality
 
	@discussion
	Rodrigo is sexy
 
	@category
	karma, looper, varispeed, msp, audio, external
 
	@keywords
	loop, looping, looper, varispeed, playback, buffer
 
	@seealso
	buffer~, groove~, record~, poke~, ipoke~
	
	@owner
	Rodrigo Constanzo
 __________________________________________________________________
 */


//  //  //  karma~ version 1.5 by pete, basically karma~ version 1.4 by raja...
//  //  //  ...with some bug fixes and code refactoring
//  //  //  November 2017
//  //  //  N.B. - [at]-commenting for 'DoctorMax' auto documentation

//  //  //  TODO version 1.5:
//  //  //  fix: a bunch of stuff

//  //  //  TODO version 2.0:
//  //  //  rewrite completely, take multiple perform routines out
//  //  //  and put interpolation routines and ipoke out into seperate files...
//  //  //  ...then will be able to integrate 'rubberband' and add better ipoke interpolations etc,
//  //  //  and possibly do seperate externals for different elements (e.g. karma~, karmaplay~, karmapoke~, karmaphase~, etc)


#include "stdlib.h"
#include "math.h"
#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "z_dsp.h"
#include "ext_atomic.h"


// Linear Interp
#define LINEAR_INTERP(f, x, y) (x + f*(y - x))
// Hermitic Cubic Interp, 4-point 3rd-order, ( James McCartney / Alex Harker )
#define CUBIC_INTERP(f, w, x, y, z) ((((0.5*(z - w) + 1.5*(x - y))*f + (w - 2.5*x + y + y - 0.5*z))*f + (0.5*(y - w)))*f + x)
// Catmull-Rom Spline Interp, 4-point 3rd-order, ( Paul Breeuwsma / Paul Bourke )
#define SPLINE_INTERP(f, w, x, y, z) (((-0.5*w + 1.5*x - 1.5*y + 0.5*z)*f*f*f) + ((w - 2.5*x + y + y - 0.5*z)*f*f) + ((-0.5*w + 0.5*y)*f) + x)
// ^^ 'SPLINE_INTERP' should be 'void inline' to save on f multiplies   // ^^                               // ^^                           // ??


typedef struct _karma {
    
    t_pxobject       ob;
    t_buffer_ref    *buf;
    t_symbol        *bufname;
    
    double  pos;            // play head
    double  maxpos;
    double  jump;           // jump phase 0..1
    double  extlwin;        // window (selection) length
    double  xstart;         // (start) position
    double  sr;             // samplerate
    double  bmsr;
    double  srscale;        // buffer / sr scaling factor
    double  prev;
    double  o1prev;
    double  o2prev;
    double  o3prev;
    double  o4prev;
    double  o1dif;
    double  o2dif;
    double  o3dif;
    double  o4dif;
    double  writeval1;
    double  writeval2;
    double  writeval3;
    double  writeval4;
    double  fad;
    double  ovdb;
    double  ovd;
    
    t_ptr_int   numof;
    t_ptr_int   loop;
    t_ptr_int   islooped;   // can disable/enable looping status (attr request)
    t_ptr_int   start;
    t_ptr_int   end;
    t_ptr_int   rpos;
    t_ptr_int   rfad;       // fade-counter for recording
    t_ptr_int   pfad;       // fade-counter for playback
    t_ptr_int   bframes;
    t_ptr_int   bchans;     // number of buffer channels
    t_ptr_int   chans;      // number of audio channels choice (object arg #2: 1 / 2 / 4)
    t_ptr_int   ramp;
    t_ptr_int   snramp;
    t_ptr_int   rprtime;    // right list outlet report granularity in ms
    t_ptr_int   curv;
    t_ptr_int   interpflag; // playback interpolation, 0 = linear, 1 = cubic, 2 = spline
    
    long    syncoutlet;     // make sync outlet ? (object arg #3: 0/1 flag)

    char    statecontrol;   // master looper state control (not 'human state')
    char    statehuman;     // master looper state human logic (not 'statecontrol') (0=stop, 1=play, 2=record, 3=overdub, 4=append 5=initial)
    char    pupdwn;         // playback up/down flag, 0 = fade up/in, 1 = fade down/out
    char    rupdwn;         // record up/down flag, 0 = fade up/in, 1 = fade down/out
    char    diro;
    char    dirp;
    char    doend;
    
    t_bool  stopallowed;    // flag, '0' if already stopped once (& init) [could have just used 'firstd' ??]
    t_bool  append;         // append
    t_bool  go;             // execute
    t_bool  rec;            // record ?
    t_bool  recpre;         // initial record ?
    t_bool  looprec;        // loop recording
    t_bool  rectoo;         // record after ... ? ... overdub ? ...
    t_bool  clockgo;        // do clock (list outlet)
    t_bool  wrap;
    t_bool  trig;
    t_bool  jnoff;
    t_bool  first;          // initial recording
    t_bool  firstd;         // initial initialise
    t_bool  skip;           // is initialising = 0
    t_bool  buf_mod;
    
    void    *tclock;
    void    *messout;
    
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
void karma_listclock(t_karma *x);
void karma_bufchange(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void karma_setup(t_karma *x, t_symbol *s);
void karma_jump(t_karma *x, double j);
void karma_append(t_karma *x);

void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags);
// mono:
void karma_mperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
// stereo:
void karma_sperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
// quad:
void karma_qperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);


static t_symbol *ps_nothing, *ps_buffer_modified;
static t_class *karma_class = NULL;


// easing function for recording (ipoke)
static inline double ease_record(double y1, char updwn, double ramp, t_ptr_int pfad)
{
    double ifup    = (1.0 - (((double)pfad) / ramp)) * PI;
    double ifdown  = (((double)pfad) / ramp) * PI;
    return updwn ? y1 * (0.5 * (1.0 - cos(ifup))) : y1 * (0.5 * (1.0 - cos(ifdown)));
}

// easing function for switch & ramp
static inline double ease_switchramp(double y1, double fad, t_ptr_int curv)
{
    switch (curv)
    {
        case 0: y1  = y1 * (1.0 - fad);                                     // case 0 = linear
            break;
        case 1: y1  = y1 * (1.0 - (sin((fad - 1) * PI/2) + 1));             // case 1 = sine ease in
            break;
        case 2: y1  = y1 * (1.0 - (fad * fad * fad));                       // case 2 = cubic ease in
            break;
        case 3: fad = fad - 1;
            y1  = y1 * (1.0 - (fad * fad * fad + 1));                       // case 3 = cubic ease out
            break;
        case 4: fad = (fad == 0.0) ? fad : pow(2, (10 * (fad - 1)));
            y1  = y1 * (1.0 - fad);                                         // case 4 = exponential ease in
            break;
        case 5: fad = (fad == 1.0) ? fad : (1 - pow(2, (-10 * fad)));
            y1  = y1 * (1.0 - fad);                                         // case 5 = exponential ease out
            break;
        case 6: if ((fad > 0) && (fad < 0.5))
            y1 = y1 * (1.0 - (0.5 * pow(2, ((20 * fad) - 10))));
        else if ((fad < 1) && (fad > 0.5))
            y1 = y1 * (1.0 - (-0.5 * pow(2, ((-20 * fad) + 10)) + 1));      // case 6 = exponential ease in/out
            break;
    }
    return y1;
}

// easing function for buffer read
static inline void ease_bufoff(t_ptr_int frms, float *b, t_ptr_int nchn, t_ptr_int mrk, char dr, double ramp)
{
    long i, fadpos;
    
    for (i = 0; i < ramp; i++)
    {
        fadpos = mrk + (dr * i);
        
        if ( !((fadpos < 0) || (fadpos > frms)) )
        {
            b[fadpos * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// easing function for buffer write
static inline void ease_bufon(t_ptr_int frms, float *b, t_ptr_int nchn, t_ptr_int mrk, t_ptr_int mrk2, char dr, double ramp)
{
    long i, fadpos, fadpos2, fadpos3;
    
    for (i = 0; i < ramp; i++)
    {
        fadpos  = (mrk  + (-dr)) + (-dr * i);
        fadpos2 = (mrk2 + (-dr)) + (-dr * i);
        fadpos3 =  mrk2 + (dr * i);
        
        if ( !((fadpos < 0) || (fadpos > frms)) )
        {
            b[fadpos * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos2 < 0) || (fadpos2 > frms)) )
        {
            b[fadpos2 * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos2 * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos2 * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos2 * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos3 < 0) || (fadpos3 > frms)) )
        {
            b[fadpos3 * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos3 * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos3 * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos3 * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / ramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// interpolation points storage
static inline void interp_index(t_ptr_int pos, t_ptr_int *indx0, t_ptr_int *indx1, t_ptr_int *indx2, t_ptr_int *indx3, char dir, char diro, t_ptr_int loop, t_ptr_int frmsm)
{
    *indx0 = pos - dir;                                 // calc of indecies 4 interps
    
    if (diro >= 0) {
        if (*indx0 < 0) {
            *indx0 = (loop + 1) + *indx0;
        } else if (*indx0 > loop) {
            *indx0 = *indx0 - (loop + 1);
        }
    } else {
        if(*indx0 < (frmsm - loop)) {
            *indx0 = frmsm - ((frmsm - loop) - *indx0);
        } else if (*indx0 > frmsm) {
            *indx0 = (frmsm - loop) + (*indx0 - frmsm);
        }
    }
    
    *indx1 = pos;
    *indx2 = pos + dir;
    
    if (diro >= 0) {
        if (*indx2 < 0) {
            *indx2 = (loop + 1) + *indx2;
        } else if (*indx2 > loop) {
            *indx2 = *indx2 - (loop + 1);
        }
    } else {
        if (*indx2 < (frmsm - loop)) {
            *indx2 = frmsm - ((frmsm - loop) - *indx2);
        } else if (*indx2 > frmsm) {
            *indx2 = (frmsm - loop) + (*indx2 - frmsm);
        }
    }
    
    *indx3 = *indx2 + dir;
    
    if (diro >= 0) {
        if(*indx3 < 0) {
            *indx3 = (loop + 1) + *indx3;
        } else if (*indx3 > loop) {
            *indx3 = *indx3 - (loop + 1);
        }
    } else {
        if (*indx3 < (frmsm - loop)) {
            *indx3 = frmsm - ((frmsm - loop) - *indx3);
        } else if (*indx3 > frmsm) {
            *indx3 = (frmsm - loop) + (*indx3 - frmsm);
        }
    }
    
    return;
}

//  //  //

void ext_main(void *r)
{
    t_class *c = class_new("karma~", (method)karma_new, (method)karma_free, (long)sizeof(t_karma), 0L, A_GIMME, 0);
    
    // @method position @digest window position start
    // @description window position start point (in phase 0..1) <br />
    // @marg 0 @name start_position @optional 0 @type float
    class_addmethod(c, (method)karma_start,     "position", A_FLOAT,    0);
    // @method window @digest window size
    // @description window size (in normalised segment length 0..1) <br />
    // @marg 0 @name window_size @optional 0 @type float
    class_addmethod(c, (method)karma_window,    "window",   A_FLOAT,    0);
    // @method jump @digest jump location
    // @description jump location (in normalised segment length 0..1) <br />
    // @marg 0 @name phase_location @optional 0 @type float
    class_addmethod(c, (method)karma_jump,      "jump",     A_FLOAT,    0);
    // @method stop @digest stop transport
    // @description stops internal transport immediately <br />
    class_addmethod(c, (method)karma_stop,      "stop",                 0);
    // @method play @digest play buffer
    // @description play buffer from stopped or recording or appending, play from beginning from playing <br />
    class_addmethod(c, (method)karma_play,      "play",                 0);
    // @method record @digest start recording
    // @description start recording (or overdubbing), or play from a recording state <br />
    class_addmethod(c, (method)karma_record,    "record",               0);
    // @method append @digest (get ready for) append recording
    // @description (get ready for) append recording to end of currently used segment (as long as buffer space available) <br />
    class_addmethod(c, (method)karma_append,    "append",               0);
    // @method set @digest set (new) buffer
    // @description set (new) buffer for recording or playback, can switch buffers in realtime <br />
    // @marg 0 @name buffer_name @optional 0 @type symbol
    // @marg 1 @name start_point @optional 1 @type float
    // @marg 2 @name end_point @optional 1 @type float
    class_addmethod(c, (method)karma_bufchange, "set",      A_GIMME,    0);
    // @method overdub @digest overdubbing amplitude
    // @description amplitude (0..1) for when in overdubbing state <br />
    // @marg 0 @name overdub @optional 0 @type float
    class_addmethod(c, (method)karma_overdub,   "overdub",  A_FLOAT,    0);
    
    class_addmethod(c, (method)karma_dsp64,     "dsp64",    A_CANT,     0);
    class_addmethod(c, (method)karma_assist,    "assist",   A_CANT,     0);
    class_addmethod(c, (method)karma_dblclick,  "dblclick", A_CANT,     0);
    class_addmethod(c, (method)karmabuf_notify, "notify",   A_CANT,     0);

    CLASS_ATTR_LONG(c, "report", 0, t_karma, rprtime);
    CLASS_ATTR_FILTER_MIN(c, "report", 0);
    CLASS_ATTR_LABEL(c, "report", 0, "Report Time (ms) for data outlet");
    // @description Set in <m>integer</m> values. Report time granualarity in <b>ms</b> for final data outlet. Default <b>50 ms</b> <br />
    
    CLASS_ATTR_LONG(c, "ramp", 0, t_karma, ramp);
    CLASS_ATTR_FILTER_CLIP(c, "ramp", 0, 2048);
    CLASS_ATTR_LABEL(c, "ramp", 0, "Ramp Time (samples)");
    // @description Set in <m>integer</m> values. Ramp time in <b>samples</b> for <m>play</m>/<m>record</m> fades. Default <b>256 samples</b> <br />
    
    CLASS_ATTR_LONG(c, "snramp", 0, t_karma, snramp);
    CLASS_ATTR_FILTER_CLIP(c, "snramp", 0, 2048);
    CLASS_ATTR_LABEL(c, "snramp", 0, "Switch&Ramp Time (samples)");
    // @description Set in <m>integer</m> values. Ramp time in <b>samples</b> for <b>switch &amp; ramp</b> type dynamic fades. Default <b>256 samples</b> <br />
    
    CLASS_ATTR_LONG(c, "snrcurv", 0, t_karma, curv);
    CLASS_ATTR_FILTER_CLIP(c, "snrcurv", 0, 6);
    CLASS_ATTR_ENUMINDEX(c, "snrcurv", 0, "Linear Sine_In Cubic_In Cubic_Out Exp_In Exp_Out Exp_In_Out");
    CLASS_ATTR_LABEL(c, "snrcurv", 0, "Switch&Ramp Curve");
    // @description Type of <b>curve</b> used in <b>switch &amp; ramp</b> type dynamic fades. Default <b>Sine_In</b> <br />
    
    CLASS_ATTR_LONG(c, "interp", 0, t_karma, interpflag);
    CLASS_ATTR_FILTER_CLIP(c, "interp", 0, 2);
    CLASS_ATTR_ENUMINDEX(c, "interp", 0, "Linear Cubic Spline");
    CLASS_ATTR_LABEL(c, "interp", 0, "Playback Interpolation");
    // @description Type of <b>interpolation</b> used in audio playback. Default <b>Cubic</b> <br />
    
    CLASS_ATTR_LONG(c, "loop", 0, t_karma, islooped);
    CLASS_ATTR_FILTER_CLIP(c, "loop", 0, 1);
    CLASS_ATTR_LABEL(c, "loop", 0, "Loop off / on");
    // @description Set as <m>integer</m> flag <b>0</b> or <b>1</b>. With <m>loop</m> switched on, <o>karma~</o> acts as a nornal looper, looping playback and/or recording depending on the state machine. With <m>loop</m> switched off, <o>karma~</o> will only play or record in oneshots. Default <b>On (1)</b> <br />

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    karma_class = c;
    
    ps_nothing = gensym("");
    ps_buffer_modified = gensym("buffer_modified");
}

void *karma_new(t_symbol *s, short argc, t_atom *argv)
{
    t_karma *x;
    t_symbol *bufname = 0;
    long syncoutlet = 0;
    t_ptr_int chans = 0;
    t_ptr_int attrstart = attr_args_offset(argc, argv);
    
    x = (t_karma *)object_alloc(karma_class);
    x->skip = 0;

    if (attrstart && argv) {
        bufname = atom_getsym(argv);
        // @arg 0 @name buffer_name @optional 0 @type symbol @digest Name of <o>buffer~</o> to be associated with the <o>karma~</o> instance
        // @description Essential argument: <o>karma~</o> will not operate without an associated <o>buffer~</o> <br />
        // The associated <o>buffer~</o> determines memory and length (if associating a buffer~ of <b>0 ms</b> in size <o>karma~</o> will do nothing) <br />
        // The associated <o>buffer~</o> can be changed on the fly (see the <m>set</m> message) but one must be present on instantiation <br />

        if (attrstart > 1) {
            chans = atom_getlong(argv + 1);
            if (attrstart > 2) {
                syncoutlet = atom_getlong(argv + 2);
                syncoutlet = CLAMP(syncoutlet, 0, 1);
            }
        }
    } else {
        object_error((t_object *)x, "karma~ will not load without an associated buffer~ declaration");
        goto zero;
    }
    
    if (x) {
        if (chans <= 1) {
            dsp_setup((t_pxobject *)x, 2);  // one audio channel inlet, one signal speed inlet
            chans = 1;
        } else if (chans == 2) {
            dsp_setup((t_pxobject *)x, 3);  // two audio channel inlets, one signal speed inlet
            chans = 2;
        } else {
            dsp_setup((t_pxobject *)x, 5);  // four audio channel inlets, one signal speed inlet
            chans = 4;
        }
        
        x->rpos = -1;
        x->rprtime = 50;
        x->snramp = x->ramp = 256;
        x->pfad = x->rfad = 257;
        x->sr = sys_getsr();
        x->ovd = x->ovdb = 1.0;
        x->islooped = x->curv = x->interpflag = 1;
        x->pupdwn = x->rupdwn = x->first = x->firstd = x->append = x->jnoff = x->statecontrol = x->statehuman = x->stopallowed = 0;
        x->dirp = x->diro = x->recpre = x->rec = x->rectoo = x->doend = x->go = x->trig = 0;
        x->numof = x->writeval1 = x->writeval2 = x->writeval3 = x->writeval4 = x->wrap = x->looprec = 0;
        x->maxpos = x->pos = 0.0;
        x->xstart = x->jump = x->fad = x->o1dif = x->o2dif = x->o3dif = x->o4dif = x->o1prev = x->o2prev = x->o3prev = x->o4prev = x->prev = 0.0;
        
        if (bufname != 0)
            x->bufname = bufname;
        else
            object_error((t_object *)x, "requires an associated buffer~ declaration");
        
        x->syncoutlet = syncoutlet; // sync outlet
        // @arg 2 @name sync_outlet @optional 1 @type int @digest Create audio rate sync position outlet ?
        // @description Default = <b>0 (off)</b> <br />
        // If <b>on</b>, <o>karma~</o> will have an additional outlet, after the audio channel outlets,
        // for sending an audio rate sync position outlet in the range <b>0..1</b> <br />

        x->chans = chans;
        // @arg 1 @name num_chans @optional 1 @type int @digest Number of Audio channels
        // @description Default = <b>1 (mono)</b> <br />
        // If <b>1</b>, <o>karma~</o> will operate in mono mode with one input for recording and one output for playback <br />
        // If <b>2</b>, <o>karma~</o> will operate in stereo mode with two inputs for recording and two outputs for playback <br />
        // If <b>4</b>, <o>karma~</o> will operate in quad mode with four inputs for recording and four outputs for playback <br />

        x->messout = listout(x);    // data outlet
        x->tclock = clock_new((t_object * )x, (method)karma_listclock);
        attr_args_process(x, argc, argv);
        
        if (chans <= 1) {           // mono
            if (syncoutlet)
                outlet_new(x, "signal");    // last: sync (optional)
            outlet_new(x, "signal");        // first: audio output
        } else if (chans == 2) {    // stereo
            if (syncoutlet)
                outlet_new(x, "signal");    // last: sync (optional)
            outlet_new(x, "signal");        // second: audio output 2
            outlet_new(x, "signal");        // first: audio output 1
        } else {                    // quad
            if (syncoutlet)
                outlet_new(x, "signal");    // last: sync (optional)
            outlet_new(x, "signal");        // fourth: audio output 4
            outlet_new(x, "signal");        // third: audio output 3
            outlet_new(x, "signal");        // second: audio output 2
            outlet_new(x, "signal");        // first: audio output 1
        }
        
        x->skip = 1;
        x->ob.z_misc |= Z_NO_INPLACE;
    }
    
zero:
    return (x);
}

void karma_free(t_karma *x)
{
    if (x->skip) {
        dsp_free((t_pxobject *)x);
        object_free(x->buf);
        object_free(x->tclock);
        object_free(x->messout);
    }
}

void karma_dblclick(t_karma *x)
{
    buffer_view(buffer_ref_getobject(x->buf));
}

void karma_setup(t_karma *x, t_symbol *s)
{
    t_buffer_obj *buf;
    x->bufname = s;
    
    if (!x->buf)
        x->buf = buffer_ref_new((t_object *)x, s);
    else
        buffer_ref_set(x->buf, s);
    
    buf = buffer_ref_getobject(x->buf);
    
    if (buf == NULL) {
        x->buf = 0;
        object_error((t_object *)x, "there is no buffer~ named '%s'", s->s_name);
    } else {
        x->diro = 0;
        x->maxpos = x->pos = 0.0;
        x->rpos = -1;
        x->bchans = buffer_getchannelcount(buf);
        x->bframes = buffer_getframecount(buf);
        x->bmsr = buffer_getmillisamplerate(buf);
        x->srscale = buffer_getsamplerate(buf) / x->sr;
        x->xstart = 0.0;                                    // !!
        x->extlwin = 1.;
        x->loop = x->end = x->bframes - 1;
    }
}

void karma_modset(t_karma *x, t_buffer_obj *b)
{
    double bsr, bmsr;
    t_ptr_int chans;
    t_ptr_int frames;
    
    if (b) {
        bsr = buffer_getsamplerate(b);
        chans = buffer_getchannelcount(b);
        frames = buffer_getframecount(b);
        bmsr = buffer_getmillisamplerate(b);
        
        if (((x->bchans != chans) || (x->bframes != frames)) || (x->bmsr != bmsr)) {
            x->bmsr = bmsr;
            x->srscale = bsr / x->sr;
            x->bframes = frames;
            x->bchans = chans;
            x->start = 0.0;
            x->loop = x->end = x->bframes - 1;
            karma_window(x, x->extlwin);
            karma_start(x, x->xstart);
        }
    }
}

void karma_bufchange(t_karma *x, t_symbol *s, short argc, t_atom *argv)
{
    t_buffer_obj *buf;
    t_symbol *b = atom_getsym(argv);
    
    if (b != ps_nothing) {
        x->bufname = b;
        
        if (!x->buf)
            x->buf = buffer_ref_new((t_object *)x, b);
        else
            buffer_ref_set(x->buf, b);
        
        buf = buffer_ref_getobject(x->buf);
        
        if (buf == NULL) {
            x->buf = 0;
            object_error((t_object *)x, "there is no buffer~ named '%s'", b->s_name);
        } else {
            x->diro = 0;
            x->maxpos = x->pos = 0.0;
            x->rpos = -1;
            x->bchans = buffer_getchannelcount(buf);
            x->bframes = buffer_getframecount(buf);
            x->bmsr = buffer_getmillisamplerate(buf);
            x->srscale = buffer_getsamplerate(buf) / x->sr;
            
            if (argc <= 1) {
                x->start = 0.0;
                x->loop = x->end = x->bframes - 1;
                karma_window(x, x->extlwin);
                karma_start(x, x->xstart);
            } else if (argc == 2) {
                x->start = 0.0;
                x->loop = x->end = atom_getfloat(argv+1) * x->bframes - 1;
            } else {
                x->start = atom_getfloat(argv+1) * x->bframes - 1;
                x->loop = x->end = atom_getfloat(argv+2) * x->bframes - 1;
            }
        }
    } else {
        object_error((t_object *)x, "requires an associated buffer~ declaration");
    }
}

void karma_listclock(t_karma *x)
{
    if (x->rprtime != 0)    // ('rprtime 0' == off, else milliseconds)
    {
        t_ptr_int frames = x->bframes - 1;
        t_ptr_int loop = x->loop;
        t_ptr_int diro = x->diro;
        
        t_bool rec = x->rec;
        t_bool go = x->go;

        char statehuman = x->statehuman;
        
        double bmsr = x->bmsr;
        double pos = x->pos;
        double xtlwin = x->extlwin;
        
        t_atom datalist[7];
        atom_setfloat(  datalist + 0,   CLAMP((diro < 0) ? ((pos - (frames - loop)) / loop) : (pos / loop), 0., 1.) );  // position float
        atom_setlong(   datalist + 1,   go  );                                                                          // play flag
        atom_setlong(   datalist + 2,   rec );                                                                          // record flag
        atom_setfloat(  datalist + 3, ((diro < 0) ? ((frames - loop) / bmsr) : 0.0)     );                              // start float
        atom_setfloat(  datalist + 4, ((diro < 0) ? (frames / bmsr) : (loop / bmsr))    );                              // end float
        atom_setfloat(  datalist + 5, ((xtlwin * loop) / bmsr)  );                                                      // window float
        atom_setlong(   datalist + 6,   statehuman  );                                                                  // state flag
        
        outlet_list(x->messout, 0L, 7, datalist);   // &datalist ?!
        
        if (sys_getdspstate() && (x->rprtime > 0)) {
            clock_delay(x->tclock, x->rprtime);
        }
    }
}

void karma_assist(t_karma *x, void *b, long m, long a, char *s)
{
    long dummy;
    int synclet;
    dummy = a + 1;
    synclet = x->syncoutlet;
    a = (a < x->chans) ? 0 : ((a > x->chans) ? 2 : 1);
    
    if (m == ASSIST_INLET) {
        switch (a)
        {
            case 0:
                if (dummy == 1) {
                    if (x->chans == 1)
                        sprintf(s, "(signal) Record Input / messages to karma~");
                    else
                        sprintf(s, "(signal) Record Input 1 / messages to karma~");
                } else {
                    sprintf(s, "(signal) Record Input %ld", dummy);
                    // @in 0 @type signal @digest Audio Inlet(s)... (object arg #2)
                }
                break;
            case 1:
                sprintf(s, "(signal) Speed Factor");
                    // @in 1 @type signal @digest Speed Factor (1. = normal speed, < 1. = slower, > 1. = faster)
                break;
            case 2:
                break;
        }
    } else {    // ASSIST_OUTLET
        switch (a)
        {
            case 0:
                if (x->chans == 1)
                    sprintf(s, "(signal) Audio Output");
                else
                    sprintf(s, "(signal) Audio Output %ld", dummy);
                    // @out 0 @type signal @digest Audio Outlet(s)... (object arg #2)
                break;
            case 1:
                if (synclet)
                    sprintf(s, "(signal) Sync Outlet (current position 0..1)");
                    // @out 1 @type signal @digest if chosen (object arg #3) Sync Outlet (current position 0..1)
                else
                    sprintf(s, "List: current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial)");
                break;
            case 2:
                sprintf(s, "List: current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial)");
                    // @out 2 @type list @digest Data Outlet (current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial))
                break;
        }
    }
}

//  //  //

void karma_start(t_karma *x, double strt)   // strt = "position" float message
{
    x->xstart = strt;
    
    if (!x->looprec)
    {
        if (x->diro < 0) {
            x->start = CLAMP( ((x->bframes - 1) - x->loop) + (x->xstart * x->loop), (x->bframes - 1) - x->loop, x->bframes - 1);
            x->end = x->start + (x->extlwin * x->loop);
            if (x->end > (x->bframes - 1)) {
                x->end = ((x->bframes - 1) - x->loop) + (x->end - (x->bframes - 1));
                x->wrap = 1;
            } else {
                x->wrap = 0;
            }
        } else {
            x->start = CLAMP(strt * x->loop, 0.0, x->loop);
            x->end = x->start + (x->extlwin * x->loop);
            if (x->end > x->loop) {
                x->end = x->end - x->loop;
                x->wrap = 1;
            } else {
                x->wrap = 0;
            }
        }
    }
}

// !! pete: i do not like the name "window" - surely it should be "selection" ??
void karma_window(t_karma *x, double dur)   // dur = "window" float message
{
    t_ptr_int loop = x->loop;
    
    if (!x->looprec) {
        x->extlwin = (dur < 0.001) ? 0.001 : dur;
        if (x->diro < 0) {
            x->end = x->start + (x->extlwin * loop);
            if (x->end > (x->bframes - 1)) {
                x->end = ((x->bframes - 1) - x->loop) + (x->end - (x->bframes - 1));
                x->wrap = 1;
            } else {
                x->wrap = 0;
            }
        } else {
            x->end = x->start + (x->extlwin * loop);
            if(x->end > loop) {
                x->end = x->end - loop;
                x->wrap = 1;
            } else {
                x->wrap=0;
            }
        }
    } else {
        x->extlwin = (dur < 0.001) ? 0.001 : dur;
    }
}

void karma_stop(t_karma *x)
{
    if (x->firstd) {
        if (x->stopallowed) {
            x->statecontrol = x->rectoo ? 6 : 7;
            x->append = 0;
            x->statehuman = 0;
            x->stopallowed = 0;
        }
    }
}

void karma_play(t_karma *x)
{
    if ((!x->go) && (x->append)) {
        x->statecontrol = 9;
        x->fad = 0.0;
    } else if ((x->rec) || (x->append)) {
        x->statecontrol = x->rectoo ? 4 : 3;
    } else {
        x->statecontrol = 5;
    }
    
    x->go = 1;
    x->statehuman = 1;
    x->stopallowed = 1;
}

void karma_record(t_karma *x)
{
    float *b;
    long i;
    char sc, sh;
    t_bool r, g, rt, ap, fr;
    t_ptr_int nc;
    t_ptr_int frms;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    r = x->rec;
    g = x->go;
    rt = x->rectoo;
    ap = x->append;
    fr = x->first;
    sc = x->statecontrol;
    sh = x->statehuman;

    x->stopallowed = 1;

    if (r) {
        if (rt) {
            sc = 2;
            sh = 3;
        } else {
            sc = 3;
            sh = 2;
        }
    } else {
        if (ap) {
            if (g) {
                if (rt) {
                    sc = 2;
                    sh = 3;
                } else {
                    sc = 10;
                    sh = 4;
                }
            } else {
                sc = 1;
                sh = 5;
            }
        } else {
            if (!g) {
                fr = 1;
                if (buf) {
                    nc = x->bchans;
                    frms = x->bframes;
                    b = buffer_locksamples(buf);
                    if (!b)
                        goto zero;
                    
                    for (i = 0; i < frms; i++) {
                        if (nc > 1) {
                            b[i * nc] = 0.0;
                            b[(i * nc) + 1] = 0.0;
                            if (nc > 2) {
                                b[(i * nc) + 2] = 0.0;
                                if (nc > 3) {
                                    b[(i * nc) + 3] = 0.0;
                                }
                            }
                        } else {
                            b[i] = 0.0;
                        }
                    }
                    
                    buffer_setdirty(buf);
                    buffer_unlocksamples(buf);
                }
                sc = 1;
                sh = 5;
            } else {
                sc = 11;
                sh = 2;
            }
        }
    }
    
    g = 1;
    x->go = g;
    x->first = fr;
    x->statecontrol = sc;
    x->statehuman = sh;
    
zero:
    return;
}

void karma_append(t_karma *x)
{
    if (x->first) {
        if ((!x->append) && (!x->looprec)) {
            x->append = 1;
            x->loop = x->bframes - 1;
            x->statecontrol = 9;
            x->statehuman = 4;
            x->stopallowed = 1;
        } else {
            object_error((t_object *)x, "can't append if already appending, or during creating 'initial-loop', or if buffer~ is completely filled");
        }
    } else {
        object_error((t_object *)x, "warning! no 'append' registered until at least one loop has been created first");
    }
}

void karma_overdub(t_karma *x, double o)
{
    x->ovdb = CLAMP(o, 0.0, 1.0);               // clamp overzealous ??
}

void karma_jump(t_karma *x, double j)
{
    if (x->firstd) {
        if((x->looprec) && (!x->rec)) {         // if(!((x->looprec) && (!x->rec))) ...
                                                // ... ?? ...
        } else {
            x->statecontrol = 8;
            x->jump = j;
            x->statehuman = 1;                  // hmmm... ...
            x->stopallowed = 1;
        }
    }
}

t_max_err karmabuf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
    if (msg == ps_buffer_modified)
        x->buf_mod = 1;
    
    return buffer_ref_notify(x->buf, s, msg, sndr, dat);
}

void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags)
{
    x->sr = srate;
    x->clockgo = 1;
    
    if (x->bufname != 0) {
        if (!x->firstd)
            karma_setup(x, x->bufname);
        if (x->chans > 1) {
            if (x->chans > 2) {
                object_method(dsp64, gensym("dsp_add64"), x, karma_qperf, 0, NULL);
                post("karma~_64bit_v1.5_quad");
            } else {
                object_method(dsp64, gensym("dsp_add64"), x, karma_sperf, 0, NULL);
                post("karma~_64bit_v1.5_stereo");
            }
        } else {
            object_method(dsp64, gensym("dsp_add64"), x, karma_mperf, 0, NULL);
            post("karma~_64bit_v1.5_mono");
        }
        if (!x->firstd) {
            karma_window(x, 1.);
            x->firstd = 1;
        } else {
            karma_window(x, x->extlwin);
            karma_start(x, x->xstart);
        }
    } else {
        object_error((t_object *)x, "fails without buffer~ name!");
    }
}


//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  //  //  PERFORM ROUTINES    //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //


// mono perform

void karma_mperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long syncoutlet = x->syncoutlet;

    double *in1 = ins[0];   // mono in
    double *in2 = ins[1];                       // speed
    
    double *out1  = outs[0];// mono out
    double *outPh = syncoutlet ? outs[1] : 0;   // sync (if arg #3 is on)

    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osamp1, ovdb, ovd, ovdbdif, xstart, xwin;
    double o1prev, o1dif, frac, fad, ramp, snramp, writeval1, coeff1, recin1;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, interp0, interp1, interp2, interp3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    rec             = x->rec;
    recpre          = x->recpre;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (rec || recpre)
        dirt        = 1;
    if (x->buf_mod) {
        karma_modset(x, buf);
        x->buf_mod  = 0;
    }
    
    o1prev          = x->o1prev;
    o1dif           = x->o1dif;
    writeval1       = x->writeval1;

    go              = x->go;
    statecontrol    = x->statecontrol;
    pupdwn          = x->pupdwn;
    rupdwn          = x->rupdwn;
    rpre            = x->rpos;
    rectoo          = x->rectoo;
    nchan           = x->bchans;    //
    srscale         = x->srscale;
    frames          = x->bframes;
    trig            = x->trig;
    jnoff           = x->jnoff;
    append          = x->append;
    diro            = x->diro;
    dirp            = x->dirp;
    loop            = x->loop;
    xwin            = x->extlwin;
    looprec         = x->looprec;
    start           = x->start;
    xstart          = x->xstart;
    end             = x->end;
    doend           = x->doend;
    ovdb            = x->ovd;
    ovd             = x->ovdb;
    ovdbdif         = (ovdb != ovd) ? ((ovd - ovdb) / n) : 0.0;
    rfad            = x->rfad;
    pfad            = x->pfad;
    dpos            = x->pos;
    pos             = trunc(dpos);
    maxpos          = x->maxpos;
    wrap            = x->wrap;
    jump            = x->jump;
    numof           = x->numof;
    fad             = x->fad;
    ramp            = (double)x->ramp;
    snramp          = (double)x->snramp;
    curv            = x->curv;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: rec init
            rec = go = trig = looprec = 1;
            rfad = rupdwn = pfad = pupdwn = statecontrol = 0;
            break;
        case 2:             // case 2: rec rectoo
            doend = 3;
            rec = rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 3:             // case 3: rec off reg
            rupdwn = 1;
            pupdwn = 3;
            pfad = rfad = statecontrol = 0;
            break;
        case 4:             // case 4: play rectoo
            doend = 2;
            rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            trig = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop rectoo
            pfad = rfad = 0;
            doend = pupdwn = rupdwn = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (rec) {
                rfad = 0;
                rupdwn = 1;
            }
            pfad = 0;
            pupdwn = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (rec) {
                rfad = 0;
                rupdwn = 2;
            }
            pfad = 0;
            pupdwn = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            pupdwn = 4;
            pfad = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            rec = looprec = rectoo = 1;
            fad = 0.0;
            rfad = rupdwn = statecontrol = 0;
            break;
        case 11:            // case 11: rec on reg
            pupdwn = 3;
            rupdwn = 5;
            rfad = pfad = statecontrol = 0;
            break;          // !!
    }

    // raja notes:
    // 'fad = 0.0' triggers switch&ramp (declick play)
    // 'rpre = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)

    while (n--)
    {
        recin1 = *in1++;
        speed = *in2++;
        dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);

        // declick for change of 'dir'ection
        if (dirp != dir) {
            if (rec && ramp) {
                ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                rfad = rupdwn = 0;
                rpre = -1;
            }
            fad = 0.0;
        };              // !! !!
        
        if ((rec - recpre) < 0) {           // samp @rec-off
            if (ramp)
                ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
            rpre = -1;
            dirt = 1;
        } else if ((rec - recpre) > 0) {    // samp @rec-on
            rfad = rupdwn = 0;
            if (speed < 1.0)
                fad = 0.0;
            if (ramp)
                ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
        }
        recpre = rec;
        
        if (!looprec)
        {
            if (go)
            {
                if (trig)
                {
                    if (doend)  // calculate end of loop
                    {
                        if (diro >= 0)
                        {
                            loop = CLAMP(maxpos, 4096, frames - 1);
                            dpos = start = (xstart * loop);
                            end = start + (xwin * loop);
                            if (end > loop) {
                                end = end - (loop + 1);
                                wrap = 1;
                            } else {
                                wrap = 0;
                            }
                            if (dir < 0) {
                                if (ramp)
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                            }
                        } else {
                            loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                            start = ((frames - 1) - loop) + (xstart * loop);
                            dpos = end = start + (xwin * loop);
                            if (end > (frames - 1)) {
                                end = ((frames - 1) - loop) + (end - frames);
                                wrap = 1;
                            } else {
                                wrap = 0;
                            }
                            dpos = end;
                            if (dir > 0) {
                                if (ramp)
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                            }
                        }
                        if (ramp)
                            ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                        rpre = -1;
                        fad = 0.0;
                        trig = 0;
                        append = rectoo = doend = 0;
                    } else {    // jump / play
                        if (jnoff)
                            dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                        else
                            dpos = (dir < 0) ? end : start;
                        if (rec) {
                            if (ramp) {
                                ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                rfad = 0;
                            }
                            rpre = -1;
                            rupdwn = 0;
                        }
                        fad = 0.0;
                        trig = 0;
                    }
                } else {        // jump-based constraints (outside 'window')
                    sprale = speed * srscale;
                    if (rec)
                        sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                    dpos = dpos + sprale;
                    
                    if (jnoff)
                    {
                        if (wrap) {
                            if ((dpos < end) || (dpos > start))
                                jnoff = 0;
                        } else {
                            if ((dpos < end) && (dpos > start))
                                jnoff = 0;
                        }
                        if (diro >= 0)
                        {
                            if (dpos > loop)
                            {
                                dpos = dpos - loop;
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            } else if (dpos < 0.0) {
                                dpos = loop + dpos;
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            }
                        } else {
                            if (dpos > (frames - 1))
                            {
                                dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            } else if (dpos < ((frames - 1) - loop)) {
                                dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            }
                        }
                    } else {    // regular 'window' / 'position' constraints
                        if (wrap)
                        {
                            if ((dpos > end) && (dpos < start))
                            {
                                dpos = (dir >= 0) ? start : end;
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            } else if (diro >= 0) {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                                else if (dpos < 0.0)
                                {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos < ((frames - 1) - loop))
                                {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec)
                                    {
                                        if (ramp) {
                                            ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos > (frames - 1)) {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {
                            if ((dpos > end) || (dpos < start))
                            {
                                dpos = (dir >= 0) ? start : end;
                                fad = 0.0;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                        rfad = 0;
                                    }
                                    rupdwn = 0;
                                    rpre = -1;
                                }
                            }
                        }
                    }
                }

                // interp ratio
                pos = trunc(dpos);
                if (dir > 0) {
                    frac = dpos - pos;
                } else if (dir < 0) {
                    frac = 1.0 - (dpos - pos);
                } else {
                    frac = 0.0;
                }
                interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                
                if (rec) {              // if recording do linear-interp else...
                    osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                } else {                // ...cubic / spline if interpflag > 0 (default cubic)
/*                    osamp1 = interp ?
                                CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]) :
                                    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
*/                  if (interp == 1)
                        osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                    else if (interp == 2)
                        osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                    else
                        osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                }
                
                if (ramp)
                {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                    if (fad < 1.0)
                    {
                        if (fad == 0.0) {
                            o1dif = o1prev - osamp1;
                        }
                        osamp1 += ease_switchramp(o1dif, fad, curv);// <- easing-curv options (implemented by raja)
                        fad += 1 / snramp;
                    }                                               // "Switch and Ramp" end
                    
                    if (pfad < ramp)
                    {                                               // realtime ramps for play on/off
                        osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                        pfad++;
                        if (pfad >= ramp)
                        {
                            switch (pupdwn)
                            {
                                case 0:
                                    break;
                                case 1:
                                    pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                    break;
                                case 2:
                                    if (!rec)
                                        trig = jnoff = 1;
                                    break;                          // !! break pete fix !!
                                case 3:                             // jump // rec off reg
                                    pupdwn = pfad = 0;
                                    break;
                                case 4:                             // append
                                    go = trig = looprec = 1;
                                    fad = 0.0;
                                    pfad = pupdwn = 0;
                                    break;
                            }
                        }
                    }
                } else {
                    switch (pupdwn)
                    {
                        case 0:
                            break;
                        case 1:
                            pupdwn = go = 0;
                            break;
                        case 2:
                            if (!rec)
                                trig = jnoff = 1;
                            break;                                  // !! break pete fix !!
                        case 3:                                     // jump     // rec off reg
                            pupdwn = 0;
                            break;
                        case 4:                                     // append
                            go = trig = looprec = 1;
                            fad = 0.0;
                            pfad = pupdwn = 0;
                            break;
                    }
                }
                
            } else {
                osamp1 = 0.0;
            }
            
            o1prev = osamp1;
            *out1++ = osamp1;
            if (syncoutlet)
                *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
            
            /*
             ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
             (modded to allow for 'window' and 'position' to change on the fly)
             raja's razor: simplest answer to everything was:
             recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
             ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
             ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
            */
            if (rec)
            {
                if ((rfad < ramp) && (ramp > 0.0))
                    recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                else
                    recin1 += ((double)b[pos * nchan]) * ovdb;
                
                if (rpre < 0) {
                    rpre = pos;
                    numof = 0.0;
                    rdif = writeval1 = 0.0;
                }
                
                if (rpre == pos) {
                    writeval1 += recin1;
                    numof += 1.0;
                } else {
                    if (numof > 1.0) {                  // linear-averaging for speed < 1x
                        writeval1 = writeval1 / numof;
                        numof = 1.0;
                    }
                    b[rpre * nchan] = writeval1;
                    rdif = (double)(pos - rpre);
                    if (rdif > 0) {                     // linear-interpolation for speed > 1x
                        coeff1 = (recin1 - writeval1) / rdif;
                        for (i = rpre + 1; i < pos; i++) {
                            writeval1 += coeff1;
                            b[i * nchan] = writeval1;
                        }
                    } else {
                        coeff1 = (recin1 - writeval1) / rdif;
                        for (i = rpre - 1; i > pos; i--) {
                            writeval1 -= coeff1;
                            b[i * nchan] = writeval1;
                        }
                    }
                    writeval1 = recin1;
                }
                rpre = pos;
                dirt = 1;
            }                                           // ~ipoke end
            
            if (ramp)                                   // realtime ramps for record on/off
            {
                if(rfad < ramp)
                {
                    rfad++;
                    if ((rupdwn) && (rfad >= ramp))
                    {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                            rfad = 0;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
            } else {
                if (rupdwn) {
                    if (rupdwn == 2) {
                        trig = jnoff = 1;
                    } else if (rupdwn == 5) {
                        rec = 1;
                    } else {
                        rec = 0;
                    }
                    rupdwn = 0;
                }
            }
            dirp = dir;
        } else {                                        // initial loop creation
            if (go)
            {
                if (trig)
                {
                    if (jnoff)                          // jump
                    {
                        if (diro >= 0) {
                            dpos = jump * maxpos;
                        } else {
                            dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                        }
                        jnoff = 0;
                        fad = 0.0;
                        if (rec) {
                            if (ramp) {
                                ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                rfad = 0;
                            }
                            rupdwn = 0;
                            rpre = -1;
                        }
                        trig = 0;
                    } else if (append) {                // append
                        fad = 0.0;
                        trig = 0;
                        if (rec)
                        {
                            dpos = maxpos;
                            if (ramp) {
                                ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                rfad = 0;
                            }
                            rectoo = 1;
                            rupdwn = 0;
                            rpre = -1;
                        } else {
                            goto apned;
                        }
                    } else {                            // trigger start of initial loop creation
                        diro = dir;
                        loop = frames - 1;
                        maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                        rectoo = 1;
                        rpre = -1;
                        fad = 0.0;
                        trig = 0;
                    }
                } else {
apned:
                    sprale = speed * srscale;
                    if (rec)
                        sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                    dpos = dpos + sprale;
                    if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                    {
                        if (dpos > (frames - 1))
                        {
                            dpos = 0.0;
                            rec = append;
                            if (rec) {
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                            doend = trig = 1;
                            looprec = rectoo = 0;
                            maxpos = frames - 1;
                        } else if (dpos < 0.0) {
                            dpos = frames - 1;
                            rec = append;
                            if (rec) {
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                            doend = trig = 1;
                            looprec = rectoo = 0;
                            maxpos = 0.0;
                        } else {                        // <- track max write pos
                            if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                maxpos = dpos;
                        }
                    } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                        if (dpos < 0.0)
                        {
                            dpos = maxpos + dpos;
                            if (ramp) {
                                ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                rpre = -1;
                                rupdwn = rfad = 0;
                            }
                        }
                    } else if (dir >= 0) {
                        if (dpos > (frames - 1))
                        {
                            dpos = maxpos + (dpos - (frames - 1));
                            if (ramp) {
                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                rpre = -1;
                                rupdwn = rfad = 0;
                            }
                        }
                    }
                }
                
                pos = trunc(dpos);
                if (dir > 0) {                          // interp ratio
                    frac = dpos - pos;
                } else if (dir < 0) {
                    frac = 1.0 - (dpos - pos);
                } else {
                    frac = 0.0;
                }
                
                if (ramp)
                {
                    if (pfad < ramp)                    // realtime ramps for play on/off
                    {
                        pfad++;
                        if (pupdwn)
                        {
                            if (pfad >= ramp)
                            {
                                if (pupdwn == 2) {
                                    doend = 4;
                                    go = 1;
                                }
                                pupdwn = 0;
                                switch (doend) {
                                    case 0:
                                    case 1:
                                        go = 0;
                                        break;
                                    case 2:
                                    case 3:
                                        go = 1;
                                        pfad = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    }
                } else {
                    if (pupdwn)
                    {
                        if (pupdwn == 2) {
                            doend = 4;
                            go = 1;
                        }
                        pupdwn = 0;
                        switch (doend) {
                            case 0:
                            case 1:
                                go = 0;
                                break;
                            case 2:
                            case 3:
                                go = 1;
                                break;
                            case 4:
                                doend = 0;
                                break;
                        }
                    }
                }
                
            }
            
            osamp1 = 0.0;
            o1prev = osamp1;
            *out1++ = osamp1;
            if (syncoutlet)
                *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
            
            // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
            // (modded to assume maximum distance recorded into buffer~ as the total length)
            if (rec)
            {
                if ((rfad < ramp) && (ramp > 0.0))
                    recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                else
                    recin1 += ((double)b[pos * nchan]) * ovdb;
                
                if (rpre < 0) {
                    rpre = pos;
                    numof = 0.0;
                    rdif = writeval1 = 0.0;
                }
                
                if (rpre == pos) {
                    writeval1 += recin1;
                    numof += 1.0;
                } else {
                    if (numof > 1.0) {                  // linear-averaging for speed < 1x
                        writeval1 = writeval1 / numof;
                        numof = 1.0;
                    }
                    b[rpre * nchan] = writeval1;
                    rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                    if (dir != diro)
                    {
                        if (diro >= 0)
                        {
                            if (rdif > 0)
                            {
                                if (rdif > (maxpos * 0.5))
                                {
                                    rdif -= maxpos;
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i >= 0; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = maxpos; i > pos; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < pos; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            } else {
                                if ((-rdif) > (maxpos * 0.5))
                                {
                                    rdif += maxpos;
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = 0; i < pos; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i > pos; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                {
                                    rdif -= ((frames - 1) - (maxpos));
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i >= maxpos; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = (frames - 1); i > pos; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < pos; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            } else {
                                if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                {
                                    rdif += ((frames - 1) - (maxpos));
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < frames; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = maxpos; i < pos; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i > pos; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            }
                        }
                    } else {
                        if (rdif > 0)
                        {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = (rpre + 1); i < pos; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = (rpre - 1); i > pos; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                    }
                    writeval1 = recin1;
                }                                       // ~ipoke end
                if (ramp)                               // realtime ramps for record on/off
                {
                    if (rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                  // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    rfad = looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                } else {
                    if (rupdwn)
                    {
                        if (rupdwn == 2) {
                            doend = 4;
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        }
                        rupdwn = 0;
                        switch (doend)
                        {
                            case 0:
                                rec = 0;
                                break;
                            case 1:
                                if (diro < 0) {
                                    loop = (frames - 1) - maxpos;
                                } else {
                                    loop = maxpos;
                                }
                                break;                      // !! break pete fix different !!
                            case 2:
                                rec = looprec = 0;
                                trig = 1;
                                break;
                            case 3:
                                rec = trig = 1;
                                looprec = 0;
                                break;
                            case 4:
                                doend = 0;
                                break;
                        }
                    }
                }               //
                rpre = pos;
                dirt = 1;
            }
            dirp = dir;
        }
        if (ovdbdif != 0.0)
            ovdb = ovdb + ovdbdif;
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->rprtime <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev       = o1prev;
    x->o1dif        = o1dif;
    x->writeval1    = writeval1;

    x->maxpos       = maxpos;
    x->numof        = numof;
    x->wrap         = wrap;
    x->fad          = fad;
    x->pos          = dpos;
    x->diro         = diro;
    x->dirp         = dirp;
    x->rpos         = rpre;
    x->rectoo       = rectoo;
    x->rfad         = rfad;
    x->trig         = trig;
    x->jnoff        = jnoff;
    x->go           = go;
    x->rec          = rec;
    x->recpre       = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn       = pupdwn;
    x->rupdwn       = rupdwn;
    x->pfad         = pfad;
    x->loop         = loop;
    x->looprec      = looprec;
    x->start        = start;
    x->end          = end;
    x->ovd          = ovdb;
    x->doend        = doend;
    x->append       = append;

    return;

zero:
    while (n--) {
        *out1++  = 0.0;
        if (syncoutlet)
            *outPh++ = 0.0;
    }
    
    return;
}

// stereo perform

void karma_sperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long syncoutlet = x->syncoutlet;
    
    double *in1 = ins[0];   // L
    double *in2 = ins[1];   // R
    double *in3 = ins[2];                       // speed
    
    double *out1  = outs[0]; // L
    double *out2  = outs[1]; // R
    double *outPh = syncoutlet ? outs[2] : 0;   // sync (if arg #3 is on)
    
    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osamp1, osamp2, ovdb, ovd, ovdbdif, xstart, xwin;
    double o1prev, o2prev, o1dif, o2dif, frac, fad, ramp, snramp, writeval1, writeval2, coeff1, coeff2, recin1, recin2;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, interp0, interp1, interp2, interp3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    rec             = x->rec;
    recpre          = x->recpre;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (rec || recpre)
        dirt        = 1;
    if (x->buf_mod) {
        karma_modset(x, buf);
        x->buf_mod  = 0;
    }
    
    o1prev          = x->o1prev;
    o2prev          = x->o2prev;
    o1dif           = x->o1dif;
    o2dif           = x->o2dif;
    writeval1       = x->writeval1;
    writeval2       = x->writeval2;

    go              = x->go;
    statecontrol    = x->statecontrol;
    pupdwn          = x->pupdwn;
    rupdwn          = x->rupdwn;
    rpre            = x->rpos;
    rectoo          = x->rectoo;
    nchan           = x->bchans;    //
    srscale         = x->srscale;
    frames          = x->bframes;
    trig            = x->trig;
    jnoff           = x->jnoff;
    append          = x->append;
    diro            = x->diro;
    dirp            = x->dirp;
    loop            = x->loop;
    xwin            = x->extlwin;
    looprec         = x->looprec;
    start           = x->start;
    xstart          = x->xstart;
    end             = x->end;
    doend           = x->doend;
    ovdb            = x->ovd;
    ovd             = x->ovdb;
    ovdbdif         = (ovdb != ovd) ? ((ovd - ovdb) / n) : 0.0;
    rfad            = x->rfad;
    pfad            = x->pfad;
    dpos            = x->pos;
    pos             = trunc(dpos);
    maxpos          = x->maxpos;
    wrap            = x->wrap;
    jump            = x->jump;
    numof           = x->numof;
    fad             = x->fad;
    ramp            = (double)x->ramp;
    snramp          = (double)x->snramp;
    curv            = x->curv;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: rec init
            rec = go = trig = looprec = 1;
            rfad = rupdwn = pfad = pupdwn = statecontrol = 0;
            break;
        case 2:             // case 2: rec rectoo
            doend = 3;
            rec = rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 3:             // case 3: rec off reg
            rupdwn = 1;
            pupdwn = 3;
            pfad = rfad = statecontrol = 0;
            break;
        case 4:             // case 4: play rectoo
            doend = 2;
            rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            trig = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop rectoo
            pfad = rfad = 0;
            doend = pupdwn = rupdwn = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (rec) {
                rfad = 0;
                rupdwn = 1;
            }
            pfad = 0;
            pupdwn = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (rec) {
                rfad = 0;
                rupdwn = 2;
            }
            pfad = 0;
            pupdwn = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            pupdwn = 4;
            pfad = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            rec = looprec = rectoo = 1;
            fad = 0.0;
            rfad = rupdwn = statecontrol = 0;
            break;
        case 11:            // case 11: rec on reg
            pupdwn = 3;
            rupdwn = 5;
            rfad = pfad = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'fad = 0.0' triggers switch&ramp (declick play)
    // 'rpre = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)
    
    if (nchan > 1)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = *in3++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                        }
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, fad, curv);
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            osamp2 = ease_record(osamp2, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                    osamp2 = 0.0;
                }
                
                o1prev = osamp1;
                o2prev = osamp2;
                *out1++ = osamp1;
                *out2++ = osamp2;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + (((double)b[(pos * nchan) + 1]) * ovdb), rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
                    apned:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                osamp2 = 0.0;
                o1prev = osamp1;
                o2prev = osamp2;
                *out1++ = osamp1;
                *out2++ = osamp2;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + ((double)b[(pos * nchan) + 1]) * ovdb, rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }

    }
    else
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = *in3++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // <- easing-curv options (implemented by raja)
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                }
                
                o1prev = osamp1;
                *out1++ = osamp1;
                o2prev = osamp2 = 0.0;
                *out2++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                    else
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                    
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
apnde:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                o1prev = osamp1;
                *out1++ = osamp1;
                osamp2 = 0.0;
                o2prev = osamp2 = 0.0;
                *out2++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                    else
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                    
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            }
                        }
                        writeval1 = recin1;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }
    
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->rprtime <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev       = o1prev;
    x->o2prev       = o2prev;
    x->o1dif        = o1dif;
    x->o2dif        = o2dif;
    x->writeval1    = writeval1;
    x->writeval2    = writeval2;
    
    x->maxpos       = maxpos;
    x->numof        = numof;
    x->wrap         = wrap;
    x->fad          = fad;
    x->pos          = dpos;
    x->diro         = diro;
    x->dirp         = dirp;
    x->rpos         = rpre;
    x->rectoo       = rectoo;
    x->rfad         = rfad;
    x->trig         = trig;
    x->jnoff        = jnoff;
    x->go           = go;
    x->rec          = rec;
    x->recpre       = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn       = pupdwn;
    x->rupdwn       = rupdwn;
    x->pfad         = pfad;
    x->loop         = loop;
    x->looprec      = looprec;
    x->start        = start;
    x->end          = end;
    x->ovd          = ovdb;
    x->doend        = doend;
    x->append       = append;
    
    return;

zero:
    while (n--) {
        *out1++  = 0.0;
        *out2++  = 0.0;
        if (syncoutlet)
            *outPh++ = 0.0;
    }
    
    return;
}

// quad perform

void karma_qperf(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long syncoutlet = x->syncoutlet;
    
    double *in1 = ins[0];   // channel 1
    double *in2 = ins[1];   // channel 2
    double *in3 = ins[2];   // channel 3
    double *in4 = ins[3];   // channel 4
    double *in5 = ins[4];                       // speed
    
    double *out1  = outs[0];// channel 1
    double *out2  = outs[1];// channel 2
    double *out3  = outs[2];// channel 3
    double *out4  = outs[3];// channel 4
    double *outPh = syncoutlet ? outs[4] : 0;   // sync (if arg #3 is on)
    
    int n = vcount;
    
    double dpos, maxpos, jump, srscale, sprale, rdif, numof;
    double speed, osamp1, osamp2, osamp3, osamp4, ovdb, ovd, ovdbdif, xstart, xwin;
    double o1prev, o2prev, o1dif, o2dif, o3prev, o4prev, o3dif, o4dif, frac, fad, ramp, snramp;
    double writeval1, writeval2, writeval3, writeval4, coeff1, coeff2, coeff3, coeff4, recin1, recin2, recin3, recin4;
    t_bool go, rec, recpre, rectoo, looprec, jnoff, append, dirt, wrap, trig;
    char dir, dirp, diro, statecontrol, pupdwn, rupdwn, doend;
    t_ptr_int pfad, rfad, i, interp0, interp1, interp2, interp3, frames, start, end, pos, rpre, loop, nchan, curv, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    rec             = x->rec;
    recpre          = x->recpre;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (rec || recpre)
        dirt        = 1;
    if (x->buf_mod) {
        karma_modset(x, buf);
        x->buf_mod  = 0;
    }

    o1prev          = x->o1prev;
    o2prev          = x->o2prev;
    o3prev          = x->o3prev;
    o4prev          = x->o4prev;
    o1dif           = x->o1dif;
    o2dif           = x->o2dif;
    o3dif           = x->o3dif;
    o4dif           = x->o4dif;
    writeval1       = x->writeval1;
    writeval2       = x->writeval2;
    writeval3       = x->writeval3;
    writeval4       = x->writeval4;

    go              = x->go;
    statecontrol    = x->statecontrol;
    pupdwn          = x->pupdwn;
    rupdwn          = x->rupdwn;
    rpre            = x->rpos;
    rectoo          = x->rectoo;
    nchan           = x->bchans;    //
    srscale         = x->srscale;
    frames          = x->bframes;
    trig            = x->trig;
    jnoff           = x->jnoff;
    append          = x->append;
    diro            = x->diro;
    dirp            = x->dirp;
    loop            = x->loop;
    xwin            = x->extlwin;
    looprec         = x->looprec;
    start           = x->start;
    xstart          = x->xstart;
    end             = x->end;
    doend           = x->doend;
    ovdb            = x->ovd;
    ovd             = x->ovdb;
    ovdbdif         = (ovdb != ovd) ? ((ovd - ovdb) / n) : 0.0;
    rfad            = x->rfad;
    pfad            = x->pfad;
    dpos            = x->pos;
    pos             = trunc(dpos);
    maxpos          = x->maxpos;
    wrap            = x->wrap;
    jump            = x->jump;
    numof           = x->numof;
    fad             = x->fad;
    ramp            = (double)x->ramp;
    snramp          = (double)x->snramp;
    curv            = x->curv;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: rec init
            rec = go = trig = looprec = 1;
            rfad = rupdwn = pfad = pupdwn = statecontrol = 0;
            break;
        case 2:             // case 2: rec rectoo
            doend = 3;
            rec = rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 3:             // case 3: rec off reg
            rupdwn = 1;
            pupdwn = 3;
            pfad = rfad = statecontrol = 0;
            break;
        case 4:             // case 4: play rectoo
            doend = 2;
            rupdwn = pupdwn = 1;
            pfad = rfad = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            trig = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop rectoo
            pfad = rfad = 0;
            doend = pupdwn = rupdwn = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (rec) {
                rfad = 0;
                rupdwn = 1;
            }
            pfad = 0;
            pupdwn = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (rec) {
                rfad = 0;
                rupdwn = 2;
            }
            pfad = 0;
            pupdwn = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            pupdwn = 4;
            pfad = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            rec = looprec = rectoo = 1;
            fad = 0.0;
            rfad = rupdwn = statecontrol = 0;
            break;
        case 11:            // case 11: rec on reg
            pupdwn = 3;
            rupdwn = 5;
            rfad = pfad = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'fad = 0.0' triggers switch&ramp (declick play)
    // 'rpre = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)
    
    if (nchan >= 4)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = *in5++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                        osamp3 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2]);
                        osamp4 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 3], b[(interp2 * nchan) + 3]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                            osamp3  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 2], b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2], b[(interp3 * nchan) + 2]);
                            osamp4  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 3], b[(interp1 * nchan) + 3], b[(interp2 * nchan) + 3], b[(interp3 * nchan) + 3]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                            osamp3  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 2], b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2], b[(interp3 * nchan) + 2]);
                            osamp4  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 3], b[(interp1 * nchan) + 3], b[(interp2 * nchan) + 3], b[(interp3 * nchan) + 3]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                            osamp3  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2]);
                            osamp4  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 3], b[(interp2 * nchan) + 3]);
                        }
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                                o3dif = o3prev - osamp3;
                                o4dif = o4prev - osamp4;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, fad, curv);
                            osamp3 += ease_switchramp(o3dif, fad, curv);
                            osamp4 += ease_switchramp(o4dif, fad, curv);
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            osamp2 = ease_record(osamp2, (pupdwn > 0), ramp, pfad);
                            osamp3 = ease_record(osamp3, (pupdwn > 0), ramp, pfad);
                            osamp4 = ease_record(osamp4, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                    osamp2 = 0.0;
                    osamp3 = 0.0;
                    osamp4 = 0.0;
                }
                
                o1prev = osamp1;
                o2prev = osamp2;
                o3prev = osamp3;
                o4prev = osamp4;
                *out1++ = osamp1;
                *out2++ = osamp2;
                *out3++ = osamp3;
                *out4++ = osamp4;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + (((double)b[(pos * nchan) + 1]) * ovdb), rupdwn, ramp, rfad);
                        recin3 = ease_record(recin3 + (((double)b[(pos * nchan) + 2]) * ovdb), rupdwn, ramp, rfad);
                        recin4 = ease_record(recin4 + (((double)b[(pos * nchan) + 3]) * ovdb), rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                        recin3 += ((double)b[(pos * nchan) + 2]) * ovdb;
                        recin4 += ((double)b[(pos * nchan) + 3]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        writeval4 += recin4;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            writeval3 = writeval3 / numof;
                            writeval4 = writeval4 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        b[(rpre * nchan) + 3] = writeval4;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            coeff4 = (recin4 - writeval4) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                writeval3 += coeff3;
                                writeval4 += coeff4;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                                b[(i * nchan) + 2] = writeval3;
                                b[(i * nchan) + 3] = writeval4;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            coeff4 = (recin4 - writeval4) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                writeval3 -= coeff3;
                                writeval4 -= coeff4;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                                b[(i * nchan) + 2] = writeval3;
                                b[(i * nchan) + 3] = writeval4;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                        writeval4 = recin4;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
                    apned:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                osamp2 = 0.0;
                osamp3 = 0.0;
                osamp4 = 0.0;
                o1prev = osamp1;
                o2prev = osamp2;
                o3prev = osamp3;
                o4prev = osamp4;
                *out1++ = osamp1;
                *out2++ = osamp2;
                *out3++ = osamp3;
                *out4++ = osamp4;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + ((double)b[(pos * nchan) + 1]) * ovdb, rupdwn, ramp, rfad);
                        recin3 = ease_record(recin3 + ((double)b[(pos * nchan) + 2]) * ovdb, rupdwn, ramp, rfad);
                        recin4 = ease_record(recin4 + ((double)b[(pos * nchan) + 3]) * ovdb, rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                        recin3 += ((double)b[(pos * nchan) + 2]) * ovdb;
                        recin4 += ((double)b[(pos * nchan) + 3]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        writeval4 += recin4;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            writeval3 = writeval3 / numof;
                            writeval4 = writeval4 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        b[(rpre * nchan) + 3] = writeval4;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                coeff3 = (recin3 - writeval3) / rdif;
                                coeff4 = (recin4 - writeval4) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    writeval3 += coeff3;
                                    writeval4 += coeff4;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                    b[(i * nchan) + 2] = writeval3;
                                    b[(i * nchan) + 3] = writeval4;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                coeff3 = (recin3 - writeval3) / rdif;
                                coeff4 = (recin4 - writeval4) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    writeval3 -= coeff3;
                                    writeval4 -= coeff4;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                    b[(i * nchan) + 2] = writeval3;
                                    b[(i * nchan) + 3] = writeval4;
                                }
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                        writeval4 = recin4;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }
        
    }
    else if (nchan == 3)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = *in5++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                        osamp3 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                            osamp3  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 2], b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2], b[(interp3 * nchan) + 2]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                            osamp3  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 2], b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2], b[(interp3 * nchan) + 2]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                            osamp3  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 2], b[(interp2 * nchan) + 2]);
                        }
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                                o3dif = o3prev - osamp3;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, fad, curv);
                            osamp3 += ease_switchramp(o3dif, fad, curv);
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            osamp2 = ease_record(osamp2, (pupdwn > 0), ramp, pfad);
                            osamp3 = ease_record(osamp3, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                    osamp2 = 0.0;
                    osamp3 = 0.0;
                }
                
                o1prev = osamp1;
                o2prev = osamp2;
                o3prev = osamp3;
                *out1++ = osamp1;
                *out2++ = osamp2;
                *out3++ = osamp3;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + (((double)b[(pos * nchan) + 1]) * ovdb), rupdwn, ramp, rfad);
                        recin3 = ease_record(recin3 + (((double)b[(pos * nchan) + 2]) * ovdb), rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                        recin3 += ((double)b[(pos * nchan) + 2]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            writeval3 = writeval3 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                writeval3 += coeff3;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                                b[(i * nchan) + 2] = writeval3;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                writeval3 -= coeff3;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                                b[(i * nchan) + 2] = writeval3;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apden;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
apden:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                osamp2 = 0.0;
                osamp3 = 0.0;
                o1prev = osamp1;
                o2prev = osamp2;
                o3prev = osamp3;
                *out1++ = osamp1;
                *out2++ = osamp2;
                *out3++ = osamp3;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + ((double)b[(pos * nchan) + 1]) * ovdb, rupdwn, ramp, rfad);
                        recin3 = ease_record(recin3 + ((double)b[(pos * nchan) + 2]) * ovdb, rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                        recin3 += ((double)b[(pos * nchan) + 2]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            writeval3 = writeval3 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                coeff3 = (recin3 - writeval3) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    writeval3 += coeff3;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                    b[(i * nchan) + 2] = writeval3;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                coeff3 = (recin3 - writeval3) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    writeval3 -= coeff3;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                    b[(i * nchan) + 2] = writeval3;
                                }
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;              // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                  // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }

    }
    else if (nchan == 2)
    {
        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = *in5++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * nchan) + 1], b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1], b[(interp3 * nchan) + 1]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * nchan) + 1], b[(interp2 * nchan) + 1]);
                        }
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, fad, curv);
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            osamp2 = ease_record(osamp2, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                    osamp2 = 0.0;
                }
                
                o1prev = osamp1;
                o2prev = osamp2;
                *out1++ = osamp1;
                *out2++ = osamp2;
                o3prev = osamp3 = 0.0;
                *out3++ = 0.0;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + (((double)b[(pos * nchan) + 1]) * ovdb), rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apdne;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
apdne:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                osamp2 = 0.0;
                o1prev = osamp1;
                o2prev = osamp2;
                *out1++ = osamp1;
                *out2++ = osamp2;
                o3prev = osamp3 = 0.0;
                *out3++ = 0.0;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                        recin2 = ease_record(recin2 + ((double)b[(pos * nchan) + 1]) * ovdb, rupdwn, ramp, rfad);
                    } else {
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                        recin2 += ((double)b[(pos * nchan) + 1]) * ovdb;
                    }
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            writeval2 = writeval2 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;                  // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }
    
    }
    else
    {
        
        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = *in5++;
            dir = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (dirp != dir) {
                if (rec && ramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -dir, ramp);
                    rfad = rupdwn = 0;
                    rpre = -1;
                }
                fad = 0.0;
            };              // !! !!
            
            if ((rec - recpre) < 0) {           // samp @rec-off
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, dir, ramp);
                rpre = -1;
                dirt = 1;
            } else if ((rec - recpre) > 0) {    // samp @rec-on
                rfad = rupdwn = 0;
                if (speed < 1.0)
                    fad = 0.0;
                if (ramp)
                    ease_bufoff(frames - 1, b, nchan, dpos, -dir, ramp);
            }
            recpre = rec;
            
            if (!looprec)
            {
                if (go)
                {
                    if (trig)
                    {
                        if (doend)  // calculate end of loop
                        {
                            if (diro >= 0)
                            {
                                loop = CLAMP(maxpos, 4096, frames - 1);
                                dpos = start = (xstart * loop);
                                end = start + (xwin * loop);
                                if (end > loop) {
                                    end = end - (loop + 1);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                if (dir < 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            } else {
                                loop = CLAMP((frames - 1) - maxpos, 4096, frames - 1);
                                start = ((frames - 1) - loop) + (xstart * loop);
                                dpos = end = start + (xwin * loop);
                                if (end > (frames - 1)) {
                                    end = ((frames - 1) - loop) + (end - frames);
                                    wrap = 1;
                                } else {
                                    wrap = 0;
                                }
                                dpos = end;
                                if (dir > 0) {
                                    if (ramp)
                                        ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                }
                            }
                            if (ramp)
                                ease_bufoff(frames - 1, b, nchan, maxpos, -dir, ramp);
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                            append = rectoo = doend = 0;
                        } else {    // jump / play
                            if (jnoff)
                                dpos = (diro >= 0) ? (jump * loop) : (((frames - 1) - loop) + (jump * loop));
                            else
                                dpos = (dir < 0) ? end : start;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rpre = -1;
                                rupdwn = 0;
                            }
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        
                        if (jnoff)
                        {
                            if (wrap) {
                                if ((dpos < end) || (dpos > start))
                                    jnoff = 0;
                            } else {
                                if ((dpos < end) && (dpos > start))
                                    jnoff = 0;
                            }
                            if (diro >= 0)
                            {
                                if (dpos > loop)
                                {
                                    dpos = dpos - loop;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < 0.0) {
                                    dpos = loop + dpos;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (dpos > (frames - 1))
                                {
                                    dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (dpos < ((frames - 1) - loop)) {
                                    dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrap)
                            {
                                if ((dpos > end) && (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                } else if (diro >= 0) {
                                    if (dpos > loop)
                                    {
                                        dpos = dpos - loop;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, loop, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (dpos < 0.0)
                                    {
                                        dpos = loop + dpos;
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (dpos < ((frames - 1) - loop))
                                    {
                                        dpos = (frames - 1) - (((frames - 1) - loop) - dpos);
                                        fad = 0.0;
                                        if (rec)
                                        {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - loop), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    } else if (dpos > (frames - 1)) {
                                        dpos = ((frames - 1) - loop) + (dpos - (frames - 1));
                                        fad = 0.0;
                                        if (rec) {
                                            if (ramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                                rfad = 0;
                                            }
                                            rupdwn = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((dpos > end) || (dpos < start))
                                {
                                    dpos = (dir >= 0) ? start : end;
                                    fad = 0.0;
                                    if (rec) {
                                        if (ramp) {
                                            ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                            rfad = 0;
                                        }
                                        rupdwn = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    pos = trunc(dpos);
                    if (dir > 0) {
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(pos, &interp0, &interp1, &interp2, &interp3, dir, diro, loop, frames - 1);     // find samp-indices 4 interp
                    
                    if (rec) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    }
                    
                    if (ramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (fad < 1.0)
                        {
                            if (fad == 0.0) {
                                o1dif = o1prev - osamp1;
                            }
                            osamp1 += ease_switchramp(o1dif, fad, curv);  // <- easing-curv options (implemented by raja)
                            fad += 1 / snramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (pfad < ramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (pupdwn > 0), ramp, pfad);
                            pfad++;
                            if (pfad >= ramp)
                            {
                                switch (pupdwn)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        pupdwn = go = 0;                // rec rectoo   // play rectoo  // stop rectoo / reg
                                        break;
                                    case 2:
                                        if (!rec)
                                            trig = jnoff = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // rec off reg
                                        pupdwn = pfad = 0;
                                        break;
                                    case 4:                             // append
                                        go = trig = looprec = 1;
                                        fad = 0.0;
                                        pfad = pupdwn = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (pupdwn)
                        {
                            case 0:
                                break;
                            case 1:
                                pupdwn = go = 0;
                                break;
                            case 2:
                                if (!rec)
                                    trig = jnoff = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // rec off reg
                                pupdwn = 0;
                                break;
                            case 4:                                     // append
                                go = trig = looprec = 1;
                                fad = 0.0;
                                pfad = pupdwn = 0;
                                break;
                        }
                    }
                    
                } else {
                    osamp1 = 0.0;
                }
                
                o1prev = osamp1;
                *out1++ = osamp1;
                o2prev = osamp2 = 0.0;
                *out2++ = 0.0;
                o3prev = osamp3 = 0.0;
                *out3++ = 0.0;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[pos * nchan] * ovdb), rupdwn, ramp, rfad); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[pos * nchan]) * ovdb), rupdwn, ramp, rfad);
                    else
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                    
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(pos - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre + 1; i < pos; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre - 1; i > pos; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    rpre = pos;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (ramp)                                   // realtime ramps for record on/off
                {
                    if(rfad < ramp)
                    {
                        rfad++;
                        if ((rupdwn) && (rfad >= ramp))
                        {
                            if (rupdwn == 2) {
                                trig = jnoff = 1;
                                rfad = 0;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            } else {
                                rec = 0;
                            }
                            rupdwn = 0;
                        }
                    }
                } else {
                    if (rupdwn) {
                        if (rupdwn == 2) {
                            trig = jnoff = 1;
                        } else if (rupdwn == 5) {
                            rec = 1;
                        } else {
                            rec = 0;
                        }
                        rupdwn = 0;
                    }
                }
                dirp = dir;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (trig)
                    {
                        if (jnoff)                          // jump
                        {
                            if (diro >= 0) {
                                dpos = jump * maxpos;
                            } else {
                                dpos = (frames - 1) - (((frames - 1) - maxpos) * jump);
                            }
                            jnoff = 0;
                            fad = 0.0;
                            if (rec) {
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rupdwn = 0;
                                rpre = -1;
                            }
                            trig = 0;
                        } else if (append) {                // append
                            fad = 0.0;
                            trig = 0;
                            if (rec)
                            {
                                dpos = maxpos;
                                if (ramp) {
                                    ease_bufon(frames - 1, b, nchan, dpos, rpre, dir, ramp);
                                    rfad = 0;
                                }
                                rectoo = 1;
                                rupdwn = 0;
                                rpre = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            diro = dir;
                            loop = frames - 1;
                            maxpos = dpos = (dir >= 0) ? 0.0 : (frames - 1);
                            rectoo = 1;
                            rpre = -1;
                            fad = 0.0;
                            trig = 0;
                        }
                    } else {
apnde:
                        sprale = speed * srscale;
                        if (rec)
                            sprale = (fabs(sprale) > (loop / 1024)) ? ((loop / 1024) * dir) : sprale;
                        dpos = dpos + sprale;
                        if (dir == diro)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (dpos > (frames - 1))
                            {
                                dpos = 0.0;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = frames - 1;
                            } else if (dpos < 0.0) {
                                dpos = frames - 1;
                                rec = append;
                                if (rec) {
                                    if (ramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                        rpre = -1;
                                        rupdwn = rfad = 0;
                                    }
                                }
                                doend = trig = 1;
                                looprec = rectoo = 0;
                                maxpos = 0.0;
                            } else {                        // <- track max write pos
                                if ( ((diro >= 0) && (maxpos < dpos)) || ((diro < 0) && (maxpos > dpos)) )
                                    maxpos = dpos;
                            }
                        } else if (dir < 0) {               // wraparounds for reversal while creating initial-loop
                            if (dpos < 0.0)
                            {
                                dpos = maxpos + dpos;
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        } else if (dir >= 0) {
                            if (dpos > (frames - 1))
                            {
                                dpos = maxpos + (dpos - (frames - 1));
                                if (ramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -dir, ramp);
                                    rpre = -1;
                                    rupdwn = rfad = 0;
                                }
                            }
                        }
                    }
                    
                    pos = trunc(dpos);
                    if (dir > 0) {                          // interp ratio
                        frac = dpos - pos;
                    } else if (dir < 0) {
                        frac = 1.0 - (dpos - pos);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (ramp)
                    {
                        if (pfad < ramp)                    // realtime ramps for play on/off
                        {
                            pfad++;
                            if (pupdwn)
                            {
                                if (pfad >= ramp)
                                {
                                    if (pupdwn == 2) {
                                        doend = 4;
                                        go = 1;
                                    }
                                    pupdwn = 0;
                                    switch (doend) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            pfad = 0;
                                            break;
                                        case 4:
                                            doend = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (pupdwn)
                        {
                            if (pupdwn == 2) {
                                doend = 4;
                                go = 1;
                            }
                            pupdwn = 0;
                            switch (doend) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    
                }
                
                osamp1 = 0.0;
                o1prev = osamp1;
                *out1++ = osamp1;
                osamp2 = 0.0;
                o2prev = osamp2 = 0.0;
                *out2++ = 0.0;
                osamp3 = 0.0;
                o3prev = osamp3 = 0.0;
                *out3++ = 0.0;
                osamp4 = 0.0;
                o4prev = osamp4 = 0.0;
                *out4++ = 0.0;
                if (syncoutlet)
                    *outPh++ = (diro >= 0) ? (dpos / loop) : (dpos - (frames - loop) / loop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (rec)
                {
                    if ((rfad < ramp) && (ramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[pos * nchan]) * ovdb, rupdwn, ramp, rfad);
                    else
                        recin1 += ((double)b[pos * nchan]) * ovdb;
                    
                    if (rpre < 0) {
                        rpre = pos;
                        numof = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == pos) {
                        writeval1 += recin1;
                        numof += 1.0;
                    } else {
                        if (numof > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / numof;
                            numof = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(pos - rpre);        // linear-interp for speed > 1x
                        if (dir != diro)
                        {
                            if (diro >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxpos * 0.5))
                                    {
                                        rdif -= maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxpos; i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxpos * 0.5))
                                    {
                                        rdif += maxpos;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < (maxpos + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = 0; i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= maxpos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = (frames - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxpos)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxpos));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxpos; i < pos; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > pos; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre + 1); i < pos; i++) {
                                    writeval1 += coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre - 1); i > pos; i--) {
                                    writeval1 -= coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            }
                        }
                        writeval1 = recin1;
                    }                                       // ~ipoke end
                    if (ramp)                               // realtime ramps for record on/off
                    {
                        if (rfad < ramp)
                        {
                            rfad++;
                            if ((rupdwn) && (rfad >= ramp))
                            {
                                if (rupdwn == 2) {
                                    doend = 4;
                                    trig = jnoff = 1;
                                    rfad = 0;
                                } else if (rupdwn == 5) {
                                    rec = 1;
                                }
                                rupdwn = 0;
                                switch (doend)
                                {
                                    case 0:
                                        rec = 0;
                                        break;
                                    case 1:
                                        if (diro < 0) {
                                            loop = (frames - 1) - maxpos;
                                        } else {
                                            loop = maxpos;
                                        }
                                        break;                  // !! break pete fix different !!
                                    case 2:
                                        rec = looprec = 0;
                                        trig = 1;
                                        break;
                                    case 3:
                                        rec = trig = 1;
                                        rfad = looprec = 0;
                                        break;
                                    case 4:
                                        doend = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (rupdwn)
                        {
                            if (rupdwn == 2) {
                                doend = 4;
                                trig = jnoff = 1;
                            } else if (rupdwn == 5) {
                                rec = 1;
                            }
                            rupdwn = 0;
                            switch (doend)
                            {
                                case 0:
                                    rec = 0;
                                    break;
                                case 1:
                                    if (diro < 0) {
                                        loop = (frames - 1) - maxpos;
                                    } else {
                                        loop = maxpos;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    rec = looprec = 0;
                                    trig = 1;
                                    break;
                                case 3:
                                    rec = trig = 1;
                                    looprec = 0;
                                    break;
                                case 4:
                                    doend = 0;
                                    break;
                            }
                        }
                    }
                    rpre = pos;
                    dirt = 1;
                }
                dirp = dir;
            }
            if (ovdbdif != 0.0)
                ovdb = ovdb + ovdbdif;
        }
        
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->rprtime <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev       = o1prev;
    x->o2prev       = o2prev;
    x->o3prev       = o3prev;
    x->o4prev       = o4prev;
    x->o1dif        = o1dif;
    x->o2dif        = o2dif;
    x->o3dif        = o3dif;
    x->o4dif        = o4dif;
    x->writeval1    = writeval1;
    x->writeval2    = writeval2;
    x->writeval3    = writeval3;
    x->writeval4    = writeval4;
    
    x->maxpos       = maxpos;
    x->numof        = numof;
    x->wrap         = wrap;
    x->fad          = fad;
    x->pos          = dpos;
    x->diro         = diro;
    x->dirp         = dirp;
    x->rpos         = rpre;
    x->rectoo       = rectoo;
    x->rfad         = rfad;
    x->trig         = trig;
    x->jnoff        = jnoff;
    x->go           = go;
    x->rec          = rec;
    x->recpre       = recpre;
    x->statecontrol = statecontrol;
    x->pupdwn       = pupdwn;
    x->rupdwn       = rupdwn;
    x->pfad         = pfad;
    x->loop         = loop;
    x->looprec      = looprec;
    x->start        = start;
    x->end          = end;
    x->ovd          = ovdb;
    x->doend        = doend;
    x->append       = append;
    
    return;
    
zero:
    while (n--) {
        *out1++  = 0.0;
        *out2++  = 0.0;
        *out3++  = 0.0;
        *out4++  = 0.0;
        if (syncoutlet)
            *outPh++ = 0.0;
    }
    
    return;
}

