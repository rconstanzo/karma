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
	Rodrigo is crazy
 
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
//  //  //  fix: a bunch of bugs & stuff, incl. ...
//  //  //  - scheduling and threading problem relating to inability to call multiple methods to the object ...
//  //  //  ... by using comma-separated message boxes (etc) <<-- big bug
//  //  //  - 'statehuman' (statemachine) output (including the overdub notify bug)
//  //  //  - when new loop points are set ("set" or "setloop" methods) list-output not updated correctly

//  //  //  TODO version 2.0:
//  //  //  rewrite, take multiple perform routines out and put
//  //  //  interpolation routines and ipoke out into seperate files
//  //  //  then will be able to integrate 'rubberband' and add better ipoke interpolations etc
//  //  //  also look into raja's new 'crossfade' ideas as an optional alternative to 'switch & ramp'
//  //  //  and possibly do seperate externals for different elements (e.g. karmaplay~, karmapoke~, karmaphase~, ...)


#include "stdlib.h"
#include "math.h"

#include "ext.h"            // max
#include "ext_obex.h"       // api

#include "ext_buffer.h"     // buffer~
#include "z_dsp.h"          // msp

#include "ext_atomic.h"


// Linear Interp
#define LINEAR_INTERP(f, x, y) (x + f*(y - x))
// Hermitic Cubic Interp, 4-point 3rd-order, ( James McCartney / Alex Harker )
#define CUBIC_INTERP(f, w, x, y, z) ((((0.5*(z - w) + 1.5*(x - y))*f + (w - 2.5*x + y + y - 0.5*z))*f + (0.5*(y - w)))*f + x)
// Catmull-Rom Spline Interp, 4-point 3rd-order, ( Paul Breeuwsma / Paul Bourke )
#define SPLINE_INTERP(f, w, x, y, z) (((-0.5*w + 1.5*x - 1.5*y + 0.5*z)*f*f*f) + ((w - 2.5*x + y + y - 0.5*z)*f*f) + ((-0.5*w + 0.5*y)*f) + x)
// ^^ 'SPLINE_INTERP' should be 'void inline' to save on f multiplies   // ^^                               // ^^


// karma~
typedef struct _karma {
    
    t_pxobject       ob;
    t_buffer_ref    *buf;
    t_buffer_ref    *buf_temp;      // so that 'set' errors etc do not interupt current buf playback ...
    t_symbol        *bufname;
    t_symbol        *bufname_temp;  // ...

    double  sr;             // system samplerate
    double  bsr;            // buffer samplerate
    double  bmsr;           // buffer samplerate in samples-per-millisecond
    double  srscale;        // scaling factor: buffer samplerate / system samplerate ("to scale playback speeds appropriately")
    double  vs;             // system vectorsize
    double  vsnorm;         // normalised system vectorsize

    double  o1prev;         // previous sample value of "osamp1" etc...
    double  o2prev;         // ...
    double  o3prev;
    double  o4prev;
    double  o1dif;          // (o1dif = o1prev - osamp1) etc...
    double  o2dif;          // ...
    double  o3dif;
    double  o4dif;
    double  writeval1;      // values to be written into buffer~...
    double  writeval2;      // ...after ipoke~ interpolation, overdub summing, etc...
    double  writeval3;      // ...
    double  writeval4;

    double  playhead;       // play head position in samples (double so that "capable of tracking playhead position in floating-point indices")
    double  maxhead;        // maximum playhead position that the recording has gone into the buffer~ in samples  // ditto
    double  jumphead;       // jump position (in terms of phase 0..1 of loop) <<-- of 'loop', not 'buffer~'
    double  selection;      // selection length of window ('selection') within loop set by 'window $1' message sent to object (in phase 0..1)
    double  selstart;       // start position of window ('selection') within loop set by the 'position $1' message sent to object (in phase 0..1)
    double  snrfade;        // fade counter for switch n ramp, normalised 0..1 ??
    double  overdubamp;     // overdub amplitude 0..1 set by 'overdub $1' message sent to object
    double  overdubprev;    // a 'current' overdub amount ("for smoothing overdub amp changes")

    long    syncoutlet;     // make sync outlet ? (object arg #3: 0/1 flag) <<-- TODO: switch to private @ttribute instead
//  long    boffset;        // zero indexed buffer channel # (default 0), user settable, not buffer~ queried -->> TODO
//  long    moduloout;      // modulo playback channel outputs flag, user settable, not buffer~ queried -->> TODO
    long    islooped;       // can disable/enable global looping status (rodrigo @ttribute request, TODO) (!! long ??)

    t_ptr_int   bframes;    // # of buffer frames (stereo has 2 samples per frame, etc.)
    t_ptr_int   bchans;     // number of buffer channels
    t_ptr_int   chans;      // number of audio channels choice (object arg #2: 1 / 2 / 4)

    t_ptr_int   interpflag; // playback interpolation, 0 = linear, 1 = cubic, 2 = spline (!! why is this a t_ptr_int ??)
    t_ptr_int   recordhead; // record head position in samples
    t_ptr_int   maxloop;    // the overall loop recorded so far (in samples)
    t_ptr_int   startloop;  // start position (in buffer~) of recorded loop in samples
    t_ptr_int   endloop;    // end position (in buffer~) of recorded loop in samples
    t_ptr_int   pokesteps;  // number of steps (samples) to keep track of in ipoke~ linear averaging scheme
    t_ptr_int   recordfade; // fade counter for recording in samples
    t_ptr_int   playfade;   // fade counter for playback in samples
    t_ptr_int   globalramp; // general fade time (for both recording and playback) in samples
    t_ptr_int   snrramp;    // switch n ramp time in samples ("generally much shorter than general fade time")
    t_ptr_int   snrtype;    // switch n ramp curve option choice (!! why is this a t_ptr_int ??)
    t_ptr_int   reportlist; // right list outlet report granularity in ms (!! why is this a t_ptr_int ??)
    
    char    statecontrol;   // master looper state control (not 'human state')
    char    statehuman;     // master looper state human logic (not 'statecontrol') (0=stop, 1=play, 2=record, 3=overdub, 4=append 5=initial)

    char    playfadeflag;   // playback up/down flag, 0 = fade up/in, 1 = fade down/out <<-- TODO: reverse !!
    char    recfadeflag;    // record up/down flag, 0 = fade up/in, 1 = fade down/out <<-- TODO: reverse !!
    char    recendmark;     // the flag to show that the loop is done recording and to mark the ending of it
    char    directionorig;  // original direction loop was initially recorded (if loop was initially recorded in reverse started from end-of-buffer etc)
    char    directionprev;  // previous direction (marker for directional changes to place where fades need to happen during recording)
    
    t_bool  stopallowed;    // flag, '0' if already stopped once (& init)
    t_bool  go;             // execute play ??
    t_bool  record;         // record flag
    t_bool  recordprev;     // previous record flag
    t_bool  looprecord;     // flag: "...for when object is in a recording stage that actually determines loop duration..."
    t_bool  recordalt;      // ("rectoo") ARGH ?? !! flag that selects between different types of recording for statecontrol (but what?!) ??
    t_bool  append;         // append flag ??
    t_bool  triginit;       // flag to show trigger start of initial-loop creation (?)
    t_bool  wrapflag;       // flag to show if a loop wraps around the buffer~ end / beginning
    t_bool  jumpflag;       // whether jump is 'on' or 'off' (flag to block jumps from coming too soon ??)

    t_bool  recordinit;     // initial recording ("...flag to determine whether to apply the 'record' message to initial loop recording or not")
    t_bool  initinit;       // initial initialise (raja: "...hack i used to determine whether DSP is turned on for the very first time or not")
    t_bool  initskip;       // is initialising = 0
    t_bool  buf_modified;   // buffer has been modified boolbuf_modified_temp
    t_bool  buf_mod_temp;   // temp buffer has been modified bool
    t_bool  clockgo;        // activate clock (for list outlet)

//  t_atom   datalist[7];   // !! TODO - store list ??
    void    *messout;       // list outlet pointer
    void    *tclock;        // list timer pointer

} t_karma;


void       *karma_new(t_symbol *s, short argc, t_atom *argv);
void        karma_free(t_karma *x);

void        karma_stop(t_karma *x);
void        karma_play(t_karma *x);
void        karma_record(t_karma *x);
//void      karma_selection_internal(t_karma *x, double selectionstart, double selectionlength);
void        karma_start(t_karma *x, double positionstart);

t_max_err   karma_buf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat);
t_max_err   karma_buf_not_temp(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat);

void        karma_assist(t_karma *x, void *b, long m, long a, char *s);
void        karma_buf_dblclick(t_karma *x);

void        karma_overdub(t_karma *x, double amplitude);
void        karma_window(t_karma *x, double duration);
//void      karma_setloop_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void        karma_setloop(t_karma *x, t_symbol *s, short ac, t_atom *av);

void        karma_buf_setup(t_karma *x, t_symbol *s);
void        karma_buf_modset(t_karma *x, t_buffer_obj *b);
void        karma_clock_list(t_karma *x);
//void      karma_buf_values_internal(t_karma *x, double low, double high, long loop_points_flag, t_bool caller);
//void      karma_buf_change_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void        karma_buf_change(t_karma *x, t_symbol *s, short ac, t_atom *av);
//void      karma_offset(t_karma *x, long channeloffset);   <<-- TODO

void        karma_jump(t_karma *x, double jumpposition);
void        karma_append(t_karma *x);

// dsp:
void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags);
// mono:
void karma_mono_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
// stereo:
void karma_stereo_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);
// quad:
void karma_quad_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr);


static  t_symbol    *ps_nothing, *ps_dummy, *ps_buffer_modified;
static  t_symbol    *ps_phase, *ps_samples, *ps_milliseconds;
static  t_class     *karma_class = NULL;

/*
inline double sort_double(double low, double high)  // this is bullshit
{
    return MIN(low, high), MAX(low, high);
}
*/
// easing function for recording (with ipoke)
static inline double ease_record(double y1, char updwn, double globalramp, t_ptr_int playfade)
{
    double ifup    = (1.0 - (((double)playfade) / globalramp)) * PI;
    double ifdown  = (((double)playfade) / globalramp) * PI;
    return updwn ? y1 * (0.5 * (1.0 - cos(ifup))) : y1 * (0.5 * (1.0 - cos(ifdown)));
}

// easing function for switch & ramp
static inline double ease_switchramp(double y1, double snrfade, t_ptr_int snrtype)
{
    switch (snrtype)
    {
        case 0: y1  = y1 * (1.0 - snrfade);                                             // case 0 = linear
            break;
        case 1: y1  = y1 * (1.0 - (sin((snrfade - 1) * PI/2) + 1));                     // case 1 = sine ease in
            break;
        case 2: y1  = y1 * (1.0 - (snrfade * snrfade * snrfade));                       // case 2 = cubic ease in
            break;
        case 3: snrfade = snrfade - 1;
            y1  = y1 * (1.0 - (snrfade * snrfade * snrfade + 1));                       // case 3 = cubic ease out
            break;
        case 4: snrfade = (snrfade == 0.0) ? snrfade : pow(2, (10 * (snrfade - 1)));
            y1  = y1 * (1.0 - snrfade);                                                 // case 4 = exponential ease in
            break;
        case 5: snrfade = (snrfade == 1.0) ? snrfade : (1 - pow(2, (-10 * snrfade)));
            y1  = y1 * (1.0 - snrfade);                                                 // case 5 = exponential ease out
            break;
        case 6: if ((snrfade > 0) && (snrfade < 0.5))
            y1 = y1 * (1.0 - (0.5 * pow(2, ((20 * snrfade) - 10))));
        else if ((snrfade < 1) && (snrfade > 0.5))
            y1 = y1 * (1.0 - (-0.5 * pow(2, ((-20 * snrfade) + 10)) + 1));              // case 6 = exponential ease in/out
            break;
    }
    return y1;
}

// easing function for buffer read
static inline void ease_bufoff(t_ptr_int frms, float *b, t_ptr_int nchn, t_ptr_int mrk, char dr, double globalramp)
{
    long i, fadpos;
    
    for (i = 0; i < globalramp; i++)
    {
        fadpos = mrk + (dr * i);
        
        if ( !((fadpos < 0) || (fadpos > frms)) )
        {
            b[fadpos * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// easing function for buffer write
static inline void ease_bufon(t_ptr_int frms, float *b, t_ptr_int nchn, t_ptr_int mrk, t_ptr_int mrk2, char dr, double globalramp)
{
    long i, fadpos, fadpos2, fadpos3;
    
    for (i = 0; i < globalramp; i++)
    {
        fadpos  = (mrk  + (-dr)) + (-dr * i);
        fadpos2 = (mrk2 + (-dr)) + (-dr * i);
        fadpos3 =  mrk2 + (dr * i);
        
        if ( !((fadpos < 0) || (fadpos > frms)) )
        {
            b[fadpos * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos2 < 0) || (fadpos2 > frms)) )
        {
            b[fadpos2 * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos2 * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos2 * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos2 * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos3 < 0) || (fadpos3 > frms)) )
        {
            b[fadpos3 * nchn] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (nchn > 1)
            {
                b[(fadpos3 * nchn) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (nchn > 2)
                {
                    b[(fadpos3 * nchn) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (nchn > 3)
                    {
                        b[(fadpos3 * nchn) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// interpolation points
static inline void interp_index(t_ptr_int playhead, t_ptr_int *indx0, t_ptr_int *indx1, t_ptr_int *indx2, t_ptr_int *indx3, char direct, char directionorig, t_ptr_int maxloop, t_ptr_int framesm1)
{
    *indx0 = playhead - direct;                                   // calc of indecies 4 interps
    
    if (directionorig >= 0) {
        if (*indx0 < 0) {
            *indx0 = (maxloop + 1) + *indx0;
        } else if (*indx0 > maxloop) {
            *indx0 = *indx0 - (maxloop + 1);
        }
    } else {
        if(*indx0 < (framesm1 - maxloop)) {
            *indx0 = framesm1 - ((framesm1 - maxloop) - *indx0);
        } else if (*indx0 > framesm1) {
            *indx0 = (framesm1 - maxloop) + (*indx0 - framesm1);
        }
    }
    
    *indx1 = playhead;
    *indx2 = playhead + direct;
    
    if (directionorig >= 0) {
        if (*indx2 < 0) {
            *indx2 = (maxloop + 1) + *indx2;
        } else if (*indx2 > maxloop) {
            *indx2 = *indx2 - (maxloop + 1);
        }
    } else {
        if (*indx2 < (framesm1 - maxloop)) {
            *indx2 = framesm1 - ((framesm1 - maxloop) - *indx2);
        } else if (*indx2 > framesm1) {
            *indx2 = (framesm1 - maxloop) + (*indx2 - framesm1);
        }
    }
    
    *indx3 = *indx2 + direct;
    
    if (directionorig >= 0) {
        if(*indx3 < 0) {
            *indx3 = (maxloop + 1) + *indx3;
        } else if (*indx3 > maxloop) {
            *indx3 = *indx3 - (maxloop + 1);
        }
    } else {
        if (*indx3 < (framesm1 - maxloop)) {
            *indx3 = framesm1 - ((framesm1 - maxloop) - *indx3);
        } else if (*indx3 > framesm1) {
            *indx3 = (framesm1 - maxloop) + (*indx3 - framesm1);
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
    class_addmethod(c, (method)karma_window,    "window",   A_FLOAT,    0);     // !! change to "selection" ??
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
    // @method setloop @digest set <o>karma~</o> loop points (not 'window')
    // @description points (start/end) for setting <o>karma~</o> loop in selected buffer~ (not 'window') <br />
    // "setloop" with no args sets loop to entire buffer~ length
    // @marg 0 @name loop_start_point @optional 1 @type float
    // @marg 1 @name loop_end_point @optional 1 @type float
    // @marg 2 @name points_type @optional 1 @type symbol
    class_addmethod(c, (method)karma_setloop,   "setloop",  A_GIMME,    0);
    // @method set @digest set (new) buffer
    // @description set (new) buffer for recording or playback, can switch buffers in realtime <br />
    // "set buffer_name" with no other args sets (new) buffer~ and (re)sets loop points to entire buffer~ length
    // @marg 0 @name buffer_name @optional 0 @type symbol
    // @marg 1 @name loop_start_point @optional 1 @type float
    // @marg 2 @name loop_end_point @optional 1 @type float
    // @marg 3 @name points_type @optional 1 @type symbol
    class_addmethod(c, (method)karma_buf_change, "set",     A_GIMME,    0);
    // @method offset @digest offset referenced buffer~ channel
    // @description offset referenced buffer~ channel, zero-indexed, default 0 (no offset) <br />
    // @marg 0 @name offset @optional 0 @type int
//  class_addmethod(c, (method)karma_offset,    "offset",   A_LONG,     0);
    // @method overdub @digest overdubbing amplitude
    // @description amplitude (0..1) for when in overdubbing state <br />
    // @marg 0 @name overdub @optional 0 @type float
    class_addmethod(c, (method)karma_overdub,   "overdub",  A_FLOAT,    0);
    
    class_addmethod(c, (method)karma_dsp64,             "dsp64",    A_CANT, 0);
    class_addmethod(c, (method)karma_assist,            "assist",   A_CANT, 0);
    class_addmethod(c, (method)karma_buf_dblclick,      "dblclick", A_CANT, 0);
    class_addmethod(c, (method)karma_buf_notify,        "notify",   A_CANT, 0);
    class_addmethod(c, (method)karma_buf_not_temp,      "notify",   A_CANT, 0);

    CLASS_ATTR_LONG(c, "report", 0, t_karma, reportlist);       // !! change to "reporttime" or "listreport" or "listinterval" ??
    CLASS_ATTR_FILTER_MIN(c, "report", 0);
    CLASS_ATTR_LABEL(c, "report", 0, "Report Time (ms) for data outlet");
    // @description Set in <m>integer</m> values. Report time granualarity in <b>ms</b> for final data outlet. Default <b>50 ms</b> <br />
    
    CLASS_ATTR_LONG(c, "ramp", 0, t_karma, globalramp);         // !! change this to ms input !!    // !! change to "[something else]" ??
    CLASS_ATTR_FILTER_CLIP(c, "ramp", 0, 2048);
    CLASS_ATTR_LABEL(c, "ramp", 0, "Ramp Time (samples)");
    // @description Set in <m>integer</m> values. Ramp time in <b>samples</b> for <m>play</m>/<m>record</m> fades. Default <b>256 samples</b> <br />
    
    CLASS_ATTR_LONG(c, "snramp", 0, t_karma, snrramp);          // !! change this to ms input !!    // !! change to "[something else]" ??
    CLASS_ATTR_FILTER_CLIP(c, "snramp", 0, 2048);
    CLASS_ATTR_LABEL(c, "snramp", 0, "Switch&Ramp Time (samples)");
    // @description Set in <m>integer</m> values. Ramp time in <b>samples</b> for <b>switch &amp; ramp</b> type dynamic fades. Default <b>256 samples</b> <br />
    
    CLASS_ATTR_LONG(c, "snrcurv", 0, t_karma, snrtype);         // !! change to "[something else]" ??
    CLASS_ATTR_FILTER_CLIP(c, "snrcurv", 0, 6);
    CLASS_ATTR_ENUMINDEX(c, "snrcurv", 0, "Linear Sine_In Cubic_In Cubic_Out Exp_In Exp_Out Exp_In_Out");
    CLASS_ATTR_LABEL(c, "snrcurv", 0, "Switch&Ramp Curve");
    // @description Type of <b>curve</b> used in <b>switch &amp; ramp</b> type dynamic fades. Default <b>Sine_In</b> <br />
    
    CLASS_ATTR_LONG(c, "interp", 0, t_karma, interpflag);       // !! change to "playinterp" ??
    CLASS_ATTR_FILTER_CLIP(c, "interp", 0, 2);
    CLASS_ATTR_ENUMINDEX(c, "interp", 0, "Linear Cubic Spline");
    CLASS_ATTR_LABEL(c, "interp", 0, "Playback Interpolation");
    // @description Type of <b>interpolation</b> used in audio playback. Default <b>Cubic</b> <br />
    
    CLASS_ATTR_LONG(c, "loop", 0, t_karma, islooped);           // !! TODO !!
    CLASS_ATTR_FILTER_CLIP(c, "loop", 0, 1);
    CLASS_ATTR_LABEL(c, "loop", 0, "Loop off / on");
    // @description Set as <m>integer</m> flag <b>0</b> or <b>1</b>. With <m>loop</m> switched on, <o>karma~</o> acts as a nornal looper, looping playback and/or recording depending on the state machine. With <m>loop</m> switched off, <o>karma~</o> will only play or record in oneshots. Default <b>On (1)</b> <br />
/*
    CLASS_ATTR_LONG(c, "modout", 0, t_karma, moduloout);        // !! TODO !!
    CLASS_ATTR_FILTER_CLIP(c, "modout", 0, 1);
    CLASS_ATTR_LABEL(c, "modout", 0, "Modulo playback channel outputs off / on");
    // @description Set as <m>integer</m> flag <b>0</b> or <b>1</b>. With <m>modout</m> switched on, <o>karma~</o> will fill all output channels with audio even if the <o>buffer~</o> (or <o>buffer~</o> as a result of <m>offset</m>) has less available channels. With <m>modout</m> switched off, <o>karma~</o> will silence any unused channels. Like the same message to the <o>sfplay~</o> object. Default <b>Off (0)</b> <br />
*/
    class_dspinit(c);
    class_register(CLASS_BOX, c);
    karma_class = c;
    
    ps_nothing = gensym("");
    ps_dummy = gensym("dummy");
    ps_buffer_modified = gensym("buffer_modified");
    
    ps_phase = gensym("phase");
    ps_samples = gensym("samples");
    ps_milliseconds = gensym("milliseconds");   // !! should be "ms" eventually

    // identify build
    post("-- karma~:");
    post("version 1.5 beta");
    post("designed by Rodrigo Constanzo");
    post("original version to 1.4 developed and coded by raja");
    post("1.5 updates coded by pete");
    post("--");
}

void *karma_new(t_symbol *s, short argc, t_atom *argv)
{
    t_karma *x;
    t_symbol *bufname = 0;
    long syncoutlet = 0;
    t_ptr_int chans = 0;
    t_ptr_int attrstart = attr_args_offset(argc, argv);
    
    x = (t_karma *)object_alloc(karma_class);
    x->initskip = 0;

    // !! TODO: argument checks !!
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
/*  } else {
        object_error((t_object *)x, "karma~ will not load without an associated buffer~ declaration");
        goto zero;
*/  }

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
        
        x->recordhead = -1;
        x->reportlist = 50;
        x->snrramp = x->globalramp = 256;
        x->playfade = x->recordfade = 257;  // ??
        x->sr = sys_getsr();
        x->vs = sys_getblksize();
        x->vsnorm = x->vs / x->sr;
        x->overdubprev = x->overdubamp = 1.0;
        x->islooped = x->snrtype = x->interpflag = 1;
        x->playfadeflag = x->recfadeflag = x->recordinit = x->initinit = x->append = x->jumpflag = x->statecontrol = x->statehuman = x->stopallowed = 0;
        x->directionprev = x->directionorig = x->recordprev = x->record = x->recordalt = x->recendmark = x->go = x->triginit = 0;
        x->pokesteps = x->writeval1 = x->writeval2 = x->writeval3 = x->writeval4 = x->wrapflag = x->looprecord = 0;
        x->maxhead = x->playhead = 0.0;
        x->selstart = x->jumphead = x->snrfade = x->o1dif = x->o2dif = x->o3dif = x->o4dif = x->o1prev = x->o2prev = x->o3prev = x->o4prev = 0.0;
        
        if (bufname != 0)
            x->bufname = bufname;
/*      else
            object_error((t_object *)x, "requires an associated buffer~ declaration");
*/
        x->syncoutlet = syncoutlet;
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

        x->messout = listout(x);    // data
        x->tclock = clock_new((t_object * )x, (method)karma_clock_list);
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

        x->initskip = 1;
        x->ob.z_misc |= Z_NO_INPLACE;
    }

//zero:
    return (x);
}

void karma_free(t_karma *x)
{
    if (x->initskip) {
        dsp_free((t_pxobject *)x);

        object_free(x->buf);
        object_free(x->buf_temp);
        
        object_free(x->tclock);
        object_free(x->messout);
    }
}

void karma_buf_dblclick(t_karma *x)
{
    buffer_view(buffer_ref_getobject(x->buf));
}

// called in 'karma_dsp64' method
void karma_buf_setup(t_karma *x, t_symbol *s)
{
    t_buffer_obj *buf;
    x->bufname = s;
    
    if (!x->buf)
        x->buf = buffer_ref_new((t_object *)x, s);
    else
        buffer_ref_set(x->buf, s);
    
    buf = buffer_ref_getobject(x->buf);
    
/*    if (buf == NULL) {
        x->buf = 0;
        object_error((t_object *)x, "there is no buffer~ named %s", s->s_name);
    } else {
*/  if (buf != NULL) {
        x->directionorig = 0;
        x->maxhead = x->playhead    = 0.0;
        x->recordhead               = -1;
        x->bchans   = buffer_getchannelcount(buf);
        x->bframes  = buffer_getframecount(buf);
        x->bmsr     = buffer_getmillisamplerate(buf);
        x->bsr      = buffer_getsamplerate(buf);
        x->srscale                  = x->bsr / x->sr;
        x->selstart = x->startloop  = 0.0;
        x->selection                = 1.0;
        x->maxloop = x->endloop     = (x->bframes - 1);

    }
}

// called on buffer modified notification
void karma_buf_modset(t_karma *x, t_buffer_obj *b)
{
    double      bsr, bmsr;
    t_ptr_int   chans;
    t_ptr_int   frames;
    
    if (b) {
        bsr     = buffer_getsamplerate(b);
        chans   = buffer_getchannelcount(b);
        frames  = buffer_getframecount(b);
        bmsr    = buffer_getmillisamplerate(b);
        
        if (((x->bchans != chans) || (x->bframes != frames)) || (x->bmsr != bmsr)) {
            x->bmsr                     = bmsr;
            x->bsr                      = bsr;
            x->srscale                  = bsr / x->sr;
            x->bframes                  = frames;
            x->bchans                   = chans;
            x->startloop                = 0.;
            x->maxloop = x->endloop     = (x->bframes - 1);

            karma_window(x, x->selection);
            karma_start(x, x->selstart);
            
            post("modset called");      // dev
        }
    }
}

// called by 'karma_buf_change_internal' & 'karma_setloop_internal'
void karma_buf_values_internal(t_karma *x, double templow, double temphigh, long loop_points_flag, t_bool caller)
{
    t_buffer_obj *buf;
//  t_symbol *loop_points_sym = 0;                      // dev
    double bframesm1, bframesms, bvsnorm, bvsnorm05;    // bframesm1 not needed ?, bframesms is bollox ?
    double low, lowtemp, high, hightemp;
    low = templow;
    high = temphigh;

    if (caller) {                                       // only if called from 'karma_buf_change_internal'
        buf         = buffer_ref_getobject(x->buf);

        x->bchans   = buffer_getchannelcount(buf);
        x->bframes  = buffer_getframecount(buf);
        x->bmsr     = buffer_getmillisamplerate(buf);
        x->bsr      = buffer_getsamplerate(buf);        // ...
        x->srscale  = x->bsr / x->sr;                   // ... !! need to account for channels !!
    }

    bframesm1   = (double)(x->bframes - 1);
    bframesms   = bframesm1 / x->bmsr * x->srscale;     // buffersize in milliseconds
    bvsnorm     = x->vsnorm * (x->bsr / bframesm1);     // vectorsize in (double) % 0..1 phase units of buffer~
    bvsnorm05   = bvsnorm * 0.5;                        // half vectorsize (normalised)
    
    // by this stage in routine, if LOW < 0., it has not been set and should be set to default (0.) regardless of 'loop_points_flag'
    if (low < 0.)
        low = 0.;
    
    if (loop_points_flag == 0) {            // if PHASE
        // by this stage in routine, if HIGH < 0., it has not been set and should be set to default (the maximum phase 1.)
        if (high < 0.)
            high = 1.;                                  // already normalised 0..1
        
        // templow already treated as phase 0..1
    } else if (loop_points_flag == 1) {     // if SAMPLES
        // by this stage in routine, if HIGH < 0., it has not been set and should be set to default (the maximum phase 1.)
        if (high < 0.)
            high = 1.;                                  // already normalised 0..1
        else
            high = high / bframesm1;                    // normalise samples high 0..1..
        
        if (low > 0.)
            low = low / bframesm1;                      // normalise samples low 0..1..
    } else {                                // if MILLISECONDS (default)
        // by this stage in routine, if HIGH < 0., it has not been set and should be set to default (the maximum phase 1.)
        if (high < 0.)
            high = 1.;                                  // already normalised 0..1
        else
            high = high / bframesms;                    // normalise milliseconds high 0..1..
        
        if (low > 0.)
            low = low / bframesms;                      // normalise milliseconds low 0..1..
    }
    
    // !! treated as normalised 0..1 from here on ... min/max & check & clamp once normalisation has occurred
    lowtemp = low;
    hightemp = high;
    low     = MIN(lowtemp, hightemp);                   // low, high = sort_double(low, high);
    high    = MAX(lowtemp, hightemp);

    if (low > 1.) {                                     // already sorted (minmax), so if this is the case we know we are fucked
        object_warn((t_object *) x, "loop minimum cannot be greater than available buffer~ size, setting to buffer~ size minus vectorsize");
        low = 1. - bvsnorm;
    }
    if (high > 1.) {
        object_warn((t_object *) x, "loop maximum cannot be greater than available buffer~ size, setting to buffer~ size");
        high = 1.;
    }

    // finally check for minimum loop-size ...
    if ( (high - low) < bvsnorm ) {
        object_warn((t_object *) x, "loop size cannot be this small, minimum is vectorsize internally (currently using %.0f samples)", x->vs);
        if ( (low - bvsnorm05) < 0. ) {
            low = 0.;
            high = bvsnorm;
        } else if ( (high + bvsnorm05) > 1. ) {
            high = 1.;
            low = 1. - bvsnorm;
        } else {
            low = low - bvsnorm05;
            high = high + bvsnorm05;
        }
    }
    low     = CLAMP(low, 0., 1.);
    high    = CLAMP(high, 0., 1.);

    // !! now we should update additional data for list outlet ??
/*
    // dev
    loop_points_sym = (loop_points_flag > 1) ? ps_milliseconds : ((loop_points_flag < 1) ? ps_phase : ps_samples);
    post("loop start normalised %.2f, loop end normalised %.2f, units %s", low, high, *loop_points_sym);
    //post("loop start samples %.2f, loop end samples %.2f, units used %s", (low * bframesm1), (high * bframesm1), *loop_points_sym);
*/
    // regardless of input choice ('loop_points_flag'), final system is normalised 0..1 (phase)

    x->startloop = low * bframesm1;
    x->maxloop = x->endloop = high * bframesm1;

    // update selection only if no additional args (min/max low/high) (thanks raja, i am an idiot !!)
    if ( (low <= 0.) && (high >= 1.) ) {
        karma_start(x, x->selstart);                                // !! raja had _start / _window reversed here !!
        karma_window(x, x->selection);
        //karma_selection_internal(x, x->selstart, x->selection);   // !! TODO
    }
}

// karma_buf_change method defered
// pete says: i know this branching is horrible, will rewrite soon...
void karma_buf_change_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv)   // " set ..... "
{
    t_bool callerid;
    t_buffer_obj *buf_temp;
    t_symbol *b;
    t_symbol *b_temp = 0;
    t_symbol *loop_points_sym = 0;
    long loop_points_flag;              // specify start/end loop points: 0 = in phase, 1 = in samples, 2 = in milliseconds (default)
    double templow, temphigh, temphightemp;

    // error check .....

    b_temp  = atom_getsym(argv + 0);    // already arg checked to be A_SYM in karma_buf_change
    
    if (b_temp == ps_nothing)
    {
        object_error((t_object *)x, "%s requires a valid buffer~ declaration (none found)", s->s_name);
        return;

    } else {
        // !! this "buf_temp" assignment is to check for a valid buffer, so that karma~ playback...
        // ...continues with main assigned "buf" even if symbol given is in error !!
        // this is slow (& expensive ??) - there must be a better way ??
        
        // use 'buffer_ref_exists()' instead ??
        
        x->bufname_temp = b_temp;
        
        if (!x->buf_temp)
            x->buf_temp = buffer_ref_new((t_object *)x, b_temp);
        else                            // this should never get called here ??
            buffer_ref_set(x->buf_temp, b_temp);
        
        buf_temp = buffer_ref_getobject(x->buf_temp);
        
        if (buf_temp == NULL) {
            
            object_warn((t_object *)x, "cannot find any buffer~ named %s, ignoring", b_temp->s_name);
            x->buf_temp = 0;            // should dspfree the temp buffer here ??
            return;

        } else {
            
            x->buf_temp = 0;            // should dspfree the temp buffer here ??
            callerid = true;            // identify caller of 'karma_buf_values_internal'
            
            b  = atom_getsym(argv + 0); // already arg checked

            x->bufname = b;
            
            if (!x->buf) {
                x->buf = buffer_ref_new((t_object *)x, b);
                x->directionorig = 0;
            } else {
                buffer_ref_set(x->buf, b);
            }

            // !! if just "set [buffername]" with no additional args and [buffername]...
            // ...already set, message will reset loop points to min / max !!
            loop_points_flag = 2;
            templow = -1.0;
            temphigh = -1.0;

    // ..... do it .....

            if (x->stopallowed == 0) {      // these should only be (re)set here if karma~ not currently playing ??
                x->directionorig = 0;
                x->maxhead = x->playhead = 0.0;
            } else {
                x->maxhead = x->playhead;   // !! we want a 'takeover' mode...
            }
            x->recordhead = -1;

            // maximum length message (4[6] atoms after 'set') = " set ...
            // ... 0::symbol::buffername [1::float::loop start] [2::float::loop end] [3::symbol::loop points type] ...
            // ... [4::symbol::offset 5::int::channel # offset] "   // <<-- not implemented "offset n" yet

            if (argc >= 4) {
                
                if (atom_gettype(argv + 3) == A_SYM) {
                    loop_points_sym = atom_getsym(argv + 3);
                    if (loop_points_sym == ps_dummy)    // !! "dummy" is silent++, move on...
                        loop_points_flag = 2;
                    else if ( (loop_points_sym == ps_phase) || (loop_points_sym == gensym("PHASE")) || (loop_points_sym == gensym("ph")) )// phase
                        loop_points_flag = 0;
                    else if ( (loop_points_sym == ps_samples) || (loop_points_sym == gensym("SAMPLES")) || (loop_points_sym == gensym("samps")) )// samps
                        loop_points_flag = 1;
                    else                                // ms or anything
                        loop_points_flag = 2;
                } else if (atom_gettype(argv + 3) == A_LONG) {      // can just be int 0..2
                    loop_points_flag = atom_getlong(argv + 3);
                } else if (atom_gettype(argv + 3) == A_FLOAT) {     // convert if error float
                    loop_points_flag = (long)atom_getfloat(argv + 3);
                } else {
                    object_warn((t_object *) x, "%s message does not understand arg no.4, using milliseconds for args 2 & 3", s->s_name);
                    loop_points_flag = 2;               // default ms
                }

                loop_points_flag = CLAMP(loop_points_flag, 0, 2);

            }
            
            if (argc >= 3) {
                
                if (atom_gettype(argv + 2) == A_FLOAT) {
                    temphigh = atom_getfloat(argv + 2);
                    if (temphigh < 0.) {
                        object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
                        //temphigh = 0.;
                    }   // !! do maximum check in karma_buf_values_internal !!
                } else if (atom_gettype(argv + 2) == A_LONG) {
                    temphigh = (double)atom_getlong(argv + 2);
                    if (temphigh < 0.) {
                        object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
                        //temphigh = 0.;
                    }   // !! do maximum check in karma_buf_values_internal !!
                } else if ( (atom_gettype(argv + 2) == A_SYM) && (argc < 4) ) {
                    //temphigh = -1.;
                    loop_points_sym = atom_getsym(argv + 2);
                    if (loop_points_sym == ps_dummy)    // !! "dummy" is silent++, move on...
                        loop_points_flag = 2;
                    else if ( (loop_points_sym == ps_phase) || (loop_points_sym == gensym("PHASE")) || (loop_points_sym == gensym("ph")) )// phase
                            loop_points_flag = 0;
                    else if ( (loop_points_sym == ps_samples) || (loop_points_sym == gensym("SAMPLES")) || (loop_points_sym == gensym("samps")) )// samps
                            loop_points_flag = 1;
                    else if ( (loop_points_sym == ps_milliseconds) || (loop_points_sym == gensym("MS")) || (loop_points_sym == gensym("ms")) )// ms
                            loop_points_flag = 2;
                    else {
                        object_warn((t_object *) x, "%s message does not understand arg no.3, setting to milliseconds", s->s_name);
                        loop_points_flag = 2;
                    }
                } else {
                    //temphigh = -1.;
                    object_warn((t_object *) x, "%s message does not understand arg no.3, setting unit to maximum", s->s_name);
                }
            }
                
            if (argc >= 2) {

                if (atom_gettype(argv + 1) == A_FLOAT) {
                    if (temphigh < 0.) {
                        temphightemp = temphigh;
                        temphigh = atom_getfloat(argv + 1);
                        templow = temphightemp;
                    } else {
                        templow = atom_getfloat(argv + 1);
                        if (templow < 0.) {
                            object_warn((t_object *) x, "loop minimum cannot be less than 0., setting to 0.");
                            templow = 0.;
                        }   // !! do maximum check in karma_buf_values_internal !!
                    }
                } else if (atom_gettype(argv + 1) == A_LONG) {
                    if (temphigh < 0.) {
                        temphightemp = temphigh;
                        temphigh = (double)atom_getlong(argv + 1);
                        templow = temphightemp;
                    } else {
                        templow = (double)atom_getlong(argv + 1);
                        if (templow < 0.) {
                            object_warn((t_object *) x, "loop minimum cannot be less than 0., setting to 0.");
                            templow = 0.;
                        }   // !! do maximum check in karma_buf_values_internal !!
                    }
                } else if (atom_gettype(argv + 1) == A_SYM) {
                    loop_points_sym = atom_getsym(argv + 1);
                    if (loop_points_sym == ps_dummy)    // !! "dummy" is silent++, move on...
                        loop_points_flag = 2;           // default ms
                    else
                        object_warn((t_object *) x, "%s message does not understand arg no.2, setting loop points to minimum (and maximum)", s->s_name);
                } else {
                    //temphigh = -1.;                   // !! no - leave temphigh alone in case only arg #2 is an error
                    //templow = -1.;
                    object_warn((t_object *) x, "%s message does not understand arg no.2, setting loop points to minimum (and maximum)", s->s_name);
                }
                
            }
/*
            // dev
            post("%s message: buffer~ %s", s->s_name, b->s_name);
*/
            karma_buf_values_internal(x, templow, temphigh, loop_points_flag, callerid);

            //x->buf_temp = 0;                          // should (dsp)free here and not earlier ??

        }

    }

}

void karma_buf_change(t_karma *x, t_symbol *s, short ac, t_atom *av)    // " set ..... "
{
    t_atom      store_av[4];
    short       i, j, a;
    a         = ac;

    // if error return...
    
    if (a <= 0) {
        object_error((t_object *) x, "%s message must be followed by argument(s) (it does nothing alone)", s->s_name);
        return;
    }
    
    if (atom_gettype(av + 0) != A_SYM) {
        object_error((t_object *)x, "first argument to %s message must be a symbol (associated buffer~ name)", s->s_name);
        return;
    }
    
    // ...else pass and defer
    
    if (a > 4) {

        object_warn((t_object *) x, "too many arguments for %s message, truncating to first four args", s->s_name);
        a   = 4;

        for (i = 0; i < a; i++) {
            store_av[i] = av[i];
        }

    } else {                        // this is a shame, how do i pass a t_atom without knowing # of atoms ?
        
        for (i = 0; i < a; i++) {
            store_av[i] = av[i];
        }
        
        for (j = i; j < 4; j++) {
            atom_setsym(store_av + j, ps_dummy);
        }

    }

    defer(x, (method)karma_buf_change_internal, s, ac, store_av);       // main method

}

// karma_setloop method defered
// pete says: i know this branching is horrible, will rewrite soon...
void karma_setloop_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv)   // " setloop ..... "
{
    t_bool callerid = false;                // identify caller of 'karma_buf_values_internal'
    t_symbol *loop_points_sym = 0;
    long loop_points_flag;                  // specify start/end loop points: 0 = in phase, 1 = in samples, 2 = in milliseconds (default)
    double templow, temphigh, temphightemp;

    // !! if just "setloop" with no additional args...
    // ...message will reset loop points to min / max !!
    loop_points_flag = 2;
    templow = -1.;
    temphigh = -1.;
    
    // maximum length message (3 atoms after 'setloop') = " setloop ...
    // ... 0::float::loop start/size [1::float::loop end] [2::symbol::loop points type] "
    
    if (argc >= 3) {
        
        if (argc > 3)
            object_warn((t_object *) x, "too many arguments for %s message, truncating to first three args", s->s_name);
        
        if (atom_gettype(argv + 2) == A_SYM) {
            loop_points_sym = atom_getsym(argv + 2);
            if ( (loop_points_sym == ps_phase) || (loop_points_sym == gensym("PHASE")) || (loop_points_sym == gensym("ph")) )// phase
                loop_points_flag = 0;
            else if ( (loop_points_sym == ps_samples) || (loop_points_sym == gensym("SAMPLES")) || (loop_points_sym == gensym("samps")) )// samps
                loop_points_flag = 1;
            else                                            // ms / anything
                loop_points_flag = 2;
        } else if (atom_gettype(argv + 2) == A_LONG) {      // can just be int
            loop_points_flag = atom_getlong(argv + 2);
        } else if (atom_gettype(argv + 2) == A_FLOAT) {     // convert if error float
            loop_points_flag = (long)atom_getfloat(argv + 2);
        } else {
            object_warn((t_object *) x, "%s message does not understand arg no.3, using milliseconds for args 1 & 2", s->s_name);
            loop_points_flag = 2;                           // default ms
        }
        
        loop_points_flag = CLAMP(loop_points_flag, 0, 2);
        
    }
    
    if (argc >= 2) {
        
        if (atom_gettype(argv + 1) == A_FLOAT) {
            temphigh = atom_getfloat(argv + 1);
            if (temphigh < 0.) {
                object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
                //temphigh = 0.;
            }   // !! do maximum check in karma_buf_values_internal !!
        } else if (atom_gettype(argv + 1) == A_LONG) {
            temphigh = (double)atom_getlong(argv + 1);
            if (temphigh < 0.) {
                object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
                //temphigh = 0.;
            }   // !! do maximum check in karma_buf_values_internal !!
        } else if ( (atom_gettype(argv + 1) == A_SYM) && (argc < 3) ) {
            //temphigh = -1.;
            loop_points_sym = atom_getsym(argv + 1);
            if ( (loop_points_sym == ps_phase) || (loop_points_sym == gensym("PHASE")) || (loop_points_sym == gensym("ph")) )// phase
                loop_points_flag = 0;
            else if ( (loop_points_sym == ps_samples) || (loop_points_sym == gensym("SAMPLES")) || (loop_points_sym == gensym("samps")) )// samps
                loop_points_flag = 1;
            else if ( (loop_points_sym == ps_milliseconds) || (loop_points_sym == gensym("MS")) || (loop_points_sym == gensym("ms")) )// ms
                loop_points_flag = 2;
            else {
                object_warn((t_object *) x, "%s message does not understand arg no.2, setting to milliseconds", s->s_name);
                loop_points_flag = 2;
            }
        } else {
            //temphigh = -1.;
            object_warn((t_object *) x, "%s message does not understand arg no.2, setting unit to maximum", s->s_name);
        }
    }
    
    if (argc >= 1) {
        
        if (atom_gettype(argv + 0) == A_FLOAT) {
            if (temphigh < 0.) {
                temphightemp = temphigh;
                temphigh = atom_getfloat(argv + 0);
                templow = temphightemp;
            } else {
                templow = atom_getfloat(argv + 0);
                if (templow < 0.) {
                    object_warn((t_object *) x, "loop minimum cannot be less than 0., setting to 0.");
                    templow = 0.;
                }   // !! do maximum check in karma_buf_values_internal !!
            }
        } else if (atom_gettype(argv + 0) == A_LONG) {
            if (temphigh < 0.) {
                temphightemp = temphigh;
                temphigh = (double)atom_getlong(argv + 0);
                templow = temphightemp;
            } else {
                templow = (double)atom_getlong(argv + 0);
                if (templow < 0.) {
                    object_warn((t_object *) x, "loop minimum cannot be less than 0., setting to 0.");
                    templow = 0.;
                }   // !! do maximum check in karma_buf_values_internal !!
            }
        } else {
            //temphigh = -1.;               // !! no - leave temphigh alone in case only arg #2 is an error
            //templow = -1.;
            object_warn((t_object *) x, "%s message does not understand arg no.1, resetting loop point", s->s_name);
        }
        
    }
/*
    // dev
    post("%s message:", s->s_name);
*/
    karma_buf_values_internal(x, templow, temphigh, loop_points_flag, callerid);
    
}

void karma_setloop(t_karma *x, t_symbol *s, short ac, t_atom *av)   // " setloop ..... "
{
    defer(x, (method)karma_setloop_internal, s, ac, av);            // main method   // does not have to be defered ??
}

void karma_clock_list(t_karma *x)
{
    if (x->reportlist > 0)    // ('reportlist 0' == off, else milliseconds)
    {
        t_ptr_int frames = x->bframes - 1;
        t_ptr_int maxloop = x->maxloop;
        t_ptr_int directionorig = x->directionorig;
        
        t_bool record = x->record;
        t_bool go = x->go;

        char statehuman = x->statehuman;
        
        double bmsr = x->bmsr;
        double playhead = x->playhead;
        double selection = x->selection;
        
        t_atom datalist[7];                                                                                 // position float % 0..1
        atom_setfloat(  datalist + 0,   CLAMP((directionorig < 0) ? ((playhead - (frames - maxloop)) / maxloop) : (playhead / maxloop), 0., 1.) );
        atom_setlong(   datalist + 1,   go  );                                                              // play flag int
        atom_setlong(   datalist + 2,   record );                                                           // record flag int
        atom_setfloat(  datalist + 3, ((directionorig < 0) ? ((frames - maxloop) / bmsr) : 0.0)     );      // start float ms
        atom_setfloat(  datalist + 4, ((directionorig < 0) ? (frames / bmsr) : (maxloop / bmsr))    );      // end float ms
        atom_setfloat(  datalist + 5, ((selection * maxloop) / bmsr)  );                                    // window float ms
        atom_setlong(   datalist + 6,   statehuman  );                                                      // state flag int
        
//      outlet_list(x->messout, 0L, 7, &datalist);
        outlet_list(x->messout, gensym("list"), 7, datalist);

        if (sys_getdspstate() && (x->reportlist > 0)) {         // '&& (x->reportlist > 0)' ??
            clock_delay(x->tclock, x->reportlist);
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
/*
void karma_selection_internal(t_karma *x, double selectionstart, double selectionlength)
{
    t_ptr_int maxloop = x->maxloop;                 //  in samples
    double minsamps, minsampsnorm;  // temp...
    minsamps = 4.0;                 // ...
    minsampsnorm = minsamps / x->sr;// ...          // 4 samples minimum as normalised value (against sr)
    // !! ^^ this should be against buffer sr surely ?? !!
    x->selstart = selectionstart;
    x->selection = (selectionlength < minsampsnorm) ? minsampsnorm : selectionlength;     // !! fix
    
    if (!x->looprecord)
    {
        if (x->directionorig < 0) { // if originally in reverse
            x->startloop = CLAMP( ((x->bframes - 1) - maxloop) + (x->selstart * maxloop), (x->bframes - 1) - maxloop, (x->bframes - 1) );
            x->endloop = x->startloop + (x->selection * maxloop);
            if (x->endloop > (x->bframes - 1)) {
                x->endloop = ((x->bframes - 1) - maxloop) + (x->endloop - (x->bframes - 1));
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        } else {                    // if originally forwards
            x->startloop = CLAMP( selectionstart * maxloop, 0.0, maxloop );
            x->endloop = x->startloop + (x->selection * maxloop);
            if (x->endloop > maxloop) {
                x->endloop = x->endloop - maxloop;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        }
    }
}
*/
/*
void karma_start(t_karma *x, double positionstart)
{
    karma_selection_internal(x, positionstart, x->selection);
}
*/
// !! pete: i do not like the name "karma_start" - surely it should be "karma_start_point" or "karma_selection_start" ??
void karma_start(t_karma *x, double positionstart)      //  positionstart = "position" float message
{                                                       // 'positionstart' (& 'selstart') is normalised 0..1 value here
    t_ptr_int maxloop = x->maxloop;                     //  in samples
    x->selstart = positionstart;
    
    if (!x->looprecord)
    {
        if (x->directionorig < 0) { // if originally in reverse
            x->startloop = CLAMP( ((x->bframes - 1) - maxloop) + (x->selstart * maxloop), (x->bframes - 1) - maxloop, (x->bframes - 1) );
            x->endloop = x->startloop + (x->selection * maxloop);
            if (x->endloop > (x->bframes - 1)) {
                x->endloop = ((x->bframes - 1) - maxloop) + (x->endloop - (x->bframes - 1));
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        } else {                    // if originally forwards
            x->startloop = CLAMP( positionstart * maxloop, 0.0, maxloop );
            x->endloop = x->startloop + (x->selection * maxloop);
            if (x->endloop > maxloop) {
                x->endloop = x->endloop - maxloop;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        }
    }
}
/*
void karma_window(t_karma *x, double duration)
{
    karma_selection_internal(x, x->selstart, duration);
}
*/
// !! pete: i do not like the name "window" - surely it should be "karma_selection" or "karma_selection_size" ??
void karma_window(t_karma *x, double duration)      //  duration = "window" float message
{                                                   // 'duration' (& 'selection') is normalised 0..1 value here
    t_ptr_int maxloop = x->maxloop;                 //  in samples
    double minsamps, minsampsnorm;  // temp...
    minsamps = 4.0;                 // ...
    minsampsnorm = minsamps / x->sr;// ...          // 4 samples minimum as normalised value (against sr)
                                                    // !! ^^ this should be against buffer sr surely ?? !!
    x->selection = (duration < minsampsnorm) ? minsampsnorm : duration;     // !! fix
    if (!x->looprecord)
    {
        x->endloop = x->startloop + (x->selection * maxloop);
        if (x->directionorig < 0) { // if originally in reverse
            if (x->endloop > (x->bframes - 1)) {
                x->endloop = ((x->bframes - 1) - maxloop) + (x->endloop - (x->bframes - 1));
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        } else {                    // if originally forwards
            if(x->endloop > maxloop) {
                x->endloop = x->endloop - maxloop;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;
            }
        }
    }
}

void karma_stop(t_karma *x)
{
    if (x->initinit) {
        if (x->stopallowed) {
            x->statecontrol = x->recordalt ? 6 : 7;
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
        x->snrfade = 0.0;
    } else if ((x->record) || (x->append)) {
        x->statecontrol = x->recordalt ? 4 : 3;
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
    t_bool record, go, recalt, append, init;
    t_ptr_int bchans, bframes;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);

    record = x->record;
    go = x->go;
    recalt = x->recordalt;
    append = x->append;
    init = x->recordinit;
    sc = x->statecontrol;
    sh = x->statehuman;     // !! fix: overdub display !! *
    
    x->stopallowed = 1;
    
    if (record) {
        if (recalt) {
            sc = 2;
            sh = 3;         // ?? *
        } else {
            sc = 3;
            sh = 2;
        }
    } else {
        if (append) {
            if (go) {
                if (recalt) {
                    sc = 2;
                    sh = 3; // ?? *
                } else {
                    sc = 10;
                    sh = 4;
                }
            } else {
                sc = 1;
                sh = 5;
            }
        } else {
            if (!go) {
                init = 1;
                if (buf) {
                    bchans = x->bchans;
                    bframes = x->bframes;
                    b = buffer_locksamples(buf);
                    if (!b)
                        goto zero;
                    
                    for (i = 0; i < bframes; i++) {
                        if (bchans > 1) {
                            b[i * bchans] = 0.0;
                            b[(i * bchans) + 1] = 0.0;
                            if (bchans > 2) {
                                b[(i * bchans) + 2] = 0.0;
                                if (bchans > 3) {
                                    b[(i * bchans) + 3] = 0.0;
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
    
    go = 1;
    x->go = go;
    x->recordinit = init;
    x->statecontrol = sc;
    x->statehuman = sh;
    
zero:
    return;
}
/*
void karma_record(t_karma *x)
{
    float *b;
    long i;
    char sc, sh;
    t_bool g, recordinit, stopallowed;
    t_ptr_int bchans, bframes;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);

    recordinit = x->recordinit;
    sc = x->statecontrol;
    sh = x->statehuman;     // !! fix: overdub display !! *
    g = 1;

    stopallowed = x->stopallowed;
    x->stopallowed = 1;

    if (x->record) {
        if (x->recordalt) {
            sc = 2;
            sh = 3;         // ?? *
        } else {
            sc = 3;
            sh = 2;
        }
    } else {
        if (x->append) {
            if (x->go) {
                if (x->recordalt) {
                    sc = 2;
                    sh = 3; // ?? *
                } else {
                    sc = 10;
                    sh = 4;
                }
            } else {
                sc = 1;
                sh = 5;
            }
        } else {
            if (!x->go) {
                recordinit = 1;
                if (buf) {
                    bchans = x->bchans;
                    bframes = x->bframes;
                    b = buffer_locksamples(buf);
                    if (!b)
                        x->stopallowed = stopallowed;   // ??
                        goto zero;
                    // !! should this happen defered ??
                    for (i = 0; i < bframes; i++) {
                        if (bchans > 1) {
                            b[i * bchans] = 0.0;
                            b[(i * bchans) + 1] = 0.0;
                            if (bchans > 2) {
                                b[(i * bchans) + 2] = 0.0;
                                if (bchans > 3) {
                                    b[(i * bchans) + 3] = 0.0;
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
    
    x->statecontrol = sc;
    x->recordinit = recordinit;
    x->go = g;
    x->statehuman = sh;

zero:
    return;
}
*/
void karma_append(t_karma *x)
{
    if (x->recordinit) {
        if ((!x->append) && (!x->looprecord)) {
            x->append = 1;
            x->maxloop = x->bframes - 1;
            x->statecontrol = 9;
            x->statehuman = 4;  // ??
            x->stopallowed = 1;
        } else {
            object_error((t_object *)x, "can't append if already appending, or during creating 'initial-loop', or if buffer~ is completely filled");
        }
    } else {
        object_error((t_object *)x, "warning! no 'append' registered until at least one loop has been created first");
    }
}

void karma_overdub(t_karma *x, double amplitude)
{
    x->overdubamp = CLAMP(amplitude, 0.0, 1.0); // clamp overzealous ??
}
/*
void karma_jump(t_karma *x, double jumpposition)
{
    if (x->initinit) {
        if ((x->looprecord) && (!x->record)) {  // if (!((x->looprecord) && (!x->record))) ...
                                                // ... ?? ...
        } else {
            x->statecontrol = 8;
            x->jumphead = jumpposition;
//          x->statehuman = 1;                  // NO ?? ...
            x->stopallowed = 1;
        }
    }
}
*/
void karma_jump(t_karma *x, double jumpposition)
{
    if (x->initinit) {
        if (!((x->looprecord) && (!x->record))) {
            x->statecontrol = 8;
            x->jumphead = jumpposition;
//          x->statehuman = 1;                  // NO ?? ...
            x->stopallowed = 1;
        }
    }
}

t_max_err karma_buf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
    if (msg == ps_buffer_modified)
        x->buf_modified = true;
    
    return buffer_ref_notify(x->buf, s, msg, sndr, dat);
}

t_max_err karma_buf_not_temp(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
    if (msg == ps_buffer_modified)
        x->buf_mod_temp = true;
    
    return buffer_ref_notify(x->buf_temp, s, msg, sndr, dat);
}

void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags)
{
    x->sr       = srate;
    x->vs       = (double)vecount;
    x->vsnorm   = x->vs / x->sr;
    x->clockgo  = 1;
    
    if (x->bufname != 0) {
        if (!x->initinit)
            karma_buf_setup(x, x->bufname);     // !! this should be defered ??
        if (x->chans > 1) {
            if (x->chans > 2) {
                object_method(dsp64, gensym("dsp_add64"), x, karma_quad_perform, 0, NULL);
                //post("karma~_64bit_v1.5_quad");
            } else {
                object_method(dsp64, gensym("dsp_add64"), x, karma_stereo_perform, 0, NULL);
                //post("karma~_64bit_v1.5_stereo");
            }
        } else {
            object_method(dsp64, gensym("dsp_add64"), x, karma_mono_perform, 0, NULL);
            //post("karma~_64bit_v1.5_mono");
        }
        if (!x->initinit) {
            karma_window(x, 1.);
            x->initinit = 1;
        } else {
            karma_window(x, x->selection);
            karma_start(x, x->selstart);
        }
/*  } else {
        object_error((t_object *)x, "fails without buffer~ name!");
*/  }
}


//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  //  //  PERFORM ROUTINES    //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //


// mono perform

void karma_mono_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long syncoutlet = x->syncoutlet;

    double *in1 = ins[0];   // mono in
    double *in2 = ins[1];                       // speed
    
    double *out1  = outs[0];// mono out
    double *outPh = syncoutlet ? outs[1] : 0;   // sync (if arg #3 is on)

    int n = vcount;
    
    double accuratehead, maxhead, jumphead, srscale, sprale, rdif, pokesteps;
    double speed, osamp1, overdubamp, overdubprev, ovdbdif, selstart, xwin;
    double o1prev, o1dif, frac, snrfade, globalramp, snrramp, writeval1, coeff1, recin1;
    t_bool go, record, recordprev, recordalt, looprecord, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, frames, startloop, endloop, playhead, rpre, maxloop, nchan, snrtype, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modset(x, buf);
        x->buf_modified  = false;
    }
    
    o1prev          = x->o1prev;
    o1dif           = x->o1dif;
    writeval1       = x->writeval1;

    go              = x->go;
    statecontrol    = x->statecontrol;
    playfadeflag    = x->playfadeflag;
    recfadeflag     = x->recfadeflag;
    rpre            = x->recordhead;
    recordalt       = x->recordalt;
    nchan           = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    maxloop         = x->maxloop;
    xwin            = x->selection;
    looprecord      = x->looprecord;
    startloop       = x->startloop;
    selstart        = x->selstart;
    endloop         = x->endloop;
    recendmark      = x->recendmark;
    overdubamp      = x->overdubprev;
    overdubprev     = x->overdubamp;
    ovdbdif         = (overdubamp != overdubprev) ? ((overdubprev - overdubamp) / n) : 0.0;
    recordfade      = x->recordfade;
    playfade        = x->playfade;
    accuratehead    = x->playhead;
    playhead        = trunc(accuratehead);
    maxhead         = x->maxhead;
    wrapflag        = x->wrapflag;
    jumphead        = x->jumphead;
    pokesteps       = x->pokesteps;
    snrfade         = x->snrfade;
    globalramp      = (double)x->globalramp;
    snrramp         = (double)x->snrramp;
    snrtype         = x->snrtype;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record init
            record = go = triginit = looprecord = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record recordalt
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off reg
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play recordalt
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            triginit = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop recordalt
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (record) {
                recordfade = 0;
                recfadeflag = 1;
            }
            playfade = 0;
            playfadeflag = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (record) {
                recordfade = 0;
                recfadeflag = 2;
            }
            playfade = 0;
            playfadeflag = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            playfadeflag = 4;
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            record = looprecord = recordalt = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on reg
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }

    // raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
    // 'rpre = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)

    while (n--)
    {
        recin1 = *in1++;
        speed = *in2++;
        direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);

        // declick for change of 'dir'ection
        if (directionprev != direction) {
            if (record && globalramp) {
                ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                recordfade = recfadeflag = 0;
                rpre = -1;
            }
            snrfade = 0.0;
        }   // !! !!
        
        if ((record - recordprev) < 0) {           // samp @record-off
            if (globalramp)
                ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
            rpre = -1;
            dirt = 1;
        } else if ((record - recordprev) > 0) {    // samp @record-on
            recordfade = recfadeflag = 0;
            if (speed < 1.0)
                snrfade = 0.0;
            if (globalramp)
                ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
        }
        recordprev = record;
        
        if (!looprecord)
        {
            if (go)
            {
                if (triginit)
                {
                    if (recendmark)  // calculate end of loop
                    {
                        if (directionorig >= 0)
                        {
                            maxloop = CLAMP(maxhead, 4096, frames - 1);
                            accuratehead = startloop = (selstart * maxloop);    // !!
                            endloop = startloop + (xwin * maxloop);
                            if (endloop > maxloop) {
                                endloop = endloop - (maxloop + 1);
                                wrapflag = 1;
                            } else {
                                wrapflag = 0;
                            }
                            if (direction < 0) {
                                if (globalramp)
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                            }
                        } else {
                            maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                            startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                            accuratehead = endloop = startloop + (xwin * maxloop);
                            if (endloop > (frames - 1)) {
                                endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                wrapflag = 1;
                            } else {
                                wrapflag = 0;
                            }
                            accuratehead = endloop;
                            if (direction > 0) {
                                if (globalramp)
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                            }
                        }
                        if (globalramp)
                            ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                        rpre = -1;
                        snrfade = 0.0;
                        triginit = 0;
                        append = recordalt = recendmark = 0;
                    } else {    // jump / play
                        if (jumpflag)
                            accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                        else
                            accuratehead = (direction < 0) ? endloop : startloop;
                        if (record) {
                            if (globalramp) {
                                ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                recordfade = 0;
                            }
                            rpre = -1;
                            recfadeflag = 0;
                        }
                        snrfade = 0.0;
                        triginit = 0;
                    }
                } else {        // jump-based constraints (outside 'window')
                    sprale = speed * srscale;
                    if (record)
                        sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                    accuratehead = accuratehead + sprale;
                    
                    if (jumpflag)
                    {
                        if (wrapflag) {
                            if ((accuratehead < endloop) || (accuratehead > startloop))
                                jumpflag = 0;
                        } else {
                            if ((accuratehead < endloop) && (accuratehead > startloop))
                                jumpflag = 0;
                        }
                        if (directionorig >= 0)
                        {
                            if (accuratehead > maxloop)
                            {
                                accuratehead = accuratehead - maxloop;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            } else if (accuratehead < 0.0) {
                                accuratehead = maxloop + accuratehead;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            }
                        } else {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            } else if (accuratehead < ((frames - 1) - maxloop)) {
                                accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            }
                        }
                    } else {    // regular 'window' / 'position' constraints
                        if (wrapflag)
                        {
                            if ((accuratehead > endloop) && (accuratehead < startloop))
                            {
                                accuratehead = (direction >= 0) ? startloop : endloop;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            } else if (directionorig >= 0) {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                                else if (accuratehead < 0.0)
                                {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead < ((frames - 1) - maxloop))
                                {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record)
                                    {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead > (frames - 1)) {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {
                            if ((accuratehead > endloop) || (accuratehead < startloop))
                            {
                                accuratehead = (direction >= 0) ? startloop : endloop;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    rpre = -1;
                                }
                            }
                        }
                    }
                }

                // interp ratio
                playhead = trunc(accuratehead);
                if (direction > 0) {
                    frac = accuratehead - playhead;
                } else if (direction < 0) {
                    frac = 1.0 - (accuratehead - playhead);
                } else {
                    frac = 0.0;
                }
                interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                
                if (record) {              // if recording do linear-interp else...
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
                
                if (globalramp)
                {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                    if (snrfade < 1.0)
                    {
                        if (snrfade == 0.0) {
                            o1dif = o1prev - osamp1;
                        }
                        osamp1 += ease_switchramp(o1dif, snrfade, snrtype);// <- easing-curv options (implemented by raja)
                        snrfade += 1 / snrramp;
                    }                                               // "Switch and Ramp" end
                    
                    if (playfade < globalramp)
                    {                                               // realtime ramps for play on/off
                        osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                        playfade++;
                        if (playfade >= globalramp)
                        {
                            switch (playfadeflag)
                            {
                                case 0:
                                    break;
                                case 1:
                                    playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                    break;
                                case 2:
                                    if (!record)
                                        triginit = jumpflag = 1;
                                    break;                          // !! break pete fix !!
                                case 3:                             // jump // record off reg
                                    playfadeflag = playfade = 0;
                                    break;
                                case 4:                             // append
                                    go = triginit = looprecord = 1;
                                    snrfade = 0.0;
                                    playfade = playfadeflag = 0;
                                    break;
                            }
                        }
                    }
                } else {
                    switch (playfadeflag)
                    {
                        case 0:
                            break;
                        case 1:
                            playfadeflag = go = 0;
                            break;
                        case 2:
                            if (!record)
                                triginit = jumpflag = 1;
                            break;                                  // !! break pete fix !!
                        case 3:                                     // jump     // record off reg
                            playfadeflag = 0;
                            break;
                        case 4:                                     // append
                            go = triginit = looprecord = 1;
                            snrfade = 0.0;
                            playfade = playfadeflag = 0;
                            break;
                    }
                }
                
            } else {
                osamp1 = 0.0;
            }
            
            o1prev = osamp1;
            *out1++ = osamp1;
            if (syncoutlet)
                *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
            
            /*
             ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
             (modded to allow for 'window' and 'position' to change on the fly)
             raja's razor: simplest answer to everything was:
             recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
             ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
             ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
            */
            if (record)
            {
                if ((recordfade < globalramp) && (globalramp > 0.0))
                    recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                else
                    recin1 += ((double)b[playhead * nchan]) * overdubamp;
                
                if (rpre < 0) {
                    rpre = playhead;
                    pokesteps = 0.0;
                    rdif = writeval1 = 0.0;
                }
                
                if (rpre == playhead) {
                    writeval1 += recin1;
                    pokesteps += 1.0;
                } else {
                    if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                        writeval1 = writeval1 / pokesteps;
                        pokesteps = 1.0;
                    }
                    b[rpre * nchan] = writeval1;
                    rdif = (double)(playhead - rpre);
                    if (rdif > 0) {                     // linear-interpolation for speed > 1x
                        coeff1 = (recin1 - writeval1) / rdif;
                        for (i = rpre + 1; i < playhead; i++) {
                            writeval1 += coeff1;
                            b[i * nchan] = writeval1;
                        }
                    } else {
                        coeff1 = (recin1 - writeval1) / rdif;
                        for (i = rpre - 1; i > playhead; i--) {
                            writeval1 -= coeff1;
                            b[i * nchan] = writeval1;
                        }
                    }
                    writeval1 = recin1;
                }
                rpre = playhead;
                dirt = 1;
            }                                           // ~ipoke end
            
            if (globalramp)                                   // realtime ramps for record on/off
            {
                if(recordfade < globalramp)
                {
                    recordfade++;
                    if ((recfadeflag) && (recordfade >= globalramp))
                    {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                            recordfade = 0;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
            } else {
                if (recfadeflag) {
                    if (recfadeflag == 2) {
                        triginit = jumpflag = 1;
                    } else if (recfadeflag == 5) {
                        record = 1;
                    } else {
                        record = 0;
                    }
                    recfadeflag = 0;
                }
            }
            directionprev = direction;
        } else {                                        // initial loop creation
            if (go)
            {
                if (triginit)
                {
                    if (jumpflag)                          // jump
                    {
                        if (directionorig >= 0) {
                            accuratehead = jumphead * maxhead;
                        } else {
                            accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                        }
                        jumpflag = 0;
                        snrfade = 0.0;
                        if (record) {
                            if (globalramp) {
                                ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                recordfade = 0;
                            }
                            recfadeflag = 0;
                            rpre = -1;
                        }
                        triginit = 0;
                    } else if (append) {                // append
                        snrfade = 0.0;
                        triginit = 0;
                        if (record)
                        {
                            accuratehead = maxhead;
                            if (globalramp) {
                                ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                recordfade = 0;
                            }
                            recordalt = 1;
                            recfadeflag = 0;
                            rpre = -1;
                        } else {
                            goto apned;
                        }
                    } else {                            // trigger start of initial loop creation
                        directionorig = direction;
                        maxloop = frames - 1;
                        maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                        recordalt = 1;
                        rpre = -1;
                        snrfade = 0.0;
                        triginit = 0;
                    }
                } else {
apned:
                    sprale = speed * srscale;
                    if (record)
                        sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                    accuratehead = accuratehead + sprale;
                    if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                    {
                        if (accuratehead > (frames - 1))
                        {
                            accuratehead = 0.0;
                            record = append;
                            if (record) {
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                            recendmark = triginit = 1;
                            looprecord = recordalt = 0;
                            maxhead = frames - 1;
                        } else if (accuratehead < 0.0) {
                            accuratehead = frames - 1;
                            record = append;
                            if (record) {
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                            recendmark = triginit = 1;
                            looprecord = recordalt = 0;
                            maxhead = 0.0;
                        } else {                        // <- track max write position
                            if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                maxhead = accuratehead;
                        }
                    } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                        if (accuratehead < 0.0)
                        {
                            accuratehead = maxhead + accuratehead;
                            if (globalramp) {
                                ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                rpre = -1;
                                recfadeflag = recordfade = 0;
                            }
                        }
                    } else if (direction >= 0) {
                        if (accuratehead > (frames - 1))
                        {
                            accuratehead = maxhead + (accuratehead - (frames - 1));
                            if (globalramp) {
                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                rpre = -1;
                                recfadeflag = recordfade = 0;
                            }
                        }
                    }
                }
                
                playhead = trunc(accuratehead);
                if (direction > 0) {                          // interp ratio
                    frac = accuratehead - playhead;
                } else if (direction < 0) {
                    frac = 1.0 - (accuratehead - playhead);
                } else {
                    frac = 0.0;
                }
                
                if (globalramp)
                {
                    if (playfade < globalramp)                    // realtime ramps for play on/off
                    {
                        playfade++;
                        if (playfadeflag)
                        {
                            if (playfade >= globalramp)
                            {
                                if (playfadeflag == 2) {
                                    recendmark = 4;
                                    go = 1;
                                }
                                playfadeflag = 0;
                                switch (recendmark) {
                                    case 0:
                                    case 1:
                                        go = 0;
                                        break;
                                    case 2:
                                    case 3:
                                        go = 1;
                                        playfade = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    }
                } else {
                    if (playfadeflag)
                    {
                        if (playfadeflag == 2) {
                            recendmark = 4;
                            go = 1;
                        }
                        playfadeflag = 0;
                        switch (recendmark) {
                            case 0:
                            case 1:
                                go = 0;
                                break;
                            case 2:
                            case 3:
                                go = 1;
                                break;
                            case 4:
                                recendmark = 0;
                                break;
                        }
                    }
                }
                
            }
            
            osamp1 = 0.0;
            o1prev = osamp1;
            *out1++ = osamp1;
            if (syncoutlet)
                *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
            
            // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
            // (modded to assume maximum distance recorded into buffer~ as the total length)
            if (record)
            {
                if ((recordfade < globalramp) && (globalramp > 0.0))
                    recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                else
                    recin1 += ((double)b[playhead * nchan]) * overdubamp;
                
                if (rpre < 0) {
                    rpre = playhead;
                    pokesteps = 0.0;
                    rdif = writeval1 = 0.0;
                }
                
                if (rpre == playhead) {
                    writeval1 += recin1;
                    pokesteps += 1.0;
                } else {
                    if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                        writeval1 = writeval1 / pokesteps;
                        pokesteps = 1.0;
                    }
                    b[rpre * nchan] = writeval1;
                    rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                    if (direction != directionorig)
                    {
                        if (directionorig >= 0)
                        {
                            if (rdif > 0)
                            {
                                if (rdif > (maxhead * 0.5))
                                {
                                    rdif -= maxhead;
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i >= 0; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = maxhead; i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            } else {
                                if ((-rdif) > (maxhead * 0.5))
                                {
                                    rdif += maxhead;
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = 0; i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            }
                        } else {
                            if (rdif > 0)
                            {
                                if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                {
                                    rdif -= ((frames - 1) - (maxhead));
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i >= maxhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = (frames - 1); i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                }
                            } else {
                                if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                {
                                    rdif += ((frames - 1) - (maxhead));
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre + 1); i < frames; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                    for (i = maxhead; i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * nchan] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / rdif;
                                    for (i = (rpre - 1); i > playhead; i--) {
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
                            for (i = (rpre + 1); i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = (rpre - 1); i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                    }
                    writeval1 = recin1;
                }                                       // ~ipoke end
                if (globalramp)                               // realtime ramps for record on/off
                {
                    if (recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                  // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    recordfade = looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }
                } else {
                    if (recfadeflag)
                    {
                        if (recfadeflag == 2) {
                            recendmark = 4;
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        }
                        recfadeflag = 0;
                        switch (recendmark)
                        {
                            case 0:
                                record = 0;
                                break;
                            case 1:
                                if (directionorig < 0) {
                                    maxloop = (frames - 1) - maxhead;
                                } else {
                                    maxloop = maxhead;
                                }
                                break;                      // !! break pete fix different !!
                            case 2:
                                record = looprecord = 0;
                                triginit = 1;
                                break;
                            case 3:
                                record = triginit = 1;
                                looprecord = 0;
                                break;
                            case 4:
                                recendmark = 0;
                                break;
                        }
                    }
                }               //
                rpre = playhead;
                dirt = 1;
            }
            directionprev = direction;
        }
        if (ovdbdif != 0.0)
            overdubamp = overdubamp + ovdbdif;
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev           = o1prev;
    x->o1dif            = o1dif;
    x->writeval1        = writeval1;

    x->maxhead          = maxhead;
    x->pokesteps        = pokesteps;
    x->wrapflag         = wrapflag;
    x->snrfade          = snrfade;
    x->playhead         = accuratehead;
    x->directionorig    = directionorig;
    x->directionprev    = directionprev;
    x->recordhead       = rpre;
    x->recordalt        = recordalt;
    x->recordfade       = recordfade;
    x->triginit         = triginit;
    x->jumpflag         = jumpflag;
    x->go               = go;
    x->record           = record;
    x->recordprev       = recordprev;
    x->statecontrol     = statecontrol;
    x->playfadeflag     = playfadeflag;
    x->recfadeflag      = recfadeflag;
    x->playfade         = playfade;
    x->maxloop          = maxloop;
    x->looprecord       = looprecord;
    x->startloop        = startloop;
    x->endloop          = endloop;
    x->overdubprev      = overdubamp;
    x->recendmark       = recendmark;
    x->append           = append;

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

void karma_stereo_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long syncoutlet = x->syncoutlet;
    
    double *in1 = ins[0];   // L
    double *in2 = ins[1];   // R
    double *in3 = ins[2];                       // speed
    
    double *out1  = outs[0]; // L
    double *out2  = outs[1]; // R
    double *outPh = syncoutlet ? outs[2] : 0;   // sync (if arg #3 is on)
    
    int n = vcount;
    
    double accuratehead, maxhead, jumphead, srscale, sprale, rdif, pokesteps;
    double speed, osamp1, osamp2, overdubamp, overdubprev, ovdbdif, selstart, xwin;
    double o1prev, o2prev, o1dif, o2dif, frac, snrfade, globalramp, snrramp, writeval1, writeval2, coeff1, coeff2, recin1, recin2;
    t_bool go, record, recordprev, recordalt, looprecord, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, frames, startloop, endloop, playhead, rpre, maxloop, nchan, snrtype, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modset(x, buf);
        x->buf_modified  = false;
    }
    
    o1prev          = x->o1prev;
    o2prev          = x->o2prev;
    o1dif           = x->o1dif;
    o2dif           = x->o2dif;
    writeval1       = x->writeval1;
    writeval2       = x->writeval2;

    go              = x->go;
    statecontrol    = x->statecontrol;
    playfadeflag    = x->playfadeflag;
    recfadeflag     = x->recfadeflag;
    rpre            = x->recordhead;
    recordalt       = x->recordalt;
    nchan           = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    maxloop         = x->maxloop;
    xwin            = x->selection;
    looprecord      = x->looprecord;
    startloop       = x->startloop;
    selstart        = x->selstart;
    endloop         = x->endloop;
    recendmark      = x->recendmark;
    overdubamp      = x->overdubprev;
    overdubprev     = x->overdubamp;
    ovdbdif         = (overdubamp != overdubprev) ? ((overdubprev - overdubamp) / n) : 0.0;
    recordfade      = x->recordfade;
    playfade        = x->playfade;
    accuratehead    = x->playhead;
    playhead        = trunc(accuratehead);
    maxhead         = x->maxhead;
    wrapflag        = x->wrapflag;
    jumphead        = x->jumphead;
    pokesteps       = x->pokesteps;
    snrfade         = x->snrfade;
    globalramp      = (double)x->globalramp;
    snrramp         = (double)x->snrramp;
    snrtype         = x->snrtype;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record init
            record = go = triginit = looprecord = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record recordalt
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off reg
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play recordalt
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            triginit = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop recordalt
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (record) {
                recordfade = 0;
                recfadeflag = 1;
            }
            playfade = 0;
            playfadeflag = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (record) {
                recordfade = 0;
                recfadeflag = 2;
            }
            playfade = 0;
            playfadeflag = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            playfadeflag = 4;
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            record = looprecord = recordalt = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on reg
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
    // 'rpre = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)
    
    if (nchan > 1)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = *in3++;
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
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
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, snrfade, snrtype);
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            osamp2 = ease_record(osamp2, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * nchan) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
                    apned:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * nchan) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre - 1); i > playhead; i--) {
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
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
        }

    }
    else
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = *in3++;
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    }
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // <- easing-curv options (implemented by raja)
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                    
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apnde:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                    
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            }
                        }
                        writeval1 = recin1;
                    }                                       // ~ipoke end
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
        }
    
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev           = o1prev;
    x->o2prev           = o2prev;
    x->o1dif            = o1dif;
    x->o2dif            = o2dif;
    x->writeval1        = writeval1;
    x->writeval2        = writeval2;
    
    x->maxhead          = maxhead;
    x->pokesteps        = pokesteps;
    x->wrapflag         = wrapflag;
    x->snrfade          = snrfade;
    x->playhead         = accuratehead;
    x->directionorig    = directionorig;
    x->directionprev    = directionprev;
    x->recordhead       = rpre;
    x->recordalt        = recordalt;
    x->recordfade       = recordfade;
    x->triginit         = triginit;
    x->jumpflag         = jumpflag;
    x->go               = go;
    x->record           = record;
    x->recordprev       = recordprev;
    x->statecontrol     = statecontrol;
    x->playfadeflag     = playfadeflag;
    x->recfadeflag      = recfadeflag;
    x->playfade         = playfade;
    x->maxloop          = maxloop;
    x->looprecord       = looprecord;
    x->startloop        = startloop;
    x->endloop          = endloop;
    x->overdubprev      = overdubamp;
    x->recendmark       = recendmark;
    x->append           = append;
    
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

void karma_quad_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
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
    
    double accuratehead, maxhead, jumphead, srscale, sprale, rdif, pokesteps;
    double speed, osamp1, osamp2, osamp3, osamp4, overdubamp, overdubprev, ovdbdif, selstart, xwin;
    double o1prev, o2prev, o1dif, o2dif, o3prev, o4prev, o3dif, o4dif, frac, snrfade, globalramp, snrramp;
    double writeval1, writeval2, writeval3, writeval4, coeff1, coeff2, coeff3, coeff4, recin1, recin2, recin3, recin4;
    t_bool go, record, recordprev, recordalt, looprecord, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, frames, startloop, endloop, playhead, rpre, maxloop, nchan, snrtype, interp;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modset(x, buf);
        x->buf_modified  = false;
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
    playfadeflag    = x->playfadeflag;
    recfadeflag     = x->recfadeflag;
    rpre            = x->recordhead;
    recordalt       = x->recordalt;
    nchan           = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    maxloop         = x->maxloop;
    xwin            = x->selection;
    looprecord      = x->looprecord;
    startloop       = x->startloop;
    selstart        = x->selstart;
    endloop         = x->endloop;
    recendmark      = x->recendmark;
    overdubamp      = x->overdubprev;
    overdubprev     = x->overdubamp;
    ovdbdif         = (overdubamp != overdubprev) ? ((overdubprev - overdubamp) / n) : 0.0;
    recordfade      = x->recordfade;
    playfade        = x->playfade;
    accuratehead    = x->playhead;
    playhead        = trunc(accuratehead);
    maxhead         = x->maxhead;
    wrapflag        = x->wrapflag;
    jumphead        = x->jumphead;
    pokesteps       = x->pokesteps;
    snrfade         = x->snrfade;
    globalramp      = (double)x->globalramp;
    snrramp         = (double)x->snrramp;
    snrtype         = x->snrtype;
    interp          = x->interpflag;
    
    switch (statecontrol)   // all-in-one 'switch' statement to catch and handle all(most) messages - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record init
            record = go = triginit = looprecord = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record recordalt
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off reg
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play recordalt
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on reg
            triginit = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop recordalt
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop reg
            if (record) {
                recordfade = 0;
                recfadeflag = 1;
            }
            playfade = 0;
            playfadeflag = 1;
            statecontrol = 0;
            break;
        case 8:             // case 8: jump
            if (record) {
                recordfade = 0;
                recfadeflag = 2;
            }
            playfade = 0;
            playfadeflag = 2;
            statecontrol = 0;
            break;
        case 9:             // case 9: append
            playfadeflag = 4;
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append
            record = looprecord = recordalt = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on reg
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
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
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
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
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                                o3dif = o3prev - osamp3;
                                o4dif = o4prev - osamp4;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, snrfade, snrtype);
                            osamp3 += ease_switchramp(o3dif, snrfade, snrtype);
                            osamp4 += ease_switchramp(o4dif, snrfade, snrtype);
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            osamp2 = ease_record(osamp2, (playfadeflag > 0), globalramp, playfade);
                            osamp3 = ease_record(osamp3, (playfadeflag > 0), globalramp, playfade);
                            osamp4 = ease_record(osamp4, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * nchan) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + (((double)b[(playhead * nchan) + 2]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin4 = ease_record(recin4 + (((double)b[(playhead * nchan) + 3]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * nchan) + 2]) * overdubamp;
                        recin4 += ((double)b[(playhead * nchan) + 3]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        writeval4 += recin4;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            writeval3 = writeval3 / pokesteps;
                            writeval4 = writeval4 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        b[(rpre * nchan) + 3] = writeval4;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            coeff4 = (recin4 - writeval4) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
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
                            for (i = rpre - 1; i > playhead; i--) {
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
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
                    apned:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * nchan) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + ((double)b[(playhead * nchan) + 2]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin4 = ease_record(recin4 + ((double)b[(playhead * nchan) + 3]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * nchan) + 2]) * overdubamp;
                        recin4 += ((double)b[(playhead * nchan) + 3]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        writeval4 += recin4;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            writeval3 = writeval3 / pokesteps;
                            writeval4 = writeval4 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        b[(rpre * nchan) + 3] = writeval4;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
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
                                        for (i = maxhead; i > playhead; i--) {
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
                                        for (i = (rpre + 1); i < playhead; i++) {
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
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = 0; i < playhead; i++) {
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
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        coeff4 = (recin4 - writeval4) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                            b[(i * nchan) + 3] = writeval4;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
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
                                        for (i = (rpre + 1); i < playhead; i++) {
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
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
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
                                        for (i = maxhead; i < playhead; i++) {
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
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
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
                                for (i = (rpre - 1); i > playhead; i--) {
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
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;                      // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
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
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
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
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                                o3dif = o3prev - osamp3;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, snrfade, snrtype);
                            osamp3 += ease_switchramp(o3dif, snrfade, snrtype);
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            osamp2 = ease_record(osamp2, (playfadeflag > 0), globalramp, playfade);
                            osamp3 = ease_record(osamp3, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * nchan) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + (((double)b[(playhead * nchan) + 2]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * nchan) + 2]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            writeval3 = writeval3 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            coeff3 = (recin3 - writeval3) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
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
                            for (i = rpre - 1; i > playhead; i--) {
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
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apden;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apden:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * nchan) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + ((double)b[(playhead * nchan) + 2]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * nchan) + 2]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        writeval3 += recin3;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            writeval3 = writeval3 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        b[(rpre * nchan) + 2] = writeval3;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
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
                                        for (i = maxhead; i > playhead; i--) {
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
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = 0; i < playhead; i++) {
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
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        coeff3 = (recin3 - writeval3) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
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
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                            b[(i * nchan) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
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
                                        for (i = maxhead; i < playhead; i++) {
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
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
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
                                for (i = (rpre - 1); i > playhead; i--) {
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
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;              // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                  // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
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
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
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
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                                o2dif = o2prev - osamp2;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // ... easing-curv options (implemented by raja)
                            osamp2 += ease_switchramp(o2dif, snrfade, snrtype);
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            osamp2 = ease_record(osamp2, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * nchan) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            coeff2 = (recin2 - writeval2) / rdif;
                            for (i = rpre - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * nchan] = writeval1;
                                b[(i * nchan) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apdne;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apdne:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * nchan) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                        recin2 += ((double)b[(playhead * nchan) + 1]) * overdubamp;
                    }
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        b[(rpre * nchan) + 1] = writeval2;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * nchan] = writeval1;
                                            b[(i * nchan) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        coeff2 = (recin2 - writeval2) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * nchan] = writeval1;
                                    b[(i * nchan) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                coeff2 = (recin2 - writeval2) / rdif;
                                for (i = (rpre - 1); i > playhead; i--) {
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
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;                  // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
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
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, nchan, rpre, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    rpre = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, rpre, direction, globalramp);
                rpre = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, nchan, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!looprecord)
            {
                if (go)
                {
                    if (triginit)
                    {
                        if (recendmark)  // calculate end of loop
                        {
                            if (directionorig >= 0)
                            {
                                maxloop = CLAMP(maxhead, 4096, frames - 1);
                                accuratehead = startloop = (selstart * maxloop);    // !!
                                endloop = startloop + (xwin * maxloop);
                                if (endloop > maxloop) {
                                    endloop = endloop - (maxloop + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                startloop = ((frames - 1) - maxloop) + (selstart * maxloop); // !!
                                accuratehead = endloop = startloop + (xwin * maxloop);
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - maxloop) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, nchan, maxhead, -direction, globalramp);
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = recordalt = recendmark = 0;
                        } else {    // jump / play
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? (jumphead * maxloop) : (((frames - 1) - maxloop) + (jumphead * maxloop));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                rpre = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        
                        if (jumpflag)
                        {
                            if (wrapflag) {
                                if ((accuratehead < endloop) || (accuratehead > startloop))
                                    jumpflag = 0;
                            } else {
                                if ((accuratehead < endloop) && (accuratehead > startloop))
                                    jumpflag = 0;
                            }
                            if (directionorig >= 0)
                            {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - maxloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        } else {    // regular 'window' / 'position' constraints
                            if (wrapflag)
                            {
                                if ((accuratehead > endloop) && (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - maxloop;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + accuratehead;
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, 0, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                } else {
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - maxloop) - accuratehead);
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - maxloop) + (accuratehead - (frames - 1));
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            rpre = -1;
                                        }
                                    }
                                }
                            } else {
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        rpre = -1;
                                    }
                                }
                            }
                        }
                    }
                    
                    // interp ratio
                    playhead = trunc(accuratehead);
                    if (direction > 0) {
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);     // find samp-indices 4 interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * nchan], b[interp1 * nchan], b[interp2 * nchan], b[interp3 * nchan]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * nchan], b[interp2 * nchan]);
                    }
                    
                    if (globalramp)
                    {                                                   // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                        if (snrfade < 1.0)
                        {
                            if (snrfade == 0.0) {
                                o1dif = o1prev - osamp1;
                            }
                            osamp1 += ease_switchramp(o1dif, snrfade, snrtype);  // <- easing-curv options (implemented by raja)
                            snrfade += 1 / snrramp;
                        }                                               // "Switch and Ramp" end
                        
                        if (playfade < globalramp)
                        {                                               // realtime ramps for play on/off
                            osamp1 = ease_record(osamp1, (playfadeflag > 0), globalramp, playfade);
                            playfade++;
                            if (playfade >= globalramp)
                            {
                                switch (playfadeflag)
                                {
                                    case 0:
                                        break;
                                    case 1:
                                        playfadeflag = go = 0;                // record recordalt   // play recordalt  // stop recordalt / reg
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
                                        break;                          // !! break pete fix !!
                                    case 3:                             // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                             // append
                                        go = triginit = looprecord = 1;
                                        snrfade = 0.0;
                                        playfade = playfadeflag = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        switch (playfadeflag)
                        {
                            case 0:
                                break;
                            case 1:
                                playfadeflag = go = 0;
                                break;
                            case 2:
                                if (!record)
                                    triginit = jumpflag = 1;
                                break;                                  // !! break pete fix !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = looprecord = 1;
                                snrfade = 0.0;
                                playfade = playfadeflag = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * nchan] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[playhead * nchan]) * overdubamp), recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                    
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(playhead - rpre);
                        if (rdif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * nchan] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / rdif;
                            for (i = rpre - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * nchan] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    rpre = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                                   // realtime ramps for record on/off
                {
                    if(recordfade < globalramp)
                    {
                        recordfade++;
                        if ((recfadeflag) && (recordfade >= globalramp))
                        {
                            if (recfadeflag == 2) {
                                triginit = jumpflag = 1;
                                recordfade = 0;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            } else {
                                record = 0;
                            }
                            recfadeflag = 0;
                        }
                    }
                } else {
                    if (recfadeflag) {
                        if (recfadeflag == 2) {
                            triginit = jumpflag = 1;
                        } else if (recfadeflag == 5) {
                            record = 1;
                        } else {
                            record = 0;
                        }
                        recfadeflag = 0;
                    }
                }
                directionprev = direction;
            } else {                                        // initial loop creation
                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                          // jump
                        {
                            if (directionorig >= 0) {
                                accuratehead = jumphead * maxhead;
                            } else {
                                accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                            }
                            jumpflag = 0;
                            snrfade = 0.0;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                rpre = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, nchan, accuratehead, rpre, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordalt = 1;
                                recfadeflag = 0;
                                rpre = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? 0.0 : (frames - 1);
                            recordalt = 1;
                            rpre = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apnde:
                        sprale = speed * srscale;
                        if (record)
                            sprale = (fabs(sprale) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : sprale;
                        accuratehead = accuratehead + sprale;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                        rpre = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                looprecord = recordalt = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {               // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, 0.0, -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, nchan, (frames - 1), -direction, globalramp);
                                    rpre = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                          // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)                    // realtime ramps for play on/off
                        {
                            playfade++;
                            if (playfadeflag)
                            {
                                if (playfade >= globalramp)
                                {
                                    if (playfadeflag == 2) {
                                        recendmark = 4;
                                        go = 1;
                                    }
                                    playfadeflag = 0;
                                    switch (recendmark) {
                                        case 0:
                                        case 1:
                                            go = 0;
                                            break;
                                        case 2:
                                        case 3:
                                            go = 1;
                                            playfade = 0;
                                            break;
                                        case 4:
                                            recendmark = 0;
                                            break;
                                    }
                                }
                            }
                        }
                    } else {
                        if (playfadeflag)
                        {
                            if (playfadeflag == 2) {
                                recendmark = 4;
                                go = 1;
                            }
                            playfadeflag = 0;
                            switch (recendmark) {
                                case 0:
                                case 1:
                                    go = 0;
                                    break;
                                case 2:
                                case 3:
                                    go = 1;
                                    break;
                                case 4:
                                    recendmark = 0;
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
                    *outPh++ = (directionorig >= 0) ? (accuratehead / maxloop) : (accuratehead - (frames - maxloop) / maxloop);
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[playhead * nchan]) * overdubamp, recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * nchan]) * overdubamp;
                    
                    if (rpre < 0) {
                        rpre = playhead;
                        pokesteps = 0.0;
                        rdif = writeval1 = 0.0;
                    }
                    
                    if (rpre == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[rpre * nchan] = writeval1;
                        rdif = (double)(playhead - rpre);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (rdif > 0)
                                {
                                    if (rdif > (maxhead * 0.5))
                                    {
                                        rdif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (maxhead * 0.5))
                                    {
                                        rdif += maxhead;
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (rdif > 0)
                                {
                                    if (rdif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-rdif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        rdif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * nchan] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / rdif;
                                        for (i = (rpre - 1); i > playhead; i--) {
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
                                for (i = (rpre + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / rdif;
                                for (i = (rpre - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    b[i * nchan] = writeval1;
                                }
                            }
                        }
                        writeval1 = recin1;
                    }                                       // ~ipoke end
                    if (globalramp)                               // realtime ramps for record on/off
                    {
                        if (recordfade < globalramp)
                        {
                            recordfade++;
                            if ((recfadeflag) && (recordfade >= globalramp))
                            {
                                if (recfadeflag == 2) {
                                    recendmark = 4;
                                    triginit = jumpflag = 1;
                                    recordfade = 0;
                                } else if (recfadeflag == 5) {
                                    record = 1;
                                }
                                recfadeflag = 0;
                                switch (recendmark)
                                {
                                    case 0:
                                        record = 0;
                                        break;
                                    case 1:
                                        if (directionorig < 0) {
                                            maxloop = (frames - 1) - maxhead;
                                        } else {
                                            maxloop = maxhead;
                                        }
                                        break;                  // !! break pete fix different !!
                                    case 2:
                                        record = looprecord = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        record = triginit = 1;
                                        recordfade = looprecord = 0;
                                        break;
                                    case 4:
                                        recendmark = 0;
                                        break;
                                }
                            }
                        }
                    } else {
                        if (recfadeflag)
                        {
                            if (recfadeflag == 2) {
                                recendmark = 4;
                                triginit = jumpflag = 1;
                            } else if (recfadeflag == 5) {
                                record = 1;
                            }
                            recfadeflag = 0;
                            switch (recendmark)
                            {
                                case 0:
                                    record = 0;
                                    break;
                                case 1:
                                    if (directionorig < 0) {
                                        maxloop = (frames - 1) - maxhead;
                                    } else {
                                        maxloop = maxhead;
                                    }
                                    break;                      // !! break pete fix different !!
                                case 2:
                                    record = looprecord = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    record = triginit = 1;
                                    looprecord = 0;
                                    break;
                                case 4:
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }
                    rpre = playhead;
                    dirt = 1;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;
        }
        
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {           // list-outlet stuff
        clock_delay(x->tclock, 0);
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) {
        clock_unset(x->tclock);
        x->clockgo  = 1;
    }

    x->o1prev           = o1prev;
    x->o2prev           = o2prev;
    x->o3prev           = o3prev;
    x->o4prev           = o4prev;
    x->o1dif            = o1dif;
    x->o2dif            = o2dif;
    x->o3dif            = o3dif;
    x->o4dif            = o4dif;
    x->writeval1        = writeval1;
    x->writeval2        = writeval2;
    x->writeval3        = writeval3;
    x->writeval4        = writeval4;
    
    x->maxhead          = maxhead;
    x->pokesteps        = pokesteps;
    x->wrapflag         = wrapflag;
    x->snrfade          = snrfade;
    x->playhead         = accuratehead;
    x->directionorig    = directionorig;
    x->directionprev    = directionprev;
    x->recordhead       = rpre;
    x->recordalt        = recordalt;
    x->recordfade       = recordfade;
    x->triginit         = triginit;
    x->jumpflag         = jumpflag;
    x->go               = go;
    x->record           = record;
    x->recordprev       = recordprev;
    x->statecontrol     = statecontrol;
    x->playfadeflag     = playfadeflag;
    x->recfadeflag      = recfadeflag;
    x->playfade         = playfade;
    x->maxloop          = maxloop;
    x->looprecord       = looprecord;
    x->startloop        = startloop;
    x->endloop          = endloop;
    x->overdubprev      = overdubamp;
    x->recendmark       = recendmark;
    x->append           = append;
    
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

