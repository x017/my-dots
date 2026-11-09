/* See LICENSE file for copyright and license details. */

/* appearance */
static const unsigned int borderpx = 1; /* border pixel of windows */
static const unsigned int snap = 32;    /* snap pixel */
static const int showbar = 0;           /* 0 means no bar */
static const int topbar = 1;            /* 0 means bottom bar */
static const int startwithgaps = 1;     /* 1 means gaps are used by default */
static const unsigned int gappx =
    10; /* default gap between windows in pixels */
static const char *fonts[] = {
    "monospace:size=10",
    "Symbols Nerd Font:size=11", /* icon glyphs for the tags */
};

/* systray */
static const unsigned int systraypinning =
    0; /* 0: sloppy systray follows selected monitor, >0: pin systray to monitor
          X */
static const unsigned int systrayonleft =
    0; /* 0: systray in the right corner, >0: systray on left of status text */
static const unsigned int systrayspacing = 2; /* systray spacing */
static const int systraypinningfailfirst =
    1; /* 1: if pinning fails, display systray on the first monitor, 0: on the
          last monitor */
static const int showsystray = 1; /* 0 means no systray */

/* colors: dusk Tokyo street */
static const char dmenufont[] = "monospace:size=10";
static const char norm_fg[] = "#a3adba";
static const char norm_bg[] = "#0e0e0e";
static const char norm_border[] = "#090b0d";

static const char sel_fg[] = "#e9eff6";
static const char sel_bg[] = "#0e0e0e";
static const char sel_border[] = "#05040f";

static const char *colors[][3] = {
    /*               fg        bg        border      */
    [SchemeNorm] = {norm_fg, norm_bg, norm_border}, /* unfocused windows */
    [SchemeSel] = {sel_fg, sel_bg, sel_border},     /* focused window */
};

/* tagging (Nerd Font icons, written as \u escapes so they survive copy/paste)
 */
static const char *tags[] = {
    "\uf121", /* 1 code      */
    "\uf269", /* 2 web       */
    "\uf120", /* 3 server    */
    "\uf1c0", /* 4 database  */
    "\uf075", /* 5 chat      */
    "\uf013", /* 6 spare     */
    "\uf002", /* 7 research  */
    "\uf04b", /* 8 media     */
    "\uf07b", /* 9 files     */
};

static const Rule rules[] = {
    /* xprop(1):
     *	WM_CLASS(STRING) = instance, class
     *	WM_NAME(STRING) = title
     */
    /* class   instance                 title  tags mask  isfloating  monitor */

    /* CODE */
    {NULL, "code-oss", NULL, 1 << 0, 0, -1},
    /* WEB */
    {NULL, "firefox", NULL, 1 << 1, 0, -1},
    /* SERVER */
    {NULL, "com.mitchellh.ghostty", NULL, 1 << 2, 0, -1},
    /* DATABASE / API */
    {NULL, "bruno", NULL, 1 << 3, 0, -1},
    /* CHAT */
    {NULL, "telegram", NULL, 1 << 4, 0, -1},
    {NULL, "discord", NULL, 1 << 4, 0, -1},
    /* MEDIA */
    {NULL, "vlc", NULL, 1 << 7, 0, -1},
    /* MISC */
    {NULL, "nemo", NULL, 1 << 8, 0, -1},
    /* FLOATING (tags 0 = open on the current tag) */
    {NULL, "pavucontrol", NULL, 0, 1, -1},
    {NULL, "blueberry", NULL, 0, 1, -1},
    {NULL, "nm-connection-editor", NULL, 0, 1, -1},
};

/* layout(s) */
static const float mfact = 0.55; /* factor of master area size [0.05..0.95] */
static const int nmaster = 1;    /* number of clients in master area */
static const int resizehints =
    1; /* 1 means respect size hints in tiled resizals */
static const int lockfullscreen =
    1; /* 1 will force focus on the fullscreen window */
static const int refreshrate =
    120; /* refresh rate (per second) for client move/resize */

static const Layout layouts[] = {
    /* symbol     arrange function */
    {"[]=", tile}, /* first entry is default: master on the left */
};

/* key definitions */
#define MODKEY Mod1Mask
#define TAGKEYS(KEY, TAG)                                                      \
  {MODKEY, KEY, view, {.ui = 1 << TAG}},                                       \
      {MODKEY | ControlMask, KEY, toggleview, {.ui = 1 << TAG}},               \
      {MODKEY | ShiftMask, KEY, tag, {.ui = 1 << TAG}},                        \
      {MODKEY | ControlMask | ShiftMask, KEY, toggletag, {.ui = 1 << TAG}},

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd)                                                             \
  {                                                                            \
    .v = (const char *[]) { "/bin/sh", "-c", cmd, NULL }                       \
  }

/* commands */
static char dmenumon[2] =
    "0"; /* component of dmenucmd, manipulated in spawn() */
static const char *dmenucmd[] = {
    "dmenu_run", "-m",    dmenumon, "-fn",  dmenufont, "-nb",  norm_bg,
    "-nf",       norm_fg, "-sb",    sel_bg, "-sf",     sel_fg, NULL};
static const char *termcmd[] = {"ghostty", NULL};
static const char *roficmd[] = {"rofi", "-show", "drun", NULL};
static const char *powercmd[] = {"mel", "--power", NULL};
static const char *refreshcmd[] = {"/home/parsa/.local/bin/refreshmenu", NULL};
static const char *lockcmd[] = {"mel", NULL};
static const char *changelayoutcmd[] = {"kb-lay-switch", NULL};
static const Key keys[] = {
    /* modifier                     key        function        argument */
    {MODKEY, XK_p, spawn, {.v = roficmd}},
    {MODKEY, XK_q, spawn, {.v = termcmd}},
    {MODKEY, XK_b, togglebar, {0}},
    {MODKEY, XK_j, focusstack, {.i = +1}},
    {MODKEY, XK_k, focusstack, {.i = -1}},
    {MODKEY, XK_h, setmfact, {.f = -0.05}},
    {MODKEY, XK_l, setmfact, {.f = +0.05}},
    {MODKEY, XK_Return, zoom, {0}},
    {MODKEY, XK_Tab, view, {0}},
    {MODKEY, XK_c, killclient, {0}},
    {MODKEY, XK_space, setlayout, {0}},
    {MODKEY | ShiftMask, XK_space, togglefloating, {0}},
    {MODKEY, XK_0, view, {.ui = ~0}},
    {MODKEY | ShiftMask, XK_l, spawn, {.v = lockcmd}},
    {MODKEY | ShiftMask, XK_0, tag, {.ui = ~0}},
    {MODKEY, XK_comma, focusmon, {.i = -1}},
    {MODKEY, XK_period, focusmon, {.i = +1}},
    {MODKEY | ShiftMask, XK_comma, tagmon, {.i = -1}},
    {MODKEY | ShiftMask, XK_period, tagmon, {.i = +1}},
    {MODKEY, XK_minus, setgaps, {.i = -5}},
    {MODKEY, XK_equal, setgaps, {.i = +5}},
    {MODKEY | ShiftMask, XK_minus, setgaps, {.i = GAP_RESET}},
    {MODKEY | ShiftMask, XK_equal, setgaps, {.i = GAP_TOGGLE}},
    {MODKEY | ShiftMask, XK_r, spawn, {.v = refreshcmd}},
    {MODKEY, XK_Left, focusdir, {.i = 0}},
    {MODKEY, XK_Right, focusdir, {.i = 1}},
    {MODKEY, XK_Up, focusdir, {.i = 2}},
    {MODKEY, XK_Down, focusdir, {.i = 3}},
    {Mod4Mask, XK_space, spawn, {.v = changelayoutcmd}},
    TAGKEYS(XK_1, 0) TAGKEYS(XK_2, 1) TAGKEYS(XK_3, 2) TAGKEYS(XK_4, 3)
        TAGKEYS(XK_5, 4) TAGKEYS(XK_6, 5) TAGKEYS(XK_7, 6) TAGKEYS(XK_8, 7)
            TAGKEYS(XK_9, 8){MODKEY, XK_m, spawn, {.v = powercmd}},
};

/* button definitions */
/* click can be ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
 * ClkClientWin, or ClkRootWin */
static const Button buttons[] = {
    /* click                event mask      button          function argument */
    {ClkLtSymbol, 0, Button1, setlayout, {0}},
    {ClkWinTitle, 0, Button2, zoom, {0}},
    {ClkStatusText, 0, Button2, spawn, {.v = termcmd}},
    {ClkClientWin, MODKEY, Button1, movemouse, {0}},
    {ClkClientWin, MODKEY, Button2, togglefloating, {0}},
    {ClkClientWin, MODKEY, Button3, resizemouse, {0}},
    {ClkTagBar, 0, Button1, view, {0}},
    {ClkTagBar, 0, Button3, toggleview, {0}},
    {ClkTagBar, MODKEY, Button1, tag, {0}},
    {ClkTagBar, MODKEY, Button3, toggletag, {0}},
};
