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
    raja (original to version 1.4) & pete (version 1.5 and 1.6)
    
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


//  //  //  karma~ version 1.6 by pete, basically karma~ version 1.4 by raja...
//  //  //  ...with some bug fixes and code refactoring
//  //  //  November 2017
//  //  //  N.B. - [at]-commenting for 'DoctorMax' auto documentation

//  //  //  TODO version 1.6:
//  //  //  fix: a bunch of bugs & stuff, incl. ...
//  //  //  - perform loop in/out updating relating to inability to call multiple methods to the object ...
//  //  //  ... by using comma-separated message boxes (etc) <<-- big bug, probably unfixable until version 2 rewrite
//  //  //  - when switching buffer whilst recording, new buffer does not behave like 'inital loop' on 'stop' / 'play'

//  //  //  TODO version 2.0:
//  //  //  rewrite from scratch, take multiple perform routines out and put
//  //  //  interpolation routines and ipoke out into seperate files
//  //  //  then will be able to integrate 'rubberband' and add better ipoke interpolations etc
//  //  //  also look into raja's new 'crossfade' ideas as an optional alternative to 'switch & ramp'
//  //  //  and possibly do seperate externals for different elements (e.g. karmaplay~, karmapoke~, karmaphase~, ...)


#include "stdlib.h"
#include "math.h"

#include "ext.h"            // max
#include "ext_obex.h"       // attributes

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


typedef struct _karma {
    
    t_pxobject      k_ob;
    t_buffer_ref    *buf;
    t_buffer_ref    *buf_temp;      // so that 'set' errors etc do not interupt current buf playback ...
    t_symbol        *bufname;
    t_symbol        *bufname_temp;  // ...

    double  ssr;            // system samplerate
    double  bsr;            // buffer samplerate
    double  bmsr;           // buffer samplerate in samples-per-millisecond
    double  srscale;        // scaling factor: buffer samplerate / system samplerate ("to scale playback speeds appropriately")
    double  vs;             // system vectorsize
    double  vsnorm;         // normalised system vectorsize
    double  bvsnorm;        // normalised buffer vectorsize

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

    double  playhead;       // play position in samples (raja: "double so that capable of tracking playhead position in floating-point indices")
    double  maxhead;        // maximum playhead position that the recording has gone into the buffer~ in samples  // ditto
    double  jumphead;       // jump position (in terms of phase 0..1 of loop) <<-- of 'loop', not 'buffer~'
    double  selstart;       // start position of window ('selection') within loop set by the 'position $1' message sent to object (in phase 0..1)
    double  selection;      // selection length of window ('selection') within loop set by 'window $1' message sent to object (in phase 0..1)
//  double  selmultiply;    // store loop length multiplier amount from 'multiply' method -->> TODO
    double  snrfade;        // fade counter for switch n ramp, normalised 0..1 ??
    double  overdubamp;     // overdub amplitude 0..1 set by 'overdub $1' message sent to object
    double  overdubprev;    // a 'current' overdub amount ("for smoothing overdub amp changes")
    double  speedfloat;     // store speed inlet value if float (not signal)

    long    syncoutlet;     // make sync outlet ? (object attribute @syncout, instantiation time only)
//  long    boffset;        // zero indexed buffer channel # (default 0), user settable, not buffer~ queried -->> TODO
    long    moduloout;      // modulo playback channel outputs flag, user settable, not buffer~ queried -->> TODO
    long    islooped;       // can disable/enable global looping status (rodrigo @ttribute request, TODO) (!! long ??)

    t_ptr_int   bframes;    // number of buffer frames (number of floats long the buffer is for a single channel)
    t_ptr_int   bchans;     // number of buffer channels (number of floats in a frame, stereo has 2 samples per frame, etc.)
    t_ptr_int   ochans;     // number of object audio channels (object arg #2: 1 / 2 / 4)
    t_ptr_int   nchans;     // number of channels to actually address (use only channel one if 'ochans' == 1, etc.)

    t_ptr_int   interpflag; // playback interpolation, 0 = linear, 1 = cubic, 2 = spline (!! why is this a t_ptr_int ??)
    t_ptr_int   recordhead; // record head position in samples
    t_ptr_int   minloop;    // the minimum point in loop so far that has been requested as start point (in samples), is static value
    t_ptr_int   maxloop;    // the overall loop end recorded so far (in samples), is static value
    t_ptr_int   startloop;  // playback start position (in buffer~) in samples, changes depending on loop points and selection logic
    t_ptr_int   endloop;    // playback end position (in buffer~) in samples, changes depending on loop points and selection logic
    t_ptr_int   pokesteps;  // number of steps (samples) to keep track of in ipoke~ linear averaging scheme
    t_ptr_int   recordfade; // fade counter for recording in samples
    t_ptr_int   playfade;   // fade counter for playback in samples
    t_ptr_int   globalramp; // general fade time (for both recording and playback) in samples
    t_ptr_int   snrramp;    // switch n ramp time in samples ("generally much shorter than general fade time")
    t_ptr_int   snrtype;    // switch n ramp curve option choice (!! why is this a t_ptr_int ??)
    t_ptr_int   reportlist; // right list outlet report granularity in ms (!! why is this a t_ptr_int ??)
    t_ptr_int   initiallow; // store inital loop low point after 'initial loop' (default -1 causes default phase 0)
    t_ptr_int   initialhigh;// store inital loop high point after 'initial loop' (default -1 causes default phase 1)

    short   speedconnect;   // 'count[]' info for 'speed' as signal or float in perform routines

    char    statecontrol;   // master looper state control (not 'human state')
    char    statehuman;     // master looper state human logic (not 'statecontrol') (0=stop, 1=play, 2=record, 3=overdub, 4=append 5=initial)

    char    playfadeflag;   // playback up/down flag, used as: 0 = fade up/in, 1 = fade down/out (<<-- TODO: reverse ??) but case switch 0..4 ??
    char    recfadeflag;    // record up/down flag, 0 = fade up/in, 1 = fade down/out (<<-- TODO: reverse ??) but used 0..5 ??
    char    recendmark;     // the flag to show that the loop is done recording and to mark the ending of it
    char    directionorig;  // original direction loop was recorded ("if loop was initially recorded in reverse started from end-of-buffer etc")
    char    directionprev;  // previous direction ("marker for directional changes to place where fades need to happen during recording")
    
    t_bool  stopallowed;    // flag, 'false' if already stopped once (& init)
    t_bool  go;             // execute play ??
    t_bool  record;         // record flag
    t_bool  recordprev;     // previous record flag
    t_bool  loopdetermine;  // flag: "...for when object is in a recording stage that actually determines loop duration..."
    t_bool  alternateflag;  // ("rectoo") ARGH ?? !! flag that selects between different types of engagement for statecontrol ??
    t_bool  append;         // append flag ??
    t_bool  triginit;       // flag to show trigger start of ...stuff... (?)
    t_bool  wrapflag;       // flag to show if a window selection wraps around the buffer~ end / beginning
    t_bool  jumpflag;       // whether jump is 'on' or 'off' ("flag to block jumps from coming too soon" ??)

    t_bool  recordinit;     // initial record (raja: "...determine whether to apply the 'record' message to initial loop recording or not")
    t_bool  initinit;       // initial initialise (raja: "...hack i used to determine whether DSP is turned on for the very first time or not")
    t_bool  initskip;       // is initialising = 0
    t_bool  buf_modified;   // buffer has been modified bool
    t_bool  clockgo;        // activate clock (for list outlet)

    void    *messout;       // list outlet pointer
    void    *tclock;        // list timer pointer

} t_karma;

t_max_err   karma_syncout_set(t_karma *x, t_object *attr, long argc, t_atom *argv);

void       *karma_new(t_symbol *s, short argc, t_atom *argv);
void        karma_free(t_karma *x);

void        karma_float(t_karma *x, double speedfloat);
void        karma_stop(t_karma *x);
void        karma_play(t_karma *x);
void        karma_record(t_karma *x);
//void      karma_select_internal(t_karma *x, double selectionstart, double selectionlength);
void        karma_select_start(t_karma *x, double positionstart);

t_max_err   karma_buf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat);

void        karma_assist(t_karma *x, void *b, long m, long a, char *s);
void        karma_buf_dblclick(t_karma *x);

void        karma_overdub(t_karma *x, double amplitude);
void        karma_select_size(t_karma *x, double duration);
//void      karma_setloop_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void        karma_setloop(t_karma *x, t_symbol *s, short ac, t_atom *av);
void        karma_resetloop(t_karma *x);
//void      karma_loop_multiply(t_karma *x, double multiplier); // <<-- TODO

void        karma_buf_setup(t_karma *x, t_symbol *s);
void        karma_buf_modify(t_karma *x, t_buffer_obj *b);
void        karma_clock_list(t_karma *x);
//void      karma_buf_values_internal(t_karma *x, double low, double high, long loop_points_flag, t_bool caller);
//void      karma_buf_change_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv);
void        karma_buf_change(t_karma *x, t_symbol *s, short ac, t_atom *av);
//void      karma_offset(t_karma *x, long channeloffset);   // <<-- TODO

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
static  t_symbol    *ps_phase, *ps_samples, *ps_milliseconds, *ps_originalloop;
static  t_class     *karma_class = NULL;


// easing function for recording (with ipoke)
static inline double ease_record(double y1, char updwn, double globalramp, t_ptr_int playfade)  // !! rewrite !!
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
                y1  = y1 * (1.0 - (snrfade * snrfade * snrfade + 1));                   // case 3 = cubic ease out
            break;
        case 4: snrfade = (snrfade == 0.0) ? snrfade : pow(2, (10 * (snrfade - 1)));
                y1  = y1 * (1.0 - snrfade);                                             // case 4 = exponential ease in
            break;
        case 5: snrfade = (snrfade == 1.0) ? snrfade : (1 - pow(2, (-10 * snrfade)));
                y1  = y1 * (1.0 - snrfade);                                             // case 5 = exponential ease out
            break;
        case 6: if ((snrfade > 0) && (snrfade < 0.5))
                    y1 = y1 * (1.0 - (0.5 * pow(2, ((20 * snrfade) - 10))));
                else if ((snrfade < 1) && (snrfade > 0.5))
                    y1 = y1 * (1.0 - (-0.5 * pow(2, ((-20 * snrfade) + 10)) + 1));      // case 6 = exponential ease in/out
            break;
    }
    return  y1;
}

// easing function for buffer read
static inline void ease_bufoff(t_ptr_int framesm1, float *b, t_ptr_int pchans, t_ptr_int markposition, char direction, double globalramp)
{
    long i, fadpos;
    
    for (i = 0; i < globalramp; i++)
    {
        fadpos = markposition + (direction * i);
        
        if ( !((fadpos < 0) || (fadpos > framesm1)) )
        {
            b[fadpos * pchans] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (pchans > 1)
            {
                b[(fadpos * pchans) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (pchans > 2)
                {
                    b[(fadpos * pchans) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (pchans > 3)
                    {
                        b[(fadpos * pchans) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// easing function for buffer write
static inline void ease_bufon(t_ptr_int framesm1, float *b, t_ptr_int pchans, t_ptr_int markposition1, t_ptr_int markposition2, char direction, double globalramp)
{
    long i, fadpos1, fadpos2, fadpos3;
    
    for (i = 0; i < globalramp; i++)
    {
        fadpos1 = (markposition1 + (-direction)) + (-direction * i);
        fadpos2 = (markposition2 + (-direction)) + (-direction * i);
        fadpos3 =  markposition2 + (direction * i);
        
        if ( !((fadpos1 < 0) || (fadpos1 > framesm1)) )
        {
            b[fadpos1 * pchans] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (pchans > 1)
            {
                b[(fadpos1 * pchans) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (pchans > 2)
                {
                    b[(fadpos1 * pchans) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (pchans > 3)
                    {
                        b[(fadpos1 * pchans) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos2 < 0) || (fadpos2 > framesm1)) )
        {
            b[fadpos2 * pchans] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (pchans > 1)
            {
                b[(fadpos2 * pchans) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (pchans > 2)
                {
                    b[(fadpos2 * pchans) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (pchans > 3)
                    {
                        b[(fadpos2 * pchans) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
        
        if ( !((fadpos3 < 0) || (fadpos3 > framesm1)) )
        {
            b[fadpos3 * pchans] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
            
            if (pchans > 1)
            {
                b[(fadpos3 * pchans) + 1] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                
                if (pchans > 2)
                {
                    b[(fadpos3 * pchans) + 2] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    
                    if (pchans > 3)
                    {
                        b[(fadpos3 * pchans) + 3] *= 0.5 * ( 1.0 - cos( (((double)i) / globalramp) * PI));
                    }
                }
            }
        }
    }
    
    return;
}

// interpolation points
static inline void interp_index(t_ptr_int playhead, t_ptr_int *indx0, t_ptr_int *indx1, t_ptr_int *indx2, t_ptr_int *indx3, char direction, char directionorig, t_ptr_int maxloop, t_ptr_int framesm1)
{
    *indx0 = playhead - direction;                                   // calc of indecies for interpolations
    
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
    *indx2 = playhead + direction;
    
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
    
    *indx3 = *indx2 + direction;
    
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
/*
// store initial loop recording points from perform loop - only call when required
static inline void initial_points(t_ptr_int minloop, t_ptr_int maxloop, t_ptr_int *initlow, t_ptr_int *inithigh)
{
    *initlow    = minloop;  // in samples...
    *inithigh   = maxloop;  // ...

    return;
}
*/
/*
static inline void initial_points(t_bool force, t_bool init, t_bool determine, char mark, t_ptr_int *initlow, t_ptr_int *inithigh, t_ptr_int minloop, t_ptr_int maxloop)
{
    if (force) {    // "resetloop force" or "setloop reset force"
        *initlow    = minloop;              // in samples...
        *inithigh   = maxloop;              // ...
    } else {        // "resetloop" or "setloop reset"
        if (init) {             // recordinit
            if (determine) {    // loopdetermine
                if (mark) {     // recendmark
                    *initlow    = minloop;  // in samples...
                    *inithigh   = maxloop;  // ...
                }
            }
        }
    }

    return;
}
*/

//  //  //

void ext_main(void *r)
{
    t_class *c = class_new("karma~", (method)karma_new, (method)karma_free, (long)sizeof(t_karma), 0L, A_GIMME, 0);

    // @method position @digest window position start
    // @description window position start point (in phase 0..1) <br />
    // @marg 0 @name start_position @optional 0 @type float
    class_addmethod(c, (method)karma_select_start,  "position", A_FLOAT,    0);
    // @method window @digest window size
    // @description window size (in normalised segment length 0..1) <br />
    // @marg 0 @name window_size @optional 0 @type float
    class_addmethod(c, (method)karma_select_size,   "window",   A_FLOAT,    0);
    // @method jump @digest jump location
    // @description jump location (in normalised segment length 0..1) <br />
    // @marg 0 @name phase_location @optional 0 @type float
    class_addmethod(c, (method)karma_jump,          "jump",     A_FLOAT,    0);
    // @method stop @digest stop transport
    // @description stops internal transport immediately <br />
    class_addmethod(c, (method)karma_stop,          "stop",                 0);
    // @method play @digest play buffer
    // @description play buffer from stopped or recording or appending, play from beginning from playing <br />
    class_addmethod(c, (method)karma_play,          "play",                 0);
    // @method record @digest start recording
    // @description start recording (or overdubbing), or play from a recording state <br />
    class_addmethod(c, (method)karma_record,        "record",               0);
    // @method append @digest (get ready for) append recording
    // @description (get ready for) append recording to end of currently used segment (as long as buffer space available) <br />
    class_addmethod(c, (method)karma_append,        "append",               0);
    // @method multiply @digest loop content multiplier
    // @description mutiply the current loop size by this factor. <br />
    // @marg 0 @name multiplier @optional 0 @type float
//  class_addmethod(c, (method)karma_loop_multiply, "multiply", A_FLOAT,    0);     // !! TODO
    // @method resetloop @digest reset loop to original loop points
    // @description reset the current buffer loop to the loop points that were created when making the initial loop <br />
    // (the same as sending the message "setloop reset" to <o>karma~</o>) <br />
    class_addmethod(c, (method)karma_resetloop,     "resetloop",            0);
    // @method setloop @digest set <o>karma~</o> loop points (not 'window')
    // @description points (start/end) for setting <o>karma~</o> loop in selected buffer~ (not 'window') <br />
    // "setloop" with no args sets loop to entire buffer~ length <br />
    // "setloop reset" sets loop points to those created when making the 'initial loop' (same as sending "resetloop" to <o>karma~</o>) <br />
    // @marg 0 @name loop_start_point @optional 1 @type float
    // @marg 1 @name loop_end_point @optional 1 @type float
    // @marg 2 @name points_type @optional 1 @type symbol
    class_addmethod(c, (method)karma_setloop,       "setloop",  A_GIMME,    0);
    // @method set @digest set (new) buffer
    // @description set (new) buffer for recording or playback, can switch buffers in realtime <br />
    // "set buffer_name" with no other args sets (new) buffer~ and (re)sets loop points to entire buffer~ length <br />
    // @marg 0 @name buffer_name @optional 0 @type symbol
    // @marg 1 @name loop_start_point @optional 1 @type float
    // @marg 2 @name loop_end_point @optional 1 @type float
    // @marg 3 @name points_type @optional 1 @type symbol
    class_addmethod(c, (method)karma_buf_change,    "set",      A_GIMME,    0);
    // @method offset @digest offset referenced buffer~ channel
    // @description offset referenced buffer~ channel, zero-indexed, default 0 (no offset) <br />
    // @marg 0 @name offset @optional 0 @type int
//  class_addmethod(c, (method)karma_offset,        "offset",   A_LONG,     0);     // !! TODO
    // @method overdub @digest overdubbing amplitude
    // @description amplitude (0..1) for when in overdubbing state <br />
    // @marg 0 @name overdub @optional 0 @type float
    class_addmethod(c, (method)karma_overdub,       "overdub",  A_FLOAT,    0);     // !! A_GIMME ?? (for amplitude + smooth time)
    // @method float @digest optional speed inlet float
    // @description  <br />
    // @marg 0 @name float @optional 0 @type float
    class_addmethod(c, (method)karma_float,         "float",    A_FLOAT,    0);
    
    class_addmethod(c, (method)karma_dsp64,         "dsp64",    A_CANT,     0);
    class_addmethod(c, (method)karma_assist,        "assist",   A_CANT,     0);
    class_addmethod(c, (method)karma_buf_dblclick,  "dblclick", A_CANT,     0);
    class_addmethod(c, (method)karma_buf_notify,    "notify",   A_CANT,     0);

    CLASS_ATTR_LONG(c, "syncout", 0, t_karma, syncoutlet);
    CLASS_ATTR_ACCESSORS(c, "syncout", (method)NULL, (method)karma_syncout_set);    // custom for using at instantiation
    CLASS_ATTR_LABEL(c, "syncout", 0, "Create audio rate Sync Outlet no/yes 0/1");  // not needed anywhere ?
    CLASS_ATTR_INVISIBLE(c, "syncout", 0);                      // do not expose to user, only callable as instantiation attribute
    // @description Set as <m>integer</m> flag <b>0</b> or <b>1</b>. With <m>syncout</m> switched on, <o>karma~</o> will add an additional audio signal outlet to the object, after the audio outs and before the final data outlet. This outlet is an audio rate sync signal 0..1 for audio rate synchronous matching to other MSP processes. With <m>syncout</m> switched off (the default), <o>karma~</o> will have no additional outlets. This attribute cannot be sent to the object dynamically or changed in the inspector - it is only available at instantiation time. <br />

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
/*
    CLASS_ATTR_LONG(c, "loop", 0, t_karma, islooped);           // !! TODO !!
    CLASS_ATTR_FILTER_CLIP(c, "loop", 0, 1);
    CLASS_ATTR_LABEL(c, "loop", 0, "Loop off / on");
    // @description Set as <m>integer</m> flag <b>0</b> or <b>1</b>. With <m>loop</m> switched on, <o>karma~</o> acts as a nornal looper, looping playback and/or recording depending on the state machine. With <m>loop</m> switched off, <o>karma~</o> will only play or record in oneshots. Default <b>On (1)</b> <br />

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
    ps_milliseconds = gensym("milliseconds");
    ps_originalloop = gensym("reset");

    // identify build
/*  post("--");
    post("karma~:");
    post("version 1.6 beta");
    post("designed by Rodrigo Constanzo");
    post("original version to 1.4 developed by raja");
    post("1.5 and 1.6 updates by pete");
    post("--");
*/
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

    // should do better argument checks here
    if (attrstart && argv) {
        bufname = atom_getsym(argv + 0);
        // @arg 0 @name buffer_name @optional 0 @type symbol @digest Name of <o>buffer~</o> to be associated with the <o>karma~</o> instance
        // @description Essential argument: <o>karma~</o> will not operate without an associated <o>buffer~</o> <br />
        // The associated <o>buffer~</o> determines memory and length (if associating a buffer~ of <b>0 ms</b> in size <o>karma~</o> will do nothing) <br />
        // The associated <o>buffer~</o> can be changed on the fly (see the <m>set</m> message) but one must be present on instantiation <br />
        if (attrstart > 1) {
            chans = atom_getlong(argv + 1);
            if (attrstart > 2) {
                //object_error((t_object *)x, "rodrigo! third arg no longer used! use new @syncout attribute instead!");
                object_warn((t_object *)x, "too many arguments to karma~, ignoring additional crap");
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
        x->reportlist = 50; // ms
        x->snrramp = x->globalramp = 256;   // samps...
        x->playfade = x->recordfade = 257;  // ...
        x->ssr = sys_getsr();
        x->vs = sys_getblksize();
        x->vsnorm = x->vs / x->ssr;
        x->overdubprev = x->overdubamp = x->speedfloat = 1.0;
        x->islooped = x->snrtype = x->interpflag = 1;
        x->playfadeflag = x->recfadeflag = x->recordinit = x->initinit = x->append = x->jumpflag = 0;
        x->statecontrol = x->statehuman = x->stopallowed = 0;
        x->go = x->triginit = 0;
        x->directionprev = x->directionorig = x->recordprev = x->record = x->alternateflag = x->recendmark = 0;
        x->pokesteps = x->wrapflag = x->loopdetermine = 0;
        x->writeval1 = x->writeval2 = x->writeval3 = x->writeval4 = 0;
        x->maxhead = x->playhead = 0.0;
        x->initiallow = x->initialhigh = -1;
        x->selstart = x->jumphead = x->snrfade = 0.0;
        x->o1dif = x->o2dif = x->o3dif = x->o4dif = x->o1prev = x->o2prev = x->o3prev = x->o4prev = 0.0;
        
        if (bufname != 0)
            x->bufname = bufname;   // !! setup is in 'karma_buf_setup()' called by 'karma_dsp64()'...
/*      else                        // ...(this means double-clicking karma~ does not show buffer~ window until DSP turned on, ho hum)
            object_error((t_object *)x, "requires an associated buffer~ declaration");
*/

        x->ochans = chans;
        // @arg 1 @name num_chans @optional 1 @type int @digest Number of Audio channels
        // @description Default = <b>1 (mono)</b> <br />
        // If <b>1</b>, <o>karma~</o> will operate in mono mode with one input for recording and one output for playback <br />
        // If <b>2</b>, <o>karma~</o> will operate in stereo mode with two inputs for recording and two outputs for playback <br />
        // If <b>4</b>, <o>karma~</o> will operate in quad mode with four inputs for recording and four outputs for playback <br />

        x->messout = listout(x);    // data
        x->tclock  = clock_new((t_object * )x, (method)karma_clock_list);
        attr_args_process(x, argc, argv);
        syncoutlet = x->syncoutlet; // pre-init
        
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
        x->k_ob.z_misc |= Z_NO_INPLACE;
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

// called by 'karma_dsp64' method
void karma_buf_setup(t_karma *x, t_symbol *s)
{
    t_buffer_obj *buf;
    x->bufname = s;
    
    if (!x->buf)
        x->buf = buffer_ref_new((t_object *)x, s);
    else
        buffer_ref_set(x->buf, s);
    
    buf = buffer_ref_getobject(x->buf);
    
    if (buf == NULL) {
        x->buf      = 0;
        //object_error((t_object *)x, "there is no buffer~ named %s", s->s_name);
    } else {
//  if (buf != NULL) {
        x->directionorig            =  0;
        x->maxhead  = x->playhead   =  0.0;
        x->recordhead               = -1;
        x->bchans   = buffer_getchannelcount(buf);
        x->bframes  = buffer_getframecount(buf);
        x->bmsr     = buffer_getmillisamplerate(buf);
        x->bsr      = buffer_getsamplerate(buf);
        x->nchans   = (x->bchans < x->ochans) ? x->bchans : x->ochans;  // MIN
        x->srscale                  = x->bsr / x->ssr;// x->ssr / x->bsr;
        x->bvsnorm  = x->vsnorm * (x->bsr / (double)x->bframes);
        x->minloop  = x->startloop  = 0.0;
        x->maxloop  = x->endloop    = (x->bframes - 1);// * ((x->bchans > 1) ? x->bchans : 1);
        x->selstart                 = 0.0;
        x->selection                = 1.0;

    }
}

// called on buffer modified notification
void karma_buf_modify(t_karma *x, t_buffer_obj *b)
{
    double      modbsr, modbmsr;
    t_ptr_int   modchans, modframes;
    
    if (b) {
        modbsr     = buffer_getsamplerate(b);
        modchans   = buffer_getchannelcount(b);
        modframes  = buffer_getframecount(b);
        modbmsr    = buffer_getmillisamplerate(b);
        
        if ( ( (x->bchans != modchans) || (x->bframes != modframes) ) || (x->bmsr != modbmsr) ) {
            x->bsr                      = modbsr;
            x->bmsr                     = modbmsr;
            x->srscale                  = modbsr / x->ssr;// x->ssr / modbsr;
            x->bframes                  = modframes;
            x->bchans                   = modchans;
            x->nchans   = (modchans < x->ochans) ? modchans : x->ochans;    // MIN
            x->minloop  = x->startloop  = 0.0;
            x->maxloop  = x->endloop    = (x->bframes - 1);// * ((modchans > 1) ? modchans : 1);
            x->bvsnorm  = x->vsnorm * (modbsr / (double)modframes);

            karma_select_size(x, x->selection);
            karma_select_start(x, x->selstart);
//          karma_select_internal(x, x->selstart, x->selection);
            
//          post("buff modify called"); // dev
        }
    }
}

// called by 'karma_buf_change_internal' & 'karma_setloop_internal'
// pete says: i know this proof-of-concept branching is horrible, will rewrite soon...
void karma_buf_values_internal(t_karma *x, double templow, double temphigh, long loop_points_flag, t_bool caller)
{
//  t_symbol *loop_points_sym = 0;                      // dev
    t_symbol *caller_sym = 0;
    t_buffer_obj *buf;
    t_ptr_int bframesm1;//, bchanscnt;
//  long bchans;
    double bframesms, bvsnorm, bvsnorm05;               // !!
    double low, lowtemp, high, hightemp;
    low = templow;
    high = temphigh;

    if (caller) {                                       // only if called from 'karma_buf_change_internal()'
        buf         = buffer_ref_getobject(x->buf);

        x->bchans   = buffer_getchannelcount(buf);
        x->bframes  = buffer_getframecount(buf);
        x->bmsr     = buffer_getmillisamplerate(buf);
        x->bsr      = buffer_getsamplerate(buf);
        x->nchans   = (x->bchans < x->ochans) ? x->bchans : x->ochans;  // MIN
        x->srscale  = x->bsr / x->ssr;// x->ssr / x->bsr;
        
        caller_sym  = gensym("set");
    } else {
        caller_sym  = gensym("setloop");
    }

    //bchans    = x->bchans;
    bframesm1   = (x->bframes - 1);
    bframesms   = (double)bframesm1 / x->bmsr;                  // buffersize in milliseconds
    bvsnorm     = x->vsnorm * (x->bsr / (double)x->bframes);    // vectorsize in (double) % 0..1 (phase) units of buffer~
    bvsnorm05   = bvsnorm * 0.5;                                // half vectorsize (normalised)
    x->bvsnorm  = bvsnorm;
    
    // by this stage in routine, if LOW < 0., it has not been set and should be set to default (0.) regardless of 'loop_points_flag'
    if (low < 0.)
        low = 0.;
    
    if (loop_points_flag == 0) {            // if PHASE
        // by this stage in routine, if HIGH < 0., it has not been set and should be set to default (the maximum phase 1.)
        if (high < 0.)
            high = 1.;                                  // already normalised 0..1
        
        // (templow already treated as phase 0..1)
    } else if (loop_points_flag == 1) {     // if SAMPLES
        // by this stage in routine, if HIGH < 0., it has not been set and should be set to default (the maximum phase 1.)
        if (high < 0.)
            high = 1.;                                  // already normalised 0..1
        else
            high = high / (double)bframesm1;            // normalise samples high 0..1..
        
        if (low > 0.)
            low = low / (double)bframesm1;              // normalise samples low 0..1..
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
        if ( (high - low) == 0. ) {
            object_warn((t_object *) x, "loop size cannot be zero, ignoring %s command", caller_sym);
            return;
        } else {
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
    }
    // regardless of input choice ('loop_points_flag'), final low/high system is normalised (& clipped) 0..1 (phase)
    low     = CLAMP(low, 0., 1.);
    high    = CLAMP(high, 0., 1.);
/*
    // dev
    loop_points_sym = (loop_points_flag > 1) ? ps_milliseconds : ((loop_points_flag < 1) ? ps_phase : ps_samples);
    post("loop start normalised %.2f, loop end normalised %.2f, units %s", low, high, *loop_points_sym);
    //post("loop start samples %.2f, loop end samples %.2f, units used %s", (low * bframesm1), (high * bframesm1), *loop_points_sym);
*/
    // to samples, and account for channels & buffer samplerate
/*    if (bchans > 1) {
        low     = (low * bframesm1) * bchans;// + channeloffset;
        high    = (high * bframesm1) * bchans;// + channeloffset;
    } else {
        low     = (low * bframesm1);// + channeloffset;
        high    = (high * bframesm1);// + channeloffset;
    }
    x->minloop = x->startloop = low;
    x->maxloop = x->endloop = high;
*/
    x->minloop = x->startloop = low * bframesm1;
    x->maxloop = x->endloop = high * bframesm1;

    // update selection
    karma_select_size(x, x->selection);
    karma_select_start(x, x->selstart);
    //karma_select_internal(x, x->selstart, x->selection);

}

// karma_buf_change method defered
// pete says: i know this proof-of-concept branching is horrible, will rewrite soon...
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

    b_temp  = atom_getsym(argv + 0);    // already arg checked to be A_SYM in karma_buf_change()
    
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
        
        buf_temp        = buffer_ref_getobject(x->buf_temp);
        
        if (buf_temp == NULL) {
            
            object_warn((t_object *)x, "cannot find any buffer~ named %s, ignoring", b_temp->s_name);
            x->buf_temp = 0;            // should dspfree the temp buffer here ??
            object_free(x->buf_temp);   // ... ??
            return;

        } else {
            
            x->buf_temp = 0;            // should dspfree the temp buffer here ??
            object_free(x->buf_temp);   // ... ??
            callerid = true;            // identify caller of 'karma_buf_values_internal()'
            
            b  = atom_getsym(argv + 0); // already arg checked

            x->bufname = b;
            
            if (!x->buf) {
                x->buf = buffer_ref_new((t_object *)x, b);
            } else {
                buffer_ref_set(x->buf, b);
            }

            //buf = buffer_ref_getobject(x->buf);   // no need in this method

            // !! if just "set [buffername]" with no additional args and [buffername]...
            // ...already set, message will reset loop points to min / max !!
            loop_points_flag = 2;
            templow = -1.0;
            temphigh = -1.0;

    // ..... do it .....

            // ... obviously pete is a bit confused about all this ...
            x->directionorig = 0;
            x->maxhead = x->playhead = 0.0;             // x->maxhead = x->playhead;    // ?? 'takeover' should be an option ??
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
                    }   // !! do maximum check in karma_buf_values_internal() !!
                } else if (atom_gettype(argv + 2) == A_LONG) {
                    temphigh = (double)atom_getlong(argv + 2);
                    if (temphigh < 0.) {
                        object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
                    }   // !! do maximum check in karma_buf_values_internal() !!
                } else if ( (atom_gettype(argv + 2) == A_SYM) && (argc < 4) ) {
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
                        }   // !! do maximum check in karma_buf_values_internal() !!
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
                        }   // !! do maximum check in karma_buf_values_internal() !!
                    }
                } else if (atom_gettype(argv + 1) == A_SYM) {
                    loop_points_sym = atom_getsym(argv + 1);
                    if (loop_points_sym == ps_dummy)    // !! "dummy" is silent++, move on...
                        loop_points_flag = 2;           // default ms
                    else if (loop_points_sym == ps_originalloop) {      // "reset" message not callable with 'set [buffername]' message
                        object_warn((t_object *) x, "%s message does not understand 'buffername' followed by %s message, ignoring", s->s_name, loop_points_sym);
                        object_warn((t_object *) x, "(the %s message cannot be used whilst changing buffer~ reference", loop_points_sym);
                        object_warn((t_object *) x, "use %s %s message or just %s message instead)", gensym("setloop"), ps_originalloop, gensym("resetloop"));
                        return;         // exit         // or should just default to ms here and carry on ??
                    } else
                        object_warn((t_object *) x, "%s message does not understand arg no.2, setting loop points to minimum (and maximum)", s->s_name);
                } else {
                    object_warn((t_object *) x, "%s message does not understand arg no.2, setting loop points to defaults", s->s_name);
                }
                
            }
/*
            // dev
            post("%s message: buffer~ %s", s->s_name, b->s_name);
*/
            karma_buf_values_internal(x, templow, temphigh, loop_points_flag, callerid);

            //x->buf_temp = 0;                          // should (dsp)free here and not earlier ??
            //object_free(x->buf_temp);

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

    } else {
        
        for (i = 0; i < a; i++) {
            store_av[i] = av[i];
        }
        
        for (j = i; j < 4; j++) {
            atom_setsym(store_av + j, ps_dummy);
        }

    }

    defer(x, (method)karma_buf_change_internal, s, ac, store_av);     // main method
    //karma_buf_change_internal(x, s, ac, store_av);

}

// karma_setloop method (defered?)
// pete says: i know this proof-of-concept branching is horrible, will rewrite soon...
void karma_setloop_internal(t_karma *x, t_symbol *s, short argc, t_atom *argv)   // " setloop ..... "
{
    t_bool callerid = false;                // identify caller of 'karma_buf_values_internal()'
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
            }   // !! do maximum check in karma_buf_values_internal() !!
        } else if (atom_gettype(argv + 1) == A_LONG) {
            temphigh = (double)atom_getlong(argv + 1);
            if (temphigh < 0.) {
                object_warn((t_object *) x, "loop maximum cannot be less than 0., resetting");
            }   // !! do maximum check in karma_buf_values_internal() !!
        } else if ( (atom_gettype(argv + 1) == A_SYM) && (argc < 3) ) {
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
                }   // !! do maximum check in karma_buf_values_internal() !!
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
                }   // !! do maximum check in karma_buf_values_internal() !!
            }
        } else {
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
    t_symbol *reset_sym = 0;
    long points_flag = 1;                   // initial low/high points stored as (t_ptr_int)samples internally
    bool callerid = false;                  // false = called from "setloop"
    double initiallow = (double)x->initiallow;
    double initialhigh = (double)x->initialhigh;
    
    if (ac == 1) {                          // " setloop reset " message
        if (atom_gettype(av) == A_SYM) {    // same as sending " resetloop " message to karma~
            reset_sym = atom_getsym(av);
            if (reset_sym == ps_originalloop) {                     // if "reset" message argument...
                                                                    // ...go straight to calling function with initial loop variables...
//              if (!x->recordinit)
                    karma_buf_values_internal(x, initiallow, initialhigh, points_flag, callerid);
//              else
//                  return;
                
            } else {
                object_error((t_object *) x, "%s does not undertsand message %s, ignoring", s->s_name, reset_sym);
                return;
            }
        } else {                                                    // ...else pass onto pre-calling parsing function...
            karma_setloop_internal(x, s, ac, av);
        }
    } else {                                                        // ...
    //  defer(x, (method)karma_setloop_internal, s, ac, av);
        karma_setloop_internal(x, s, ac, av);
    }

}

// same as sending " setloop reset " message to karma~
void karma_resetloop(t_karma *x)                // " resetloop " message only
{
    long points_flag = 1;                       // initial low/high points stored as samples internally
    bool callerid = false;                      // false = called from "resetloop"
    double initiallow = (double)x->initiallow;  // initial low/high points stored as t_ptr_int...
    double initialhigh = (double)x->initialhigh;// ...

//  if (!x->recordinit)
        karma_buf_values_internal(x, initiallow, initialhigh, points_flag, callerid);
//  else
//      return;
}

void karma_clock_list(t_karma *x)
{
    t_bool rlgtz = x->reportlist > 0;
    
    if (rlgtz)                                      // ('reportlist 0' == off, else milliseconds)
    {
        t_ptr_int frames        = x->bframes - 1;   // !! no '- 1' ??
        t_ptr_int maxloop       = x->maxloop;
        t_ptr_int minloop       = x->minloop;
        t_ptr_int setloopsize;
        
        double bmsr         = x->bmsr;
        double playhead     = x->playhead;
        double selection    = x->selection;
        double normalisedposition;
        setloopsize         = maxloop - minloop;
        
        float reversestart  = ((double)(frames - setloopsize));
        float forwardstart  = ((double)minloop);    // ??           // (minloop + 1)
        float reverseend    = ((double)frames);
        float forwardend    = ((double)maxloop);    // !!           // (maxloop + 1)        // !! only broken on initial buffersize report ?? !!
        float selectionsize = (selection * ((double)setloopsize));  // (setloopsize + 1)    // !! only broken on initial buffersize report ?? !!

        t_bool directflag   = x->directionorig < 0;                 // !! reverse = 1, forward = 0
        t_bool record       = x->record;                            // pointless (and actually is 'record' or 'overdub')
        t_bool go           = x->go;                                // pointless (and actually this is on whenever transport is,...
                                                                    // ...not stricly just 'play')
        char statehuman     = x->statehuman;
                                                    //  ((playhead-(frames-maxloop))/setloopsize) : ((playhead-startloop)/setloopsize)  // ??
        normalisedposition  = CLAMP( directflag ? ((playhead-(frames-setloopsize))/setloopsize) : ((playhead-minloop)/setloopsize), 0., 1. );
        
        t_atom datalist[7];                         // !! reverse logics are wrong ??
        atom_setfloat(  datalist + 0,   normalisedposition      );                              // position float normalised 0..1
        atom_setlong(   datalist + 1,   go         );                                           // play flag int
        atom_setlong(   datalist + 2,   record     );                                           // record flag int
        atom_setfloat(  datalist + 3, ( directflag ? reversestart   :   forwardstart) / bmsr ); // start float ms
        atom_setfloat(  datalist + 4, ( directflag ? reverseend     :   forwardend  ) / bmsr ); // end float ms
        atom_setfloat(  datalist + 5, ( selectionsize / bmsr )  );                              // window float ms
        atom_setlong(   datalist + 6,   statehuman );                                           // state flag int
        
        outlet_list(x->messout, 0L, 7, datalist);
//      outlet_list(x->messout, gensym("list"), 7, datalist);
        
        if (sys_getdspstate() && (rlgtz)) {         // '&& (x->reportlist > 0)' ??
            clock_delay(x->tclock, x->reportlist);
        }
    }
}

void karma_assist(t_karma *x, void *b, long m, long a, char *s)
{
    long dummy;
    long synclet;
    dummy = a + 1;
    synclet = x->syncoutlet;
    a = (a < x->ochans) ? 0 : ((a > x->ochans) ? 2 : 1);
    
    if (m == ASSIST_INLET) {
        switch (a)
        {
            case 0:
                if (dummy == 1) {
                    if (x->ochans == 1)
                        strncpy_zero(s, "(signal) Record Input / messages to karma~", 256);
                    else
                        strncpy_zero(s, "(signal) Record Input 1 / messages to karma~", 256);
                } else {
                    snprintf_zero(s, 256, "(signal) Record Input %ld", dummy);
                    // @in 0 @type signal @digest Audio Inlet(s)... (object arg #2)
                }
                break;
            case 1:
                strncpy_zero(s, "(signal/float) Speed Factor (1. == normal speed)", 256);
                    // @in 1 @type signal_or_float @digest Speed Factor (1. = normal speed, < 1. = slower, > 1. = faster)
                break;
            case 2:
                break;
        }
    } else {    // ASSIST_OUTLET
        switch (a)
        {
            case 0:
                if (x->ochans == 1)
                    strncpy_zero(s, "(signal) Audio Output", 256);
                else
                    snprintf_zero(s, 256, "(signal) Audio Output %ld", dummy);
                    // @out 0 @type signal @digest Audio Outlet(s)... (object arg #2)
                break;
            case 1:
                if (synclet)
                    strncpy_zero(s, "(signal) Sync Outlet (current position 0..1)", 256);
                    // @out 1 @type signal @digest if chosen (@syncout 1) Sync Outlet (current position 0..1)
                else
                    strncpy_zero(s, "List: current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial)", 512);
                break;
            case 2:
                strncpy_zero(s, "List: current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial)", 512);
                    // @out 2 @type list @digest Data Outlet (current position (float 0..1) play state (int 0/1) record state (int 0/1) start position (float ms) end position (float ms) window size (float ms) current state (int 0=stop 1=play 2=record 3=overdub 4=append 5=initial))
                break;
        }
    }
}

//  //  //

void karma_float(t_karma *x, double speedfloat)
{
    long inlet = proxy_getinlet((t_object *)x);
    long chans = (long)x->ochans;

    if (inlet == chans) {   // if speed inlet
        x->speedfloat = speedfloat;
    }
}
/*
void karma_select_internal(t_karma *x, double selectionstart, double selectionlength)
{
    t_ptr_int bfrmaesminusone;
    double setloopsize;

    double minsampsnorm = x->bvsnorm * 0.5;         // half vectorsize samples minimum as normalised value  // !! buffer sr !!
    x->selstart = selectionstart;
    x->selection = (selectionlength < minsampsnorm) ? minsampsnorm : selectionlength;

    // for dealing with selection-out-of-bounds logic:

    if (!x->loopdetermine)
    {
        setloopsize = x->maxloop - x->minloop;

        if (x->directionorig < 0)   // if originally in reverse
        {
            bfrmaesminusone = x->bframes - 1;
            
            x->startloop = CLAMP( (bfrmaesminusone - x->maxloop) + (x->selstart * x->maxloop), bfrmaesminusone - x->maxloop, bfrmaesminusone );
            x->endloop = x->startloop + (x->selection * x->maxloop);

            if (x->endloop > bfrmaesminusone) {
                x->endloop = (bfrmaesminusone - setloopsize) + (x->endloop - bfrmaesminusone);
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        } else {                    // if originally forwards

            x->startloop = CLAMP( selectionstart * x->maxloop, x->minloop, x->maxloop );
            x->endloop = x->startloop + (x->selection * setloopsize);
            
            if (x->endloop > x->maxloop) {
                x->endloop = x->endloop - setloopsize;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        }
    }
}

void karma_select_start(t_karma *x, double positionstart)
{
    karma_select_internal(x, positionstart, x->selection);
}

void karma_select_size(t_karma *x, double duration)
{
    karma_select_internal(x, x->selstart, duration);
}
*/
void karma_select_start(t_karma *x, double positionstart)   // positionstart = "position" float message
{
    t_ptr_int bfrmaesminusone, setloopsize;
    x->selstart = CLAMP(positionstart, 0., 1.);
    
    // for dealing with selection-out-of-bounds logic:
    
    if (!x->loopdetermine)
    {
        setloopsize = x->maxloop - x->minloop;
        
        if (x->directionorig < 0)   // if originally in reverse
        {
            bfrmaesminusone = x->bframes - 1;

            x->startloop = CLAMP( (bfrmaesminusone - x->maxloop) + (positionstart * setloopsize), bfrmaesminusone - x->maxloop, bfrmaesminusone );
            x->endloop = x->startloop + (x->selection * x->maxloop);
            
            if (x->endloop > bfrmaesminusone) {
                x->endloop = (bfrmaesminusone - setloopsize) + (x->endloop - bfrmaesminusone);
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        } else {                    // if originally forwards

            x->startloop = CLAMP( ((positionstart * setloopsize) + x->minloop), x->minloop, x->maxloop );   // no need for CLAMP ??
            x->endloop = x->startloop + (x->selection * setloopsize);
            
            if (x->endloop > x->maxloop) {
                x->endloop = x->endloop - setloopsize;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        }
    }
}

void karma_select_size(t_karma *x, double duration)     // duration = "window" float message
{
    t_ptr_int bfrmaesminusone, setloopsize;
    
    //double minsampsnorm = x->bvsnorm * 0.5;           // half vectorsize samples minimum as normalised value  // !! buffer sr !!
    //x->selection = (duration < 0.0) ? 0.0 : duration; // !! allow zero for rodrigo !!
    x->selection = CLAMP(duration, 0., 1.);
    
    // for dealing with selection-out-of-bounds logic:
    
    if (!x->loopdetermine)
    {
        setloopsize = x->maxloop - x->minloop;
        x->endloop = x->startloop + (x->selection * setloopsize);
        
        if (x->directionorig < 0)   // if originally in reverse
        {
            bfrmaesminusone = x->bframes - 1;
            
            if (x->endloop > bfrmaesminusone) {
                x->endloop = (bfrmaesminusone - setloopsize) + (x->endloop - bfrmaesminusone);
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        } else {                    // if originally forwards
            
            if(x->endloop > x->maxloop) {
                x->endloop = x->endloop - setloopsize;
                x->wrapflag = 1;
            } else {
                x->wrapflag = 0;    // selection-in-bounds
            }
            
        }
    }
}

void karma_stop(t_karma *x)
{
    if (x->initinit) {
        if (x->stopallowed) {
            x->statecontrol = x->alternateflag ? 6 : 7;
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
        x->snrfade = 0.0;   // !! should disable ??
    } else if ((x->record) || (x->append)) {
        x->statecontrol = x->alternateflag ? 4 : 3;
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
    t_bool record, go, altflag, append, init;
    t_ptr_int bframes, rchans;  // !! local 'rchans' = 'nchans' not 'bchans' !!
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);

    record = x->record;
    go = x->go;
    altflag = x->alternateflag;
    append = x->append;
    init = x->recordinit;
    sc = x->statecontrol;
    sh = x->statehuman;
    
    x->stopallowed = 1;
    
    if (record) {
        if (altflag) {
            sc = 2;
            sh = 3;         // ?? is this wrong?, it is not neccessarily overdub ??
        } else {
            sc = 3;
            sh = (sh == 3) ? 1 : 2; // !! hack !! (but works !!)
        }
    } else {
        if (append) {
            if (go) {
                if (altflag) {
                    sc = 2;
                    sh = 3; // ?? is this wrong?, it is not neccessarily overdub ??
                } else {
                    sc = 10;// !!
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
                    rchans = x->bchans;     // !! nchans not bchans = only record onto channel(s) currently used by karma~...
                    bframes = x->bframes;   // ...(leave other channels in tact)    <<-- BOLLOX
                    b = buffer_locksamples(buf);
                    if (!b)
                        goto zero;
                    
                    for (i = 0; i < bframes; i++) {
                        if (rchans > 1) {
                            b[i * rchans] = 0.0;
                            b[(i * rchans) + 1] = 0.0;
                            if (rchans > 2) {
                                b[(i * rchans) + 2] = 0.0;
                                if (rchans > 3) {
                                    b[(i * rchans) + 3] = 0.0;
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
            } else {            // !! not 'record', not 'append', is 'go' ??
                sc = 11;        // !! ?? seems wrong
                sh = 3;         // !! is this overdub ?? seems wrong
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
// store initial loop recording points on command - pointless
void karma_initial_points(t_karma *x, t_bool force)
{
    if (force) {    // "resetloop force" or "setloop reset force"
        x->initiallow = x->minloop;                 // in samples...
        x->initialhigh = x->maxloop;                // ...
    } else {        // "resetloop" or "setloop reset"
        if (x->recordinit) {
            if (x->loopdetermine) {
                if (x->recendmark) {
                    x->initiallow = x->minloop;     // in samples...
                    x->initialhigh = x->maxloop;    // ...
                }
            }
        }
    }
}
*/
void karma_append(t_karma *x)
{
    if (x->recordinit) {
        if ((!x->append) && (!x->loopdetermine)) {
            x->append = 1;
            x->maxloop = (x->bframes - 1);
            x->statecontrol = 9;
            x->statehuman = 4;
            x->stopallowed = 1;
        } else {
            object_error((t_object *)x, "can't append if already appending, or during 'initial-loop', or if buffer~ is full");
        }
    } else {
        object_error((t_object *)x, "warning! no 'append' registered until at least one loop has been created first");
    }
}

void karma_overdub(t_karma *x, double amplitude)
{
    x->overdubamp = CLAMP(amplitude, 0.0, 1.0);
}
/*
void karma_jump(t_karma *x, double jumpposition)
{
    if (x->initinit) {
        if ((x->loopdetermine) && (!x->record)) {  // if (!((x->loopdetermine) && (!x->record))) ...
                                                // ... ?? ...
        } else {
            x->statecontrol = 8;
            x->jumphead = jumpposition;
//          x->statehuman = 1;                  // ??
            x->stopallowed = 1;
        }
    }
}
*/
void karma_jump(t_karma *x, double jumpposition)
{
    if (x->initinit) {
        if ( !((x->loopdetermine)&&(!x->record)) ) {
            x->statecontrol = 8;
            x->jumphead = CLAMP(jumpposition, 0., 1.);  // for now phase only, TODO - ms & samples
//          x->statehuman = 1;                          // no - 'jump' is whatever 'statehuman' currently is (most likely 'play')
            x->stopallowed = 1;
        }
    }
}

//  //  //

t_max_err karma_syncout_set(t_karma *x, t_object *attr, long argc, t_atom *argv)
{
    long syncout = atom_getlong(argv);

    if (!x->initskip) {
        if (argc && argv) {
            x->syncoutlet   = CLAMP(syncout, 0, 1);
        }
    } else {
        object_warn((t_object *) x, "the syncout attribute is only available at instantiation time, ignoring 'syncout %ld'", syncout);
    }

    return 0;
}
/*
t_max_err karma_buf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
    if (msg == ps_buffer_modified)
        x->buf_modified = true;
    
    return buffer_ref_notify(x->buf, s, msg, sndr, dat);
}
*/
t_max_err karma_buf_notify(t_karma *x, t_symbol *s, t_symbol *msg, void *sndr, void *dat)
{
//  t_symbol *bufnamecheck = (t_symbol *)object_method((t_object *)sndr, gensym("getname"));
 
//  if (bufnamecheck == x->bufname) {   // check...
    if (buffer_ref_exists(x->buf)) {    // this hack does not really work...
        if (msg == ps_buffer_modified)
            x->buf_modified = true;     // set flag
        return  buffer_ref_notify(x->buf, s, msg, sndr, dat);   // ...return
    } else {
        return  MAX_ERR_NONE;
    }
}

void karma_dsp64(t_karma *x, t_object *dsp64, short *count, double srate, long vecount, long flags)
{
    x->ssr      = srate;
    x->vs       = (double)vecount;
    x->vsnorm   = (double)vecount / srate;      // x->vs / x->ssr;
    x->clockgo  = 1;
    
    if (x->bufname != 0) {
//      if (buffer_ref_exists(x->buf)) {        // this hack does not really work...
            if (!x->initinit)
                karma_buf_setup(x, x->bufname); // does 'x->bvsnorm'    // !! this should be defered ??
//      }
        if (x->ochans > 1) {
            if (x->ochans > 2) {
                x->speedconnect = count[4];     // speed is 5th inlet
                object_method(dsp64, gensym("dsp_add64"), x, karma_quad_perform, 0, NULL);
                //post("karma~_v1.5_quad");
            } else {
                x->speedconnect = count[2];     // speed is 3rd inlet
                object_method(dsp64, gensym("dsp_add64"), x, karma_stereo_perform, 0, NULL);
                //post("karma~_v1.5_stereo");
            }
        } else {
            x->speedconnect = count[1];         // speed is 2nd inlet
            object_method(dsp64, gensym("dsp_add64"), x, karma_mono_perform, 0, NULL);
            //post("karma~_v1.5_mono");
        }
        if (!x->initinit) {
            karma_select_size(x, 1.);
            x->initinit = 1;
        } else {
            karma_select_size(x, x->selection);
            karma_select_start(x, x->selstart);
//          karma_select_internal(x, x->selstart, x->selection);
        }
/*  } else {
        object_error((t_object *)x, "fails without buffer~ name!");
*/  }
}


//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  (crazy) PERFORM ROUTINES    //  //  //  //  //  //  //  //  //
//  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //  //


// mono perform

void karma_mono_perform(t_karma *x, t_object *dsp64, double **ins, long nins, double **outs, long nouts, long vcount, long flgs, void *usr)
{
    long    syncoutlet  = x->syncoutlet;

    double *in1 = ins[0];   // mono in
    double *in2 = ins[1];                       // speed (if signal connected)
    
    double *out1  = outs[0];// mono out
    double *outPh = syncoutlet ? outs[1] : 0;   // sync (if @syncout 1)

    long    n = vcount;
    short   speedinlet  = x->speedconnect;
    
    double accuratehead, maxhead, jumphead, srscale, speedsrscaled, recplaydif, pokesteps;
    double speed, speedfloat, osamp1, overdubamp, overdubprev, ovdbdif, selstart, selection;
    double o1prev, o1dif, frac, snrfade, globalramp, snrramp, writeval1, coeff1, recin1;
    t_bool go, record, recordprev, alternateflag, loopdetermine, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, pchans, snrtype, interp;
    t_ptr_int frames, startloop, endloop, playhead, recordhead, minloop, maxloop, setloopsize;
    t_ptr_int initiallow, initialhigh;
    
    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->k_ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modify(x, buf);
        x->buf_modified  = false;
    }
    
    o1prev          = x->o1prev;
    o1dif           = x->o1dif;
    writeval1       = x->writeval1;

    go              = x->go;
    statecontrol    = x->statecontrol;
    playfadeflag    = x->playfadeflag;
    recfadeflag     = x->recfadeflag;
    recordhead      = x->recordhead;
    alternateflag   = x->alternateflag;
    pchans          = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    minloop         = x->minloop;
    maxloop         = x->maxloop;
    initiallow      = x->initiallow;
    initialhigh     = x->initialhigh;
    selection       = x->selection;
    loopdetermine   = x->loopdetermine;
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
    speedfloat      = x->speedfloat;
    
    switch (statecontrol)   // "all-in-one 'switch' statement to catch and handle all(most) messages" - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record initial loop
            record = go = triginit = loopdetermine = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record alternateflag (wtf is 'alternateflag' ('rectoo') ?!)  // in to OVERDUB ??
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off regular
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // out of OVERDUB ??
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on regular
            triginit = 1;   // ?!?!
            statecontrol = 0;
            break;
        case 6:             // case 6: stop alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // after OVERDUB ??
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop regular
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
            playfadeflag = 4;   // !! modified in perform loop switch case(s) for playing behind append
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append (what is special about it ?!)   // in to RECORD / OVERDUB ??
            record = loopdetermine = alternateflag = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on regular (when ?! not looped ?!)
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }

    //  raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
    // 'recordhead = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)

    while (n--)
    {
        recin1 = *in1++;
        speed = speedinlet ? *in2++ : speedfloat;   // signal of float ?
        direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);

        // declick for change of 'dir'ection
        if (directionprev != direction) {
            if (record && globalramp) {
                ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                recordfade = recfadeflag = 0;
                recordhead = -1;
            }
            snrfade = 0.0;
        }
        
        if ((record - recordprev) < 0) {           // samp @record-off
            if (globalramp)
                ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
            //initialhigh = loopdetermine ? recordhead : initialhigh;
            recordhead = -1;
            dirt = 1;
        } else if ((record - recordprev) > 0) {    // samp @record-on
            recordfade = recfadeflag = 0;
            if (speed < 1.0)
                snrfade = 0.0;
            if (globalramp)
                ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
        }
        recordprev = record;
        
        if (!loopdetermine)
        {
            if (go)
            {
                /*
                calculate_head(directionorig, maxhead, frames, minloop, selstart, selection, direction, globalramp, &b, pchans, record, jumpflag, jumphead, &maxloop, &setloopsize, &accuratehead, &startloop, &endloop, &wrapflag, &recordhead, &snrfade, &append, &alternateflag, &recendmark, &triginit, &speedsrscaled, &recordfade, &recfadeflag);
                */
                
                if (triginit)
                {
                    if (recendmark)  // calculate end of loop
                    {
                        if (directionorig >= 0)
                        {
                            maxloop = CLAMP(maxhead, 4096, frames - 1); // why 4096 ??
                            setloopsize = maxloop - minloop;
                            accuratehead = startloop = minloop + (selstart * setloopsize);
                            endloop = startloop + (selection * setloopsize);
                            if (endloop > maxloop) {
                                endloop = endloop - (setloopsize + 1);
                                wrapflag = 1;
                            } else {
                                wrapflag = 0;
                            }
                            if (direction < 0) {
                                if (globalramp)
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                            }
                        } else {
                            maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                            setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                            startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                            accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                            if (endloop > (frames - 1)) {
                                endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                wrapflag = 1;
                            } else {
                                wrapflag = 0;
                            }
                            accuratehead = endloop;
                            if (direction > 0) {
                                if (globalramp)
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                            }
                        }
                        if (globalramp)
                            ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                        recordhead = -1;
                        snrfade = 0.0;
                        triginit = 0;
                        append = alternateflag = recendmark = 0;
                    } else {    // jump / play (inside 'window')
                        setloopsize = maxloop - minloop;
                        if (jumpflag)
                            accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                        else
                            accuratehead = (direction < 0) ? endloop : startloop;
                        if (record) {
                            if (globalramp) {
                                ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                recordfade = 0;
                            }
                            recordhead = -1;
                            recfadeflag = 0;
                        }
                        snrfade = 0.0;
                        triginit = 0;
                    }
                } else {        // jump-based constraints (outside 'window')
                    setloopsize = maxloop - minloop;
                    speedsrscaled = speed * srscale;
                    
                    if (record)
                        speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                    accuratehead = accuratehead + speedsrscaled;
                    
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
                                accuratehead = accuratehead - setloopsize;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
                                }
                            } else if (accuratehead < 0.0) {
                                accuratehead = maxloop + accuratehead;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
                                }
                            }
                        } else {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
                                }
                            } else if (accuratehead < ((frames - 1) - maxloop)) {
                                accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
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
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
                                }
                            } else if (directionorig >= 0) {
                                if (accuratehead > maxloop)
                                {
                                    accuratehead = accuratehead - setloopsize;  // fixed position ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                                else if (accuratehead < 0.0)
                                {
                                    accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {    // reverse
                                if (accuratehead < ((frames - 1) - maxloop))
                                {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record)
                                    {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead > (frames - 1)) {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            }
                        } else {    // (not wrapflag)
                            if ((accuratehead > endloop) || (accuratehead < startloop))
                            {
                                accuratehead = (direction >= 0) ? startloop : endloop;
                                snrfade = 0.0;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                        recordfade = 0;
                                    }
                                    recfadeflag = 0;
                                    recordhead = -1;
                                }
                            }
                        }
                    }
                }

                /* calculate_head() to here */
                
                // interp ratio
                playhead = trunc(accuratehead);
                if (direction > 0) {
                    frac = accuratehead - playhead;
                } else if (direction < 0) {
                    frac = 1.0 - (accuratehead - playhead);
                } else {
                    frac = 0.0;
                }                                                                                   // setloopsize  // ??
                interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices
                
                if (record) {           // if recording do linear-interp else...
                    osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                    if (interp == 1)
                        osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                    else if (interp == 2)
                        osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                    else
                        osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                }
                
                if (globalramp)
                {                                           // "Switch and Ramp" - http://msp.ucsd.edu/techniques/v0.11/book-html/node63.html
                    if (snrfade < 1.0)
                    {
                        if (snrfade == 0.0) {
                            o1dif = o1prev - osamp1;
                        }
                        osamp1 += ease_switchramp(o1dif, snrfade, snrtype);// <- easing-curv options implemented by raja
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
                                    playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                    break;
                                case 2:
                                    if (!record)
                                        triginit = jumpflag = 1;
//                                  break;                  // !! no break - pass 2 -> 3 !!
                                case 3:                     // jump // record off reg
                                    playfadeflag = playfade = 0;
                                    break;
                                case 4:                     // append
                                    go = triginit = loopdetermine = 1;
                                    // !! will disbling this enable play behind append ?? should this be dependent on passing previous maxloop ??
                                    snrfade = 0.0;
                                    playfade = 0;           // !!
                                    playfadeflag = 0;
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
//                          break;                                  // !! no break - pass 2 -> 3 !!
                        case 3:                                     // jump     // record off reg
                            playfadeflag = 0;
                            break;
                        case 4:                                     // append
                            go = triginit = loopdetermine = 1;
                            // !! will disbling this enable play behind append ?? should this be based on passing previous maxloop ??
                            snrfade = 0.0;
                            playfade = 0;   // !!
                            playfadeflag = 0;
                            break;
                    }
                }
                
            } else {
                osamp1 = 0.0;
            }
            
            o1prev = osamp1;
            *out1++ = osamp1;
            if (syncoutlet) {
                setloopsize = maxloop-minloop;
                *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
            }

            /*
             ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
             (modded to allow for 'selection' (window) and 'selstart' (position) to change on the fly)
             raja's razor: simplest answer to everything was:
             recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
             ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
             ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
            */
            if (record)
            {
                if ((recordfade < globalramp) && (globalramp > 0.0))
                    recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                else
                    recin1 += ((double)b[playhead * pchans]) * overdubamp;
                
                if (recordhead < 0) {
                    recordhead = playhead;
                    pokesteps = 0.0;
                    recplaydif = writeval1 = 0.0;
                }
                
                if (recordhead == playhead) {
                    writeval1 += recin1;
                    pokesteps += 1.0;
                } else {
                    if (pokesteps > 1.0) {              // linear-averaging for speed < 1x
                        writeval1 = writeval1 / pokesteps;
                        pokesteps = 1.0;
                    }
                    b[recordhead * pchans] = writeval1;
                    recplaydif = (double)(playhead - recordhead);
                    if (recplaydif > 0) {               // linear-interpolation for speed > 1x
                        coeff1 = (recin1 - writeval1) / recplaydif;
                        for (i = recordhead + 1; i < playhead; i++) {
                            writeval1 += coeff1;
                            b[i * pchans] = writeval1;
                        }
                    } else {
                        coeff1 = (recin1 - writeval1) / recplaydif;
                        for (i = recordhead - 1; i > playhead; i--) {
                            writeval1 -= coeff1;
                            b[i * pchans] = writeval1;
                        }
                    }
                    writeval1 = recin1;
                }
                recordhead = playhead;
                dirt = 1;
            }                                           // ~ipoke end
            
            if (globalramp)                             // realtime ramps for record on/off
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
        // !! is 'loopdetermine' !!

            if (go)
            {
                if (triginit)
                {
                    if (jumpflag)                       // jump
                    {
                        if (directionorig >= 0) {
                            accuratehead = jumphead * maxhead;      // !! maxhead !!
                        } else {
                            accuratehead = (frames - 1) - (((frames - 1) - maxhead) * jumphead);
                        }
                        jumpflag = 0;
                        snrfade = 0.0;
                        if (record) {
                            if (globalramp) {
                                ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                recordfade = 0;
                            }
                            recfadeflag = 0;
                            recordhead = -1;
                        }
                        triginit = 0;
                    } else if (append) {                // append
                        snrfade = 0.0;
                        triginit = 0;
                        if (record)
                        {
                            accuratehead = maxhead;                 // !! maxhead !!
                            if (globalramp) {
                                ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                recordfade = 0;
                            }
                            alternateflag = 1;
                            recfadeflag = 0;
                            recordhead = -1;
                        } else {
                            goto apned;
                        }
                    } else {                            // trigger start of initial loop creation
                        directionorig = direction;
                        minloop = 0.0;
                        maxloop = frames - 1;
                        maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                        alternateflag = 1;
                        recordhead = -1;
                        snrfade = 0.0;
                        triginit = 0;
                    }
                } else {
apned:
                    setloopsize = maxloop - minloop;                // not really required here because initial loop ??
                    speedsrscaled = speed * srscale;
                    if (record)                                     // why 1024 ??
                        speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                    accuratehead = accuratehead + speedsrscaled;
                    if (direction == directionorig)                 // buffer~ boundary constraints and registry of maximum distance traversed
                    {
                        if (accuratehead > (frames - 1))
                        {
                            accuratehead = 0.0;
                            record = append;
                            if (record) {
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);   // maxloop ??
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                            recendmark = triginit = 1;
                            loopdetermine = alternateflag = 0;
                            maxhead = frames - 1;
                        } else if (accuratehead < 0.0) {
                            accuratehead = frames - 1;
                            record = append;
                            if (record) {
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                            recendmark = triginit = 1;
                            loopdetermine = alternateflag = 0;
                            maxhead = 0.0;
                        } else {                                    // <- track max write position
                            if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) ) {
                                maxhead = accuratehead;
                            }
                        }
                    } else if (direction < 0) {                     // wraparounds for reversal while creating initial-loop
                        if (accuratehead < 0.0)
                        {
                            accuratehead = maxhead + accuratehead;
                            if (globalramp) {
                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                recordhead = -1;
                                recfadeflag = recordfade = 0;
                            }
                        }
                    } else if (direction >= 0) {
                        if (accuratehead > (frames - 1))
                        {
                            accuratehead = maxhead + (accuratehead - (frames - 1));
                            if (globalramp) {
                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);   // maxloop ??
                                recordhead = -1;
                                recfadeflag = recordfade = 0;
                            }
                        }
                    }
                //initialhigh = append ? initialhigh : maxhead;   // !! !!
                }
                
                playhead = trunc(accuratehead);
                if (direction > 0) {                            // interp ratio
                    frac = accuratehead - playhead;
                } else if (direction < 0) {
                    frac = 1.0 - (accuratehead - playhead);
                } else {
                    frac = 0.0;
                }
                
                if (globalramp)
                {
                    if (playfade < globalramp)                  // realtime ramps for play on/off
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
            if (syncoutlet) {
                setloopsize = maxloop-minloop;
                *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
            }
            
            // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
            // (modded to assume maximum distance recorded into buffer~ as the total length)
            if (record)
            {
                if ((recordfade < globalramp) && (globalramp > 0.0))
                    recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                else
                    recin1 += ((double)b[playhead * pchans]) * overdubamp;
                
                if (recordhead < 0) {
                    recordhead = playhead;
                    pokesteps = 0.0;
                    recplaydif = writeval1 = 0.0;
                }
                
                if (recordhead == playhead) {
                    writeval1 += recin1;
                    pokesteps += 1.0;
                } else {
                    if (pokesteps > 1.0) {                          // linear-averaging for speed < 1x
                        writeval1 = writeval1 / pokesteps;
                        pokesteps = 1.0;
                    }
                    b[recordhead * pchans] = writeval1;
                    recplaydif = (double)(playhead - recordhead);   // linear-interp for speed > 1x
                    if (direction != directionorig)
                    {
                        if (directionorig >= 0)
                        {
                            if (recplaydif > 0)
                            {
                                if (recplaydif > (maxhead * 0.5))
                                {
                                    recplaydif -= maxhead;
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead - 1); i >= 0; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                    for (i = maxhead; i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead + 1); i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                }
                            } else {
                                if ((-recplaydif) > (maxhead * 0.5))
                                {
                                    recplaydif += maxhead;
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                    for (i = 0; i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead - 1); i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                {
                                    recplaydif -= ((frames - 1) - (maxhead));
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead - 1); i >= maxhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                    for (i = (frames - 1); i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead + 1); i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                }
                            } else {
                                if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                {
                                    recplaydif += ((frames - 1) - (maxhead));
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead + 1); i < frames; i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                    for (i = maxhead; i < playhead; i++) {
                                        writeval1 += coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                } else {
                                    coeff1 = (recin1 - writeval1) / recplaydif;
                                    for (i = (recordhead - 1); i > playhead; i--) {
                                        writeval1 -= coeff1;
                                        b[i * pchans] = writeval1;
                                    }
                                }
                            }
                        }
                    } else {
                        if (recplaydif > 0)
                        {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = (recordhead + 1); i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * pchans] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = (recordhead - 1); i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * pchans] = writeval1;
                            }
                        }
                    }
                    writeval1 = recin1;
                }                           // ~ipoke end
                if (globalramp)             // realtime ramps for record on/off
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
                            //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                  // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    recordfade = loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                              break;                      // !! no break - pass 1 -> 2 !!
                            case 2:
                                //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                record = loopdetermine = 0;
                                triginit = 1;
                                break;
                            case 3:
                                //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                record = triginit = 1;
                                loopdetermine = 0;
                                break;
                            case 4:
                                //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                recendmark = 0;
                                break;
                        }
                    }
                }               //
                recordhead = playhead;
                dirt = 1;
                //initialhigh = maxloop;
            }
            directionprev = direction;
        }
        if (ovdbdif != 0.0)
            overdubamp = overdubamp + ovdbdif;

        initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
    }
    
    if (dirt) {                 // notify other buf-related jobs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {                           // list-outlet stuff
        clock_delay(x->tclock, 0);              // why ??
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) { // why '!go' ??
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
    x->recordhead       = recordhead;
    x->alternateflag    = alternateflag;
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
    x->minloop          = minloop;
    x->maxloop          = maxloop;
    x->initiallow       = initiallow;
    x->initialhigh      = initialhigh;
    x->loopdetermine    = loopdetermine;
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
    long    syncoutlet = x->syncoutlet;
    
    double *in1 = ins[0];       // L
    double *in2 = ins[1];       // R
    double *in3 = ins[2];                       // speed (if signal connected)
    
    double *out1  = outs[0];    // L
    double *out2  = outs[1];    // R
    double *outPh = syncoutlet ? outs[2] : 0;   // sync (if @syncout 1)
    
    long    n = vcount;
    short   speedinlet  = x->speedconnect;
    
    double accuratehead, maxhead, jumphead, srscale, speedsrscaled, recplaydif, pokesteps;
    double speed, speedfloat, osamp1, osamp2, overdubamp, overdubprev, ovdbdif, selstart, selection;
    double o1prev, o2prev, o1dif, o2dif, frac, snrfade, globalramp, snrramp, writeval1, writeval2, coeff1, coeff2, recin1, recin2;
    t_bool go, record, recordprev, alternateflag, loopdetermine, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, pchans, snrtype, interp;
    t_ptr_int frames, startloop, endloop, playhead, recordhead, minloop, maxloop, setloopsize;
    t_ptr_int initiallow, initialhigh;

    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->k_ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modify(x, buf);
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
    recordhead      = x->recordhead;
    alternateflag   = x->alternateflag;
    pchans          = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    minloop         = x->minloop;
    maxloop         = x->maxloop;
    initiallow      = x->initiallow;
    initialhigh     = x->initialhigh;
    selection       = x->selection;
    loopdetermine   = x->loopdetermine;
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
    speedfloat      = x->speedfloat;
    
    switch (statecontrol)   // "all-in-one 'switch' statement to catch and handle all(most) messages" - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record initial loop
            record = go = triginit = loopdetermine = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record alternateflag (wtf is 'alternateflag' ('rectoo') ?!)  // in to OVERDUB ??
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off regular
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // out of OVERDUB ??
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on regular
            triginit = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // after OVERDUB ??
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop regular
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
            playfadeflag = 4;   // !! modified in perform loop switch case(s) for playing behind append
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append (what is special about it ?!)   // in to RECORD / OVERDUB ??
            record = loopdetermine = alternateflag = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on regular (when ?! not looped ?!)
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
    // 'recordhead = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)
    
    if (pchans > 1)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = speedinlet ? *in3++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * pchans) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    recordhead = playhead;
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
            // !! is 'loopdetermine' !!

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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
                    apned:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
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
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * pchans) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        recplaydif = (double)(playhead - recordhead);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
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
//                                      break;                      // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                      // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
        }

    }
    else
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            speed = speedinlet ? *in3++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                    
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * pchans] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * pchans] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    recordhead = playhead;
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
            // !! is 'loopdetermine' !!

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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apnde:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
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
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                    
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        recplaydif = (double)(playhead - recordhead);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    b[i * pchans] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    b[i * pchans] = writeval1;
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
//                                      break;                      // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                      // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
        }
    
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {                           // list-outlet stuff
        clock_delay(x->tclock, 0);              // why ??
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) { // why '!go' ??
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
    x->recordhead       = recordhead;
    x->alternateflag    = alternateflag;
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
    x->minloop          = minloop;
    x->initiallow       = initiallow;
    x->initialhigh      = initialhigh;
    x->loopdetermine    = loopdetermine;
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
    long    syncoutlet = x->syncoutlet;
    
    double *in1 = ins[0];   // channel 1
    double *in2 = ins[1];   // channel 2
    double *in3 = ins[2];   // channel 3
    double *in4 = ins[3];   // channel 4
    double *in5 = ins[4];                       // speed (if signal connected)
    
    double *out1  = outs[0];// channel 1
    double *out2  = outs[1];// channel 2
    double *out3  = outs[2];// channel 3
    double *out4  = outs[3];// channel 4
    double *outPh = syncoutlet ? outs[4] : 0;   // sync (if @syncout 1)
    
    long    n = vcount;
    short   speedinlet  = x->speedconnect;
    
    double accuratehead, maxhead, jumphead, srscale, speedsrscaled, recplaydif, pokesteps;
    double speed, speedfloat, osamp1, osamp2, osamp3, osamp4, overdubamp, overdubprev, ovdbdif, selstart, selection;
    double o1prev, o2prev, o1dif, o2dif, o3prev, o4prev, o3dif, o4dif, frac, snrfade, globalramp, snrramp;
    double writeval1, writeval2, writeval3, writeval4, coeff1, coeff2, coeff3, coeff4, recin1, recin2, recin3, recin4;
    t_bool go, record, recordprev, alternateflag, loopdetermine, jumpflag, append, dirt, wrapflag, triginit;
    char direction, directionprev, directionorig, statecontrol, playfadeflag, recfadeflag, recendmark;
    t_ptr_int playfade, recordfade, i, interp0, interp1, interp2, interp3, pchans, snrtype, interp;
    t_ptr_int frames, startloop, endloop, playhead, recordhead, minloop, maxloop, setloopsize;
    t_ptr_int initiallow, initialhigh;

    t_buffer_obj *buf = buffer_ref_getobject(x->buf);
    float *b = buffer_locksamples(buf);
    
    record          = x->record;
    recordprev      = x->recordprev;
    dirt            = 0;
    if (!b || x->k_ob.z_disabled)
        goto zero;
    if (record || recordprev)
        dirt        = 1;
    if (x->buf_modified) {
        karma_buf_modify(x, buf);
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
    recordhead      = x->recordhead;
    alternateflag   = x->alternateflag;
    pchans          = x->bchans;
    srscale         = x->srscale;
    frames          = x->bframes;
    triginit        = x->triginit;
    jumpflag        = x->jumpflag;
    append          = x->append;
    directionorig   = x->directionorig;
    directionprev   = x->directionprev;
    minloop         = x->minloop;
    maxloop         = x->maxloop;
    initiallow      = x->initiallow;
    initialhigh     = x->initialhigh;
    selection       = x->selection;
    loopdetermine   = x->loopdetermine;
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
    speedfloat      = x->speedfloat;
    
    switch (statecontrol)   // "all-in-one 'switch' statement to catch and handle all(most) messages" - raja
    {
        case 0:             // case 0: zero
            break;
        case 1:             // case 1: record initial loop
            record = go = triginit = loopdetermine = 1;
            recordfade = recfadeflag = playfade = playfadeflag = statecontrol = 0;
            break;
        case 2:             // case 2: record alternateflag (wtf is 'alternateflag' ('rectoo') ?!)  // in to OVERDUB ??
            recendmark = 3;
            record = recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 3:             // case 3: record off regular
            recfadeflag = 1;
            playfadeflag = 3;
            playfade = recordfade = statecontrol = 0;
            break;
        case 4:             // case 4: play alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // out of OVERDUB ??
            recendmark = 2;
            recfadeflag = playfadeflag = 1;
            playfade = recordfade = statecontrol = 0;
            break;
        case 5:             // case 5: play on regular
            triginit = 1;
            statecontrol = 0;
            break;
        case 6:             // case 6: stop alternateflag (wtf is 'alternateflag' ('rectoo') ?!)    // after OVERDUB ??
            playfade = recordfade = 0;
            recendmark = playfadeflag = recfadeflag = 1;
            statecontrol = 0;
            break;
        case 7:             // case 7: stop regular
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
            playfadeflag = 4;   // !! modified in perform loop switch case(s) for playing behind append
            playfade = 0;
            statecontrol = 0;
            break;
        case 10:            // case 10: special case append (what is special about it ?!)   // in to RECORD / OVERDUB ??
            record = loopdetermine = alternateflag = 1;
            snrfade = 0.0;
            recordfade = recfadeflag = statecontrol = 0;
            break;
        case 11:            // case 11: record on regular (when ?! not looped ?!)
            playfadeflag = 3;
            recfadeflag = 5;
            recordfade = playfade = statecontrol = 0;
            break;          // !!
    }
    
    // raja notes:
    // 'snrfade = 0.0' triggers switch&ramp (declick play)
    // 'recordhead = -1' triggers ipoke-interp cuts and accompanies buf~ fades (declick record)
    
    if (pchans >= 4)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = speedinlet ? *in5++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                        osamp3 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2]);
                        osamp4 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 3], b[(interp2 * pchans) + 3]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                            osamp3  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 2], b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2], b[(interp3 * pchans) + 2]);
                            osamp4  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 3], b[(interp1 * pchans) + 3], b[(interp2 * pchans) + 3], b[(interp3 * pchans) + 3]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                            osamp3  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 2], b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2], b[(interp3 * pchans) + 2]);
                            osamp4  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 3], b[(interp1 * pchans) + 3], b[(interp2 * pchans) + 3], b[(interp3 * pchans) + 3]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                            osamp3  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2]);
                            osamp4  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 3], b[(interp2 * pchans) + 3]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * pchans) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + (((double)b[(playhead * pchans) + 2]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin4 = ease_record(recin4 + (((double)b[(playhead * pchans) + 3]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * pchans) + 2]) * overdubamp;
                        recin4 += ((double)b[(playhead * pchans) + 3]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
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
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        b[(recordhead * pchans) + 2] = writeval3;
                        b[(recordhead * pchans) + 3] = writeval4;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            coeff3 = (recin3 - writeval3) / recplaydif;
                            coeff4 = (recin4 - writeval4) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                writeval3 += coeff3;
                                writeval4 += coeff4;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                                b[(i * pchans) + 2] = writeval3;
                                b[(i * pchans) + 3] = writeval4;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            coeff3 = (recin3 - writeval3) / recplaydif;
                            coeff4 = (recin4 - writeval4) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                writeval3 -= coeff3;
                                writeval4 -= coeff4;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                                b[(i * pchans) + 2] = writeval3;
                                b[(i * pchans) + 3] = writeval4;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                        writeval4 = recin4;
                    }
                    recordhead = playhead;
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
            // !! is 'loopdetermine' !!

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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apned;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
                    apned:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
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
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * pchans) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + ((double)b[(playhead * pchans) + 2]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin4 = ease_record(recin4 + ((double)b[(playhead * pchans) + 3]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * pchans) + 2]) * overdubamp;
                        recin4 += ((double)b[(playhead * pchans) + 3]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = writeval3 = writeval4 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
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
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        b[(recordhead * pchans) + 2] = writeval3;
                        b[(recordhead * pchans) + 3] = writeval4;
                        recplaydif = (double)(playhead - recordhead);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            writeval4 += coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        coeff4 = (recin4 - writeval4) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            writeval4 -= coeff4;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                            b[(i * pchans) + 3] = writeval4;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                coeff3 = (recin3 - writeval3) / recplaydif;
                                coeff4 = (recin4 - writeval4) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    writeval3 += coeff3;
                                    writeval4 += coeff4;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                    b[(i * pchans) + 2] = writeval3;
                                    b[(i * pchans) + 3] = writeval4;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                coeff3 = (recin3 - writeval3) / recplaydif;
                                coeff4 = (recin4 - writeval4) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    writeval3 -= coeff3;
                                    writeval4 -= coeff4;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                    b[(i * pchans) + 2] = writeval3;
                                    b[(i * pchans) + 3] = writeval4;
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
//                                      break;                      // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                      // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
        }
        
    }
    else if (pchans == 3)
    {

        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = speedinlet ? *in5++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                        osamp3 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                            osamp3  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 2], b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2], b[(interp3 * pchans) + 2]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                            osamp3  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 2], b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2], b[(interp3 * pchans) + 2]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                            osamp3  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 2], b[(interp2 * pchans) + 2]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                 */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * pchans) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + (((double)b[(playhead * pchans) + 2]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * pchans) + 2]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
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
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        b[(recordhead * pchans) + 2] = writeval3;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            coeff3 = (recin3 - writeval3) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                writeval3 += coeff3;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                                b[(i * pchans) + 2] = writeval3;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            coeff3 = (recin3 - writeval3) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                writeval3 -= coeff3;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                                b[(i * pchans) + 2] = writeval3;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                        writeval3 = recin3;
                    }
                    recordhead = playhead;
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
            // !! is 'loopdetermine' !!

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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apden;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apden:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
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
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * pchans) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin3 = ease_record(recin3 + ((double)b[(playhead * pchans) + 2]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                        recin3 += ((double)b[(playhead * pchans) + 2]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = writeval3 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
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
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        b[(recordhead * pchans) + 2] = writeval3;
                        recplaydif = (double)(playhead - recordhead);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            writeval3 += coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        coeff3 = (recin3 - writeval3) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            writeval3 -= coeff3;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                            b[(i * pchans) + 2] = writeval3;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                coeff3 = (recin3 - writeval3) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    writeval3 += coeff3;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                    b[(i * pchans) + 2] = writeval3;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                coeff3 = (recin3 - writeval3) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    writeval3 -= coeff3;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                    b[(i * pchans) + 2] = writeval3;
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
//                                      break;              // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                  // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
        }

    }
    else if (pchans == 2)
    {
        while (n--)
        {
            recin1 = *in1++;
            recin2 = *in2++;
            recin3 = *in3++;
            recin4 = *in4++;
            speed = speedinlet ? *in5++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                        osamp2 =    LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1) {
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = CUBIC_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                        } else if (interp == 2) {
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                            osamp2  = SPLINE_INTERP(frac, b[(interp0 * pchans) + 1], b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1], b[(interp3 * pchans) + 1]);
                        } else {
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                            osamp2  = LINEAR_INTERP(frac, b[(interp1 * pchans) + 1], b[(interp2 * pchans) + 1]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + (((double)b[(playhead * pchans) + 1]) * overdubamp), recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                writeval2 += coeff2;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            coeff2 = (recin2 - writeval2) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                writeval2 -= coeff2;
                                b[i * pchans] = writeval1;
                                b[(i * pchans) + 1] = writeval2;
                            }
                        }
                        writeval1 = recin1;
                        writeval2 = recin2;
                    }
                    recordhead = playhead;
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
            // !! is 'loopdetermine' !!

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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apdne;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apdne:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)                    // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
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
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0)) {
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                        recin2 = ease_record(recin2 + ((double)b[(playhead * pchans) + 1]) * overdubamp, recfadeflag, globalramp, recordfade);
                    } else {
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                        recin2 += ((double)b[(playhead * pchans) + 1]) * overdubamp;
                    }
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = writeval2 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        writeval2 += recin2;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                  // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            writeval2 = writeval2 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        b[(recordhead * pchans) + 1] = writeval2;
                        recplaydif = (double)(playhead - recordhead);        // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            writeval2 += coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        coeff2 = (recin2 - writeval2) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            writeval2 -= coeff2;
                                            b[i * pchans] = writeval1;
                                            b[(i * pchans) + 1] = writeval2;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    writeval2 += coeff2;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                coeff2 = (recin2 - writeval2) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    writeval2 -= coeff2;
                                    b[i * pchans] = writeval1;
                                    b[(i * pchans) + 1] = writeval2;
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
//                                      break;                  // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                      // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }                           // ~ipoke end
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
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
            speed = speedinlet ? *in5++ : speedfloat;   // signal of float ?
            direction = (speed > 0) ? 1 : ((speed < 0) ? -1 : 0);
            
            // declick for change of 'dir'ection
            if (directionprev != direction) {
                if (record && globalramp) {
                    ease_bufoff(frames - 1, b, pchans, recordhead, -direction, globalramp);
                    recordfade = recfadeflag = 0;
                    recordhead = -1;
                }
                snrfade = 0.0;
            }   // !! !!
            
            if ((record - recordprev) < 0) {           // samp @record-off
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, recordhead, direction, globalramp);
                //initialhigh = loopdetermine ? recordhead : initialhigh;
                recordhead = -1;
                dirt = 1;
            } else if ((record - recordprev) > 0) {    // samp @record-on
                recordfade = recfadeflag = 0;
                if (speed < 1.0)
                    snrfade = 0.0;
                if (globalramp)
                    ease_bufoff(frames - 1, b, pchans, accuratehead, -direction, globalramp);
            }
            recordprev = record;
            
            if (!loopdetermine)
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
                                setloopsize = maxloop - minloop;
                                accuratehead = startloop = minloop + (selstart * setloopsize);
                                endloop = startloop + (selection * setloopsize);
                                if (endloop > maxloop) {
                                    endloop = endloop - (setloopsize + 1);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                if (direction < 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            } else {
                                maxloop = CLAMP((frames - 1) - maxhead, 4096, frames - 1);
                                setloopsize = maxloop - minloop;    // ((frames - 1) - setloopsize - minloop)   // ??
                                startloop = ((frames - 1) - setloopsize) + (selstart * setloopsize);    // ((frames - 1) - maxloop) + (selstart * maxloop);   // ??
                                accuratehead = endloop = startloop + (selection * setloopsize);         // startloop + (selection * maxloop);   ??
                                if (endloop > (frames - 1)) {
                                    endloop = ((frames - 1) - setloopsize) + (endloop - frames);
                                    wrapflag = 1;
                                } else {
                                    wrapflag = 0;
                                }
                                accuratehead = endloop;
                                if (direction > 0) {
                                    if (globalramp)
                                        ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                }
                            }
                            if (globalramp)
                                ease_bufoff(frames - 1, b, pchans, maxhead, -direction, globalramp);
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                            append = alternateflag = recendmark = 0;
                        } else {    // jump / play (inside 'window')
                            setloopsize = maxloop - minloop;
                            if (jumpflag)
                                accuratehead = (directionorig >= 0) ? ((jumphead * setloopsize) + minloop) : (((frames - 1) - maxloop) + (jumphead * setloopsize));
                            else
                                accuratehead = (direction < 0) ? endloop : startloop;
                            if (record) {
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recordhead = -1;
                                recfadeflag = 0;
                            }
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {        // jump-based constraints (outside 'window')
                        setloopsize = maxloop - minloop;
                        speedsrscaled = speed * srscale;
                        
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (setloopsize / 1024)) ? ((setloopsize / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        
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
                                    accuratehead = accuratehead - setloopsize;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < 0.0) {
                                    accuratehead = maxloop + accuratehead;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                }
                            } else {
                                if (accuratehead > (frames - 1))
                                {
                                    accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...((frames - 1) - maxloop)...   // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (accuratehead < ((frames - 1) - maxloop)) {
                                    accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...((frames - 1) - maxloop)... // ??
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
                                    }
                                } else if (directionorig >= 0) {
                                    if (accuratehead > maxloop)
                                    {
                                        accuratehead = accuratehead - setloopsize;  // fixed position ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, maxloop, -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                    else if (accuratehead < 0.0)
                                    {
                                        accuratehead = maxloop + setloopsize;       // !! this is surely completely wrong ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, minloop, -direction, globalramp);     // 0.0  // ??
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                } else {    // reverse
                                    if (accuratehead < ((frames - 1) - maxloop))
                                    {
                                        accuratehead = (frames - 1) - (((frames - 1) - setloopsize) - accuratehead);    // ...- maxloop)... // ??
                                        snrfade = 0.0;
                                        if (record)
                                        {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, ((frames - 1) - maxloop), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    } else if (accuratehead > (frames - 1)) {
                                        accuratehead = ((frames - 1) - setloopsize) + (accuratehead - (frames - 1));    // ...- maxloop)...   // ??
                                        snrfade = 0.0;
                                        if (record) {
                                            if (globalramp) {
                                                ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                                recordfade = 0;
                                            }
                                            recfadeflag = 0;
                                            recordhead = -1;
                                        }
                                    }
                                }
                            } else {    // (not wrapflag)
                                if ((accuratehead > endloop) || (accuratehead < startloop))
                                {
                                    accuratehead = (direction >= 0) ? startloop : endloop;
                                    snrfade = 0.0;
                                    if (record) {
                                        if (globalramp) {
                                            ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                            recordfade = 0;
                                        }
                                        recfadeflag = 0;
                                        recordhead = -1;
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
                    }                                                                                   // setloopsize  // ??
                    interp_index(playhead, &interp0, &interp1, &interp2, &interp3, direction, directionorig, maxloop, frames - 1);  // samp-indices for interp
                    
                    if (record) {              // if recording do linear-interp else...
                        osamp1 =    LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
                    } else {                // ...cubic / spline if interpflag > 0 (default cubic)
                        if (interp == 1)
                            osamp1  = CUBIC_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                        else if (interp == 2)
                            osamp1  = SPLINE_INTERP(frac, b[interp0 * pchans], b[interp1 * pchans], b[interp2 * pchans], b[interp3 * pchans]);
                        else
                            osamp1  = LINEAR_INTERP(frac, b[interp1 * pchans], b[interp2 * pchans]);
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
                                        playfadeflag = go = 0;  // record alternateflag   // play alternateflag  // stop alternateflag / regular
                                        break;
                                    case 2:
                                        if (!record)
                                            triginit = jumpflag = 1;
//                                      break;                  // !! no break - pass 2 -> 3 !!
                                    case 3:                     // jump // record off reg
                                        playfadeflag = playfade = 0;
                                        break;
                                    case 4:                     // append
                                        go = triginit = loopdetermine = 1;
                                        // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
//                              break;                                  // !! no break - pass 2 -> 3 !!
                            case 3:                                     // jump     // record off reg
                                playfadeflag = 0;
                                break;
                            case 4:                                     // append
                                go = triginit = loopdetermine = 1;
                                // !! will disabling this enable play behind append ?? should this be based on passing previous maxloop ??
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                /*
                 ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                 (modded to allow for 'window' and 'position' to change on the fly)
                 raja's razor: simplest answer to everything was:
                 recin1 = ease_record(recin1 + (b[playhead * pchans] * overdubamp), recfadeflag, globalramp, recordfade); ...
                 ... placed at the beginning / input of ipoke~ code to apply appropriate ramps to oldbuf + newinput (everything all-at-once) ...
                 ... allows ipoke~ code to work its sample-specific math / magic accurately through the ducking / ramps even at high speed
                */
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + (((double)b[playhead * pchans]) * overdubamp), recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                    
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {              // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        recplaydif = (double)(playhead - recordhead);
                        if (recplaydif > 0) {                     // linear-interpolation for speed > 1x
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = recordhead + 1; i < playhead; i++) {
                                writeval1 += coeff1;
                                b[i * pchans] = writeval1;
                            }
                        } else {
                            coeff1 = (recin1 - writeval1) / recplaydif;
                            for (i = recordhead - 1; i > playhead; i--) {
                                writeval1 -= coeff1;
                                b[i * pchans] = writeval1;
                            }
                        }
                        writeval1 = recin1;
                    }
                    recordhead = playhead;
                    dirt = 1;
                }                                           // ~ipoke end
                
                if (globalramp)                             // realtime ramps for record on/off
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
            // !! is 'loopdetermine' !!

                if (go)
                {
                    if (triginit)
                    {
                        if (jumpflag)                       // jump
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
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                recfadeflag = 0;
                                recordhead = -1;
                            }
                            triginit = 0;
                        } else if (append) {                // append
                            snrfade = 0.0;
                            triginit = 0;
                            if (record)
                            {
                                accuratehead = maxhead;
                                if (globalramp) {
                                    ease_bufon(frames - 1, b, pchans, accuratehead, recordhead, direction, globalramp);
                                    recordfade = 0;
                                }
                                alternateflag = 1;
                                recfadeflag = 0;
                                recordhead = -1;
                            } else {
                                goto apnde;
                            }
                        } else {                            // trigger start of initial loop creation
                            directionorig = direction;
                            minloop = 0.0;
                            maxloop = frames - 1;
                            maxhead = accuratehead = (direction >= 0) ? minloop : maxloop;     // (direction >= 0) ? 0.0 : (frames - 1);
                            alternateflag = 1;
                            recordhead = -1;
                            snrfade = 0.0;
                            triginit = 0;
                        }
                    } else {
apnde:
                        speedsrscaled = speed * srscale;
                        if (record)
                            speedsrscaled = (fabs(speedsrscaled) > (maxloop / 1024)) ? ((maxloop / 1024) * direction) : speedsrscaled;
                        accuratehead = accuratehead + speedsrscaled;
                        if (direction == directionorig)     // buffer~ boundary constraints and registry of maximum distance traversed
                        {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = 0.0;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = frames - 1;
                            } else if (accuratehead < 0.0) {
                                accuratehead = frames - 1;
                                record = append;
                                if (record) {
                                    if (globalramp) {
                                        ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                        recordhead = -1;
                                        recfadeflag = recordfade = 0;
                                    }
                                }
                                recendmark = triginit = 1;
                                loopdetermine = alternateflag = 0;
                                maxhead = 0.0;
                            } else {                        // <- track max write position
                                if ( ((directionorig >= 0) && (maxhead < accuratehead)) || ((directionorig < 0) && (maxhead > accuratehead)) )
                                    maxhead = accuratehead;
                            }
                        } else if (direction < 0) {         // wraparounds for reversal while creating initial-loop
                            if (accuratehead < 0.0)
                            {
                                accuratehead = maxhead + accuratehead;
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, 0.0, -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        } else if (direction >= 0) {
                            if (accuratehead > (frames - 1))
                            {
                                accuratehead = maxhead + (accuratehead - (frames - 1));
                                if (globalramp) {
                                    ease_bufoff(frames - 1, b, pchans, (frames - 1), -direction, globalramp);
                                    recordhead = -1;
                                    recfadeflag = recordfade = 0;
                                }
                            }
                        }
                    //initialhigh = append ? initialhigh : maxhead;   // !! !!
                    }
                    
                    playhead = trunc(accuratehead);
                    if (direction > 0) {                    // interp ratio
                        frac = accuratehead - playhead;
                    } else if (direction < 0) {
                        frac = 1.0 - (accuratehead - playhead);
                    } else {
                        frac = 0.0;
                    }
                    
                    if (globalramp)
                    {
                        if (playfade < globalramp)          // realtime ramps for play on/off
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
                if (syncoutlet) {
                    setloopsize = maxloop-minloop;
                    *outPh++    = (directionorig>=0) ? ((accuratehead-minloop)/setloopsize) : ((accuratehead-(frames-setloopsize))/setloopsize);
                }
                
                // ~ipoke - originally by PA Tremblay: http://www.pierrealexandretremblay.com/welcome.html
                // (modded to assume maximum distance recorded into buffer~ as the total length)
                if (record)
                {
                    if ((recordfade < globalramp) && (globalramp > 0.0))
                        recin1 = ease_record(recin1 + ((double)b[playhead * pchans]) * overdubamp, recfadeflag, globalramp, recordfade);
                    else
                        recin1 += ((double)b[playhead * pchans]) * overdubamp;
                    
                    if (recordhead < 0) {
                        recordhead = playhead;
                        pokesteps = 0.0;
                        recplaydif = writeval1 = 0.0;
                    }
                    
                    if (recordhead == playhead) {
                        writeval1 += recin1;
                        pokesteps += 1.0;
                    } else {
                        if (pokesteps > 1.0) {                      // linear-averaging for speed < 1x
                            writeval1 = writeval1 / pokesteps;
                            pokesteps = 1.0;
                        }
                        b[recordhead * pchans] = writeval1;
                        recplaydif = (double)(playhead - recordhead);     // linear-interp for speed > 1x
                        if (direction != directionorig)
                        {
                            if (directionorig >= 0)
                            {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (maxhead * 0.5))
                                    {
                                        recplaydif -= maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i >= 0; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = maxhead; i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (maxhead * 0.5))
                                    {
                                        recplaydif += maxhead;
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < (maxhead + 1); i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = 0; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                }
                            } else {
                                if (recplaydif > 0)
                                {
                                    if (recplaydif > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif -= ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i >= maxhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = (frames - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                } else {
                                    if ((-recplaydif) > (((frames - 1) - (maxhead)) * 0.5))
                                    {
                                        recplaydif += ((frames - 1) - (maxhead));
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead + 1); i < frames; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                        for (i = maxhead; i < playhead; i++) {
                                            writeval1 += coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    } else {
                                        coeff1 = (recin1 - writeval1) / recplaydif;
                                        for (i = (recordhead - 1); i > playhead; i--) {
                                            writeval1 -= coeff1;
                                            b[i * pchans] = writeval1;
                                        }
                                    }
                                }
                            }
                        } else {
                            if (recplaydif > 0)
                            {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                for (i = (recordhead + 1); i < playhead; i++) {
                                    writeval1 += coeff1;
                                    b[i * pchans] = writeval1;
                                }
                            } else {
                                coeff1 = (recin1 - writeval1) / recplaydif;
                                for (i = (recordhead - 1); i > playhead; i--) {
                                    writeval1 -= coeff1;
                                    b[i * pchans] = writeval1;
                                }
                            }
                        }
                        writeval1 = recin1;
                    }                                           // ~ipoke end
                    if (globalramp)                             // realtime ramps for record on/off
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
//                                      break;                  // !! no break - pass 1 -> 2 !!
                                    case 2:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = loopdetermine = 0;
                                        triginit = 1;
                                        break;
                                    case 3:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                        record = triginit = 1;
                                        recordfade = loopdetermine = 0;
                                        break;
                                    case 4:
                                        //initial_points(minloop, maxloop, &initiallow, &initialhigh);
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
//                                  break;                      // !! no break - pass 1 -> 2 !!
                                case 2:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = loopdetermine = 0;
                                    triginit = 1;
                                    break;
                                case 3:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    record = triginit = 1;
                                    loopdetermine = 0;
                                    break;
                                case 4:
                                    //initial_points(minloop, maxloop, &initiallow, &initialhigh);
                                    recendmark = 0;
                                    break;
                            }
                        }
                    }
                    recordhead = playhead;
                    dirt = 1;
                    //initialhigh = maxloop;
                }
                directionprev = direction;
            }
            if (ovdbdif != 0.0)
                overdubamp = overdubamp + ovdbdif;

            initialhigh = (dirt) ? maxloop : initialhigh;  // recordhead ??
        }
        
    }
    
    if (dirt) {                 // notify other buf-related objs of write
        buffer_setdirty(buf);
    }
    buffer_unlocksamples(buf);
    
    if (x->clockgo) {                           // list-outlet stuff
        clock_delay(x->tclock, 0);              // why ??
        x->clockgo  = 0;
    } else if ((!go) || (x->reportlist <= 0)) { // why '!go' ??
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
    x->recordhead       = recordhead;
    x->alternateflag    = alternateflag;
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
    x->minloop          = minloop;
    x->initiallow       = initiallow;
    x->initialhigh      = initialhigh;
    x->loopdetermine    = loopdetermine;
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


